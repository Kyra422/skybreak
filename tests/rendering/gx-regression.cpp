// LIGHT7 renderer unit tests: TEV last-stage output, indirect texturing (the EGG
// light-texture configuration), XF colour-channel lighting parity with Dolphin
// (separate alpha channel, numColorChans, colour texgens from the lit colour) and
// the zero rasterized colour. Drives the real GL renderer (renderer_gl.cpp through
// IRenderer, tev_shader_gen.cpp) with hand-built GX command lists on a hidden SDL
// GL 3.3 core context and a 640x528 EFB FBO (harness as gxseam/texgen-test.cpp).
// Every feature has a control that must give the same result before and after.
//
//   T1 D1: stage 1 writes C2 (red), stage 0 PREV (green)  -> red (last stage on screen)
//   T2 control: stage 1 writes PREV                         -> red
//   I1 indirect, EGG normal-sphere setup: ITF_8, bias STU, matrix 0 = (0.5,0,0) exp 0,
//      wrap ITW_0, TEXCOORD for the stage wrapped to 0; ind map = RGBA8 16x8 whose
//      alpha (S) is 139 left / 249 right -> s = 5.5 / 60.5 texels in a 64x4 I8 ramp
//      (value 4*x, nearest, repeat)                          -> 20 / 240
//   I2 negative S: alpha 117 -> s = -5.5 -> repeat texel 58  -> 232
//   I3 matrix scale exponent -1                             -> s = 2.75 / 30.25 -> 8 / 120
//   I4 control: IND_CMD = 0: plain lookup; texcoord scale 16 on the 64-wide map (TEX7, Dolphin
//      fixpoint uv * scale / size) -> texels 4 / 12 -> 16 / 48
//   L1 alpha channel has its own light mask: colour lit by light 0 (red), alpha lit
//      with mask 0 and ambient alpha 0x80                     -> rgb 255,0,0 a 128
//   L2 control: alpha channel ctrl = colour ctrl            -> a 255
//   N1 XF numColorChans = 0: rasterized colour = vertex colour (green), not the
//      lit/material register value (black)                   -> green
//   N2 control: numColorChans = 1                           -> black
//   C1 colour texgen (type COLOR0) takes the LIT colour (R 0.75) not the vertex colour
//      (R 0.25): s = 0.75 in the halves texture              -> 255
//   C2 control: channel unlit, material = vertex colour     -> 0
//   Z1 TEV order colour channel ZERO: RASA = 0              -> alpha 0
//   Z2 control: COLOR0A0 with vertex alpha 1                -> alpha 255
//   S1 TEV tex swap table 1 = (A,R,G,R) on an RGBA8 map (77,128,128,10|200)
//                                                           -> rgba 10|200,77,128,77
//   S2 control: tex swap table 0 = identity                 -> 77,128,128,10|200
//   S3 TEV ras swap table 2 = (B,G,R,A), vertex colour (64,128,191,255) -> 191,128,64,255
//   S4 control: ras swap table 0                            -> 64,128,191,255
// BaseState writes the GXInit swap tables (BP 0xF6-0xFD); all-zero registers would
// mean table 0 = RRRR, as in Dolphin.
#include <SDL.h>
#include <glad/glad.h>

#include "runtime/cpu_context.h"
#include "runtime/gx/fifo_parser.h"
#include "runtime/gx/gx_capture.h"
#include "runtime/gx/renderer.h"
#include "runtime/gx/interp_render.h"
#include "runtime/gx_state.h"
#include "runtime/texture_cache_gl.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace nwii::runtime {
MMU *g_mmu = nullptr;
CPUContext *g_ctx_ptr = nullptr;
uint16_t HW_Reg_Read16(uint32_t) { return 0; }
uint32_t HW_Reg_Read32(uint32_t) { return 0; }
void HW_Reg_Write16(uint32_t, uint16_t) {}
void HW_Reg_Write32(uint32_t, uint32_t) {}
uint32_t g_watch_addr = 0;
void watch_hit(uint32_t, uint32_t, int) {}
namespace hw {
void pe_signal_token(uint32_t, bool) {}
void pe_signal_finish() {}
void pi_fifo_get(uint32_t &base, uint32_t &end, uint32_t &wptr) { base = end = wptr = 0; }
void pi_fifo_set_wptr(uint32_t) {}
uint32_t cp_fifo_base_reg() { return 0; }
} // namespace hw
} // namespace nwii::runtime
void GX_SetWindow(void *window);

namespace gx = nwii::runtime::gx;
using gx::GXCommand;
using gx::GXCommandType;

static int checks = 0, fails = 0;
static void check(bool ok, const char *label) {
  ++checks;
  if (!ok) {
    ++fails;
    std::fprintf(stderr, "FAIL: %s\n", label);
  }
}

static const int EW = gx::kEfbWidth, EH = gx::kEfbHeight;
static GLuint s_fbo = 0;
static std::unique_ptr<gx::IRenderer> s_r;
static const uint32_t kHalves = 0x00400000; // I8 16x8: x<8 = 0, x>=8 = 255
static const uint32_t kRamp = 0x00410000;   // I8 64x4: value 4*x
static const uint32_t kInd = 0x00420000;    // RGBA8 16x8 indirect map

struct Quad {
  float rgba[4] = {1, 1, 1, 1};
  float nrm[3] = {0, 0, 1};
  bool has_norm = false;
};

struct Cmds {
  std::vector<GXCommand> v;
  void bp(uint8_t reg, uint32_t val) {
    GXCommand c{};
    c.type = GXCommandType::BPRegister;
    c.reg = reg;
    c.val = val & 0xFFFFFF;
    v.push_back(c);
  }
  void xf(uint16_t reg, std::vector<float> p) {
    GXCommand c{};
    c.type = GXCommandType::XFRegister;
    c.reg = reg;
    c.val = 0;
    c.length = uint16_t(p.size() - 1);
    c.payload = std::move(p);
    v.push_back(c);
  }
  void xfu(uint16_t reg, uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    xf(reg, {f});
  }
  // Full-EFB quad (clip space), tex0 = (0..1) left->right, top->bottom.
  void quad(const Quad &q) {
    GXCommand c{};
    c.type = GXCommandType::DrawPrimitive;
    c.prim_type = 0x80;
    const float xy[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    const float st[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int k = 0; k < 4; ++k) {
      gx::VertexData vd{};
      vd.pos[0] = vd.raw_pos[0] = xy[k][0];
      vd.pos[1] = vd.raw_pos[1] = xy[k][1];
      vd.pos[2] = vd.raw_pos[2] = 0.5f;
      vd.has_pos = vd.pre_xf = vd.has_color = true;
      std::memcpy(vd.color, q.rgba, sizeof vd.color);
      vd.has_tex[0] = true;
      vd.tex[0][0] = st[k][0];
      vd.tex[0][1] = st[k][1];
      if (q.has_norm) {
        vd.has_norm = true;
        std::memcpy(vd.norm, q.nrm, sizeof vd.norm);
        std::memcpy(vd.raw_norm, q.nrm, sizeof vd.raw_norm);
      }
      for (int t = 0; t < 8; ++t)
        vd.texMtxIdx[t] = 60; // identity row
      c.vertices.push_back(vd);
    }
    v.push_back(c);
  }
};

// genmode: ntex texgens, nchan channels, ntev stages, nind indirect stages.
static uint32_t Gen(int ntex, int nchan, int ntev, int nind) {
  return uint32_t(ntex) | (uint32_t(nchan) << 4) | (uint32_t(ntev - 1) << 10) | (uint32_t(nind) << 16);
}
// Colour combiner: out = d + lerp(a, b, c), clamp, dest; alpha likewise.
static uint32_t CC(int a, int b, int c, int d, int dest) {
  return uint32_t(d) | (uint32_t(c) << 4) | (uint32_t(b) << 8) | (uint32_t(a) << 12) | (1u << 19) |
         (uint32_t(dest) << 22);
}
static uint32_t AC(int a, int b, int c, int d, int dest) {
  return (uint32_t(d) << 4) | (uint32_t(c) << 7) | (uint32_t(b) << 10) | (uint32_t(a) << 13) | (1u << 19) |
         (uint32_t(dest) << 22);
}
enum { CC_C0 = 2, CC_C1 = 4, CC_TEXC = 8, CC_RASC = 10, CC_ZERO = 15 };
enum { CA_A0 = 1, CA_TEXA = 4, CA_RASA = 5, CA_ZERO = 7 };

// Common: viewport/scissor/projection, XF texgen 0 = TEX0 regular, one colour channel
// = vertex colour unlit, textures: map0 halves (clamp, near), map1 ramp (repeat, near).
static void BaseState(Cmds &c) {
  c.bp(0x00, Gen(1, 1, 1, 0));
  c.bp(0x28, 0u | (0u << 3) | (1u << 6)); // stage 0: texmap 0, texcoord 0, enable, COLOR0A0
  c.bp(0xC0, CC(CC_ZERO, CC_ZERO, CC_ZERO, CC_TEXC, 0));
  c.bp(0xC1, AC(CA_ZERO, CA_ZERO, CA_ZERO, CA_TEXA, 0));
  for (int i = 0; i < 16; ++i) c.bp(uint8_t(0x10 + i), 0); // no indirect
  c.bp(0xF3, 0x3F0000); // alpha test always
  // GXInit swap tables: 0 = RGBA (identity), 1 = RRRA, 2 = GGGA, 3 = BBBA.
  c.bp(0xF6, 0 | (1 << 2)); c.bp(0xF7, 2 | (3 << 2));
  c.bp(0xF8, 0 | (0 << 2)); c.bp(0xF9, 0 | (3 << 2));
  c.bp(0xFA, 1 | (1 << 2)); c.bp(0xFB, 1 | (3 << 2));
  c.bp(0xFC, 2 | (2 << 2)); c.bp(0xFD, 2 | (3 << 2));
  c.bp(0x43, 0x000001);
  c.bp(0x40, 0x000000);
  c.bp(0x41, 0x000018);
  c.bp(0x42, 0x000000);
  c.bp(0x20, (342u) | (342u << 12));
  c.bp(0x21, (342u + EH - 1) | ((342u + EW - 1) << 12));
  c.bp(0x59, (171u) | (171u << 10));
  c.bp(0x80, 0x000000);                          // map0: nearest, clamp
  c.bp(0x88, 15u | (7u << 10) | (1u << 20));     // map0: 16x8 I8
  c.bp(0x94, kHalves >> 5);
  c.bp(0x81, 0x000005);                          // map1: nearest, repeat/repeat
  c.bp(0x89, 63u | (3u << 10) | (1u << 20));     // map1: 64x4 I8
  c.bp(0x95, kRamp >> 5);
  c.bp(0x30, 15u);                               // texcoord 0 scale = 16 x 8
  c.bp(0x31, 7u);
  c.xfu(0x1009, 1);
  c.xfu(0x100A, 0x000000FF); c.xfu(0x100B, 0x000000FF);
  c.xfu(0x100C, 0xFFFFFFFF); c.xfu(0x100D, 0xFFFFFFFF);
  c.xfu(0x100E, 1);                              // colour0 = vertex colour, unlit
  c.xfu(0x1010, 1);
  c.xf(0x1020, {1.f, 0.f, 1.f, 0.f, -1.f, 0.f});
  c.xfu(0x1026, 1);
  c.xf(0x101A, {EW / 2.f, -EH / 2.f, 16777215.f, EW / 2.f + 342, EH / 2.f + 342, 16777215.f});
  c.xfu(0x103F, 1);
  c.xfu(0x1012, 0);
  for (int i = 0; i < 8; ++i) c.xfu(uint16_t(0x1040 + i), (5u << 7)); // regular, ST, TEX0
  for (int i = 0; i < 8; ++i) c.xfu(uint16_t(0x1050 + i), 61u);
  c.xf(60 * 4, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
}

static void Run(Cmds &c) {
  glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(1, 1, 1, 1);
  glClearColor(0.25f, 0.25f, 0.25f, 0.25f);
  glDepthMask(GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  s_r->Render(c.v);
  glFinish();
}
static void Px(int x, int y, uint8_t o[4]) {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
  glReadPixels(x, EH - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, o);
}
static bool Near(int v, int want, int tol = 3) { return std::abs(v - want) <= tol; }
static void LR(uint8_t l[4], uint8_t r[4]) {
  Px(EW / 4, EH / 2, l);
  Px(3 * EW / 4, EH / 2, r);
}

// I8 texel write (8x4 tiles).
static void PutI8(uint32_t base, int w, int x, int y, uint8_t v) {
  const int block = (y / 4) * (w / 8) + x / 8, within = (y % 4) * 8 + (x % 8);
  nwii::runtime::g_mmu->mem1[base + block * 32 + within] = v;
}
// RGBA8 texel write (4x4 tiles, AR then GB halves).
static void PutRGBA8(uint32_t base, int w, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  const int block = (y / 4) * (w / 4) + x / 4, i = (y % 4) * 4 + (x % 4);
  uint8_t *p = &nwii::runtime::g_mmu->mem1[base + block * 64];
  p[2 * i] = a; p[2 * i + 1] = r; p[32 + 2 * i] = g; p[32 + 2 * i + 1] = b;
}
static void FillInd(uint8_t sl, uint8_t sr) {
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 16; ++x)
      PutRGBA8(kInd, 16, x, y, 77, 128, 128, x < 8 ? sl : sr); // S=A, T=B, U=G
  // texture cache keys hash the texel data: the new contents get a new texture
}

// Indirect: stage 0 samples map1 (ramp) at TEXCOORD 0 wrapped to 0 + matrix 0 * ind(map2).
static void IndState(Cmds &c, uint32_t indcmd, int scale) {
  BaseState(c);
  c.bp(0x00, Gen(1, 1, 1, 1));
  c.bp(0x28, 1u | (0u << 3) | (1u << 6));        // stage 0: texmap 1, texcoord 0, enable
  c.bp(0x82, 0x000000);                          // map2: nearest, clamp
  c.bp(0x8A, 15u | (7u << 10) | (6u << 20));     // map2: 16x8 RGBA8
  c.bp(0x96, kInd >> 5);
  c.bp(0x27, 2u | (0u << 3));                    // ind stage 0: map 2, texcoord 0
  c.bp(0x25, 0);                                 // ind scale 1
  const uint32_t s = uint32_t(scale);            // stored scale = exp + 17
  c.bp(0x06, 512u | ((s & 3) << 22));            // ma = 0.5, mb = 0
  c.bp(0x07, 0u | (((s >> 2) & 3) << 22));       // mc = md = 0
  c.bp(0x08, 0u | (((s >> 4) & 3) << 22));       // me = mf = 0
  c.bp(0x10, indcmd);
}
// bt 0, ITF_8, bias STU, matrix 0, wrap S/T = ITW_0.
static const uint32_t kEggInd = 0u | (0u << 2) | (7u << 4) | (1u << 9) | (6u << 13) | (6u << 16);

static void LightState(Cmds &c, uint32_t cctrl, uint32_t actrl, uint32_t nchan) {
  BaseState(c);
  c.bp(0x00, Gen(0, 1, 1, 0));
  c.bp(0x28, 0u);                                // no texture, COLOR0A0
  c.bp(0xC0, CC(CC_ZERO, CC_ZERO, CC_ZERO, CC_RASC, 0));
  c.bp(0xC1, AC(CA_ZERO, CA_ZERO, CA_ZERO, CA_RASA, 0));
  c.xfu(0x1009, nchan);
  c.xfu(0x100A, 0x00000080);                     // ambient: black, alpha 0x80
  c.xfu(0x100C, 0xFFFFFFFF);                     // material white
  c.xfu(0x100E, cctrl);
  c.xfu(0x1010, actrl);
  // light 0: colour red, alpha 255; directional (attn none), far along +z.
  float l[16] = {};
  const uint32_t col = 0xFF0000FF;
  std::memcpy(&l[3], &col, 4);
  l[4] = 1; l[7] = 1;
  l[10] = 0; l[11] = 0; l[12] = 100000.f;
  l[13] = 0; l[14] = 0; l[15] = 1;
  c.xf(0x0600, std::vector<float>(l, l + 16));
}

int main() {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) return 2;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_Window *win = SDL_CreateWindow("light7", 0, 0, 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  SDL_GLContext glc = win ? SDL_GL_CreateContext(win) : nullptr;
  if (!glc || !gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
    std::fprintf(stderr, "no GL context\n");
    return 2;
  }
  GLuint tex = 0, dtex = 0;
  glGenFramebuffers(1, &s_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EW, EH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glGenTextures(1, &dtex);
  glBindTexture(GL_TEXTURE_2D, dtex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, EW, EH, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dtex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 2;
  auto ctx = std::make_unique<nwii::runtime::CPUContext>();
  nwii::runtime::g_mmu = &ctx->mmu;
  nwii::runtime::g_ctx_ptr = ctx.get();
  GX_SetWindow(win);
  s_r = gx::IRenderer::Create();
  s_r->Initialize(nullptr);
  gx::RendererSetCompileBudget(1 << 30);

  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 16; ++x) PutI8(kHalves, 16, x, y, x < 8 ? 0 : 255);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 64; ++x) PutI8(kRamp, 64, x, y, uint8_t(4 * x));
  uint8_t l[4], r[4];

  // ---- T1/T2: last TEV stage output regardless of destination (CAP7 D1) ----
  for (int dest : {3, 0}) {
    Cmds c; BaseState(c);
    c.bp(0x00, Gen(0, 1, 2, 0));
    c.bp(0x28, 0u);
    c.bp(0xE2, 0u | (255u << 12));                 // C0: R 0, A 255
    c.bp(0xE3, 0u | (255u << 12));                 // C0: B 0, G 255  -> green
    c.bp(0xE4, 255u | (255u << 12));               // C1: R 255, A 255
    c.bp(0xE5, 0u | (0u << 12));                   // C1: B 0, G 0   -> red
    c.bp(0xC0, CC(CC_ZERO, CC_ZERO, CC_ZERO, CC_C0, 0));
    c.bp(0xC1, AC(CA_ZERO, CA_ZERO, CA_ZERO, CA_A0, 0));
    c.bp(0xC2, CC(CC_ZERO, CC_ZERO, CC_ZERO, CC_C1, dest));
    c.bp(0xC3, AC(CA_ZERO, CA_ZERO, CA_ZERO, CA_A0, 0));
    c.quad(Quad{}); Run(c);
    LR(l, r);
    std::printf("T%d last stage dest %s: rgb=%d,%d,%d\n", dest ? 1 : 2, dest ? "C2" : "PREV", l[0], l[1], l[2]);
    check(Near(l[0], 255) && Near(l[1], 0) && Near(l[2], 0),
          dest ? "T1 last stage writing C2 is what reaches the EFB (red)" : "T2 control: last stage PREV (red)");
  }

  // ---- I1-I4: indirect texturing, EGG light-texture configuration ----
  {
    auto run = [&](uint32_t cmd, int scale, uint8_t sl, uint8_t sr, int &a, int &b) {
      FillInd(sl, sr);
      Cmds c; IndState(c, cmd, scale); c.quad(Quad{}); Run(c);
      LR(l, r); a = l[0]; b = r[0];
    };
    int a, b;
    run(kEggInd, 17, 139, 249, a, b);
    std::printf("I1 indirect S = 5.5 / 60.5 texels: L=%d R=%d (want 20 / 240)\n", a, b);
    check(Near(a, 20) && Near(b, 240), "I1 indirect offset = matrix * (ind.A - 128), texel units");
    run(kEggInd, 17, 117, 249, a, b);
    std::printf("I2 indirect S = -5.5 (repeat): L=%d (want 232)\n", a);
    check(Near(a, 232), "I2 negative offset wraps (signed s11 matrix, biased ITF_8 coord)");
    run(kEggInd, 16, 139, 249, a, b);
    std::printf("I3 matrix exponent -1: L=%d R=%d (want 8 / 120)\n", a, b);
    check(Near(a, 8) && Near(b, 120), "I3 matrix scale exponent halves the offset");
    run(0, 17, 139, 249, a, b);
    std::printf("I4 control IND_CMD = 0: L=%d R=%d (want 16 / 48)\n", a, b);
    check(Near(a, 16) && Near(b, 48), "I4 control: no indirect -> plain lookup (uv * tc scale / map size)");
  }


  // VISUAL32: format split and same-stage bump alpha. Reference values follow
  // the GX format bit layouts, not values generated by the implementation.
  for (int fmt = 0; fmt < 4; ++fmt) {
    FillInd(139, 249);
    Cmds c; IndState(c, kEggInd | (uint32_t(fmt) << 2), 17);
    c.quad(Quad{}); Run(c); LR(l,r);
    // ITF_8 preserves the existing control. Other formats take high bits,
    // then add bias +1: offsets = (18/32,9/16,5/8) * half a texel.
    const int expectedL[4] = {20,36,16,8}, expectedR[4] = {240,64,32,16};
    std::printf("V32 offset fmt%d: L=%d R=%d want %d/%d\n",fmt,l[0],r[0],expectedL[fmt],expectedR[fmt]);
    check(Near(l[0],expectedL[fmt]) && Near(r[0],expectedR[fmt]),"V32 format offset reads the high bits");
    for (uint32_t chan : {5u,6u}) {
      Cmds b; IndState(b, (uint32_t(fmt)<<2) | (1u<<7), 17);
      b.bp(0x28,chan<<7); // untextured raster bump output
      b.bp(0xC0,CC(CC_ZERO,CC_ZERO,CC_ZERO,CC_RASC,0));
      b.bp(0xC1,AC(CA_ZERO,CA_ZERO,CA_ZERO,CA_RASA,0));
      b.quad(Quad{});Run(b);LR(l,r);
      const int shifts[4]={0,5,4,3};
      int wantL=(139<<shifts[fmt])&248,wantR=(249<<shifts[fmt])&248;
      if(chan==6){wantL|=wantL>>5;wantR|=wantR>>5;}
      std::printf("V32 bump fmt%d chan%u: L=%d R=%d want %d/%d\n",fmt,chan,l[0],r[0],wantL,wantR);
      check(Near(l[0],wantL)&&Near(r[0],wantR)&&Near(l[3],wantL),"V32 bump alpha comes from the current stage and correct format bits");
    }
  }

  // ---- L1/L2: alpha channel lighting is separate from colour ----
  {
    // colour: mat reg, lit, light 0, amb reg, diffuse clamp, attn none (dir)
    const uint32_t cctrl = 0x2u | (1u << 2) | (2u << 7) | (2u << 9);
    const uint32_t actrl = 0x2u | (2u << 9); // alpha: lit, NO lights
    Quad q; q.has_norm = true;
    Cmds c; LightState(c, cctrl, actrl, 1); c.quad(q); Run(c);
    LR(l, r);
    std::printf("L1 colour lit by light 0, alpha lit w/o lights: rgba=%d,%d,%d,%d (want 255,0,0,128)\n",
                l[0], l[1], l[2], l[3]);
    check(Near(l[0], 255) && Near(l[1], 0) && Near(l[2], 0), "L1 colour = mat * (amb + light0)");
    check(Near(l[3], 128), "L1 alpha uses its own (empty) light mask: mat.a * amb.a");
    Cmds d; LightState(d, cctrl, cctrl, 1); d.quad(q); Run(d);
    LR(l, r);
    std::printf("L2 control alpha ctrl = colour ctrl: a=%d (want 255)\n", l[3]);
    check(Near(l[3], 255), "L2 control: alpha lit by light 0 -> 255");
  }

  // ---- N1/N2: numColorChans = 0 -> vertex colour ----
  {
    Quad q; q.rgba[0] = 0; q.rgba[1] = 1; q.rgba[2] = 0; q.rgba[3] = 1;
    for (uint32_t nch : {0u, 1u}) {
      Cmds c; LightState(c, 0x2u /* lit, no lights, mat reg */, 0x2u, nch);
      c.xfu(0x100C, 0x000000FF); // material black
      c.xfu(0x100A, 0x000000FF);
      c.quad(q); Run(c);
      LR(l, r);
      std::printf("N%u numColorChans=%u: rgb=%d,%d,%d\n", nch ? 2u : 1u, nch, l[0], l[1], l[2]);
      if (nch == 0)
        check(Near(l[0], 0) && Near(l[1], 255) && Near(l[2], 0), "N1 no colour channels -> vertex colour");
      else
        check(Near(l[0], 0) && Near(l[1], 0) && Near(l[2], 0), "N2 control: channel 0 lit -> black");
    }
  }

  // ---- C1/C2: colour texgen from the lit channel colour ----
  {
    Quad q; q.rgba[0] = 0.25f; q.rgba[1] = 0.25f; q.rgba[2] = 0.25f;
    for (int lit : {1, 0}) {
      Cmds c; BaseState(c);
      c.xfu(0x1040, (2u << 4) | (2u << 7)); // texgen 0: COLOR0
      c.xfu(0x100A, 0xFFFFFFFF);            // ambient white
      c.xfu(0x100C, 0xC0C0C0FF);            // material 0.75
      c.xfu(0x100E, lit ? 0x2u : 0x1u);     // lit (mat reg) / unlit (mat = vertex)
      c.quad(q); Run(c);
      LR(l, r);
      std::printf("C%d colour texgen, channel %s: tex=%d\n", lit ? 1 : 2, lit ? "lit (0.75)" : "vertex (0.25)", l[0]);
      check(Near(l[0], lit ? 255 : 0),
            lit ? "C1 colour texgen reads the lit colour (s = 0.75)" : "C2 control: vertex colour (s = 0.25)");
    }
  }

  // ---- Z1/Z2: rasterized colour ZERO has alpha 0 ----
  for (uint32_t chan : {7u, 0u}) {
    Cmds c; LightState(c, 0x1u, 0x1u, 1); // unlit, vertex colour (white, alpha 1)
    c.bp(0x28, chan << 7);
    c.quad(Quad{}); Run(c);
    LR(l, r);
    std::printf("Z%d ras channel %u: rgba=%d,%d,%d,%d\n", chan == 7 ? 1 : 2, chan, l[0], l[1], l[2], l[3]);
    if (chan == 7)
      check(Near(l[0], 0) && Near(l[3], 0), "Z1 colour channel ZERO -> rgba 0 (Dolphin tev_ras_table)");
    else
      check(Near(l[0], 255) && Near(l[3], 255), "Z2 control: COLOR0A0 = vertex colour");
  }

  // ---- S1-S4: TEV swap tables (alpha env bits 0-1 ras table, 2-3 tex table) ----
  // SS draws its character shadow maps (RGB565 EFB copy, silhouette in R) with tex
  // table RRRR: alpha = tex.R. Without the swap the whole projected area darkens.
  for (int tsw : {1, 0}) {
    FillInd(10, 200); // RGBA8 16x8: r 77, g 128, b 128, a 10 | 200
    Cmds c; BaseState(c);
    c.bp(0x28, 2u | (0u << 3) | (1u << 6));        // stage 0: texmap 2, texcoord 0
    c.bp(0x82, 0x000000);
    c.bp(0x8A, 15u | (7u << 10) | (6u << 20));     // map2: 16x8 RGBA8
    c.bp(0x96, kInd >> 5);
    c.bp(0xF8, 3u | (0u << 2)); c.bp(0xF9, 1u | (0u << 2)); // table 1 = A R G R
    c.bp(0xC1, AC(CA_ZERO, CA_ZERO, CA_ZERO, CA_TEXA, 0) | (uint32_t(tsw) << 2));
    c.quad(Quad{}); Run(c);
    LR(l, r);
    std::printf("S%d tex swap table %d: L rgba=%d,%d,%d,%d R rgba=%d,%d,%d,%d\n", tsw ? 1 : 2, tsw, l[0], l[1],
                l[2], l[3], r[0], r[1], r[2], r[3]);
    if (tsw)
      check(Near(l[0], 10) && Near(r[0], 200) && Near(l[1], 77) && Near(l[2], 128) && Near(l[3], 77),
            "S1 tex swap table 1 (A,R,G,R) swizzles the texture sample");
    else
      check(Near(l[0], 77) && Near(l[1], 128) && Near(l[2], 128) && Near(l[3], 10) && Near(r[3], 200),
            "S2 control: tex swap table 0 = identity");
  }
  for (int rsw : {2, 0}) {
    Quad q; q.rgba[0] = 0.25f; q.rgba[1] = 0.5f; q.rgba[2] = 0.75f; q.rgba[3] = 1.0f;
    Cmds c; LightState(c, 0x1u, 0x1u, 1);          // unlit, vertex colour
    c.bp(0xFA, 2u | (1u << 2)); c.bp(0xFB, 0u | (3u << 2)); // table 2 = B G R A
    c.bp(0xC1, AC(CA_ZERO, CA_ZERO, CA_ZERO, CA_RASA, 0) | uint32_t(rsw));
    c.quad(q); Run(c);
    LR(l, r);
    std::printf("S%d ras swap table %d: rgba=%d,%d,%d,%d\n", rsw ? 3 : 4, rsw, l[0], l[1], l[2], l[3]);
    if (rsw)
      check(Near(l[0], 191) && Near(l[1], 128) && Near(l[2], 64) && Near(l[3], 255),
            "S3 ras swap table 2 (B,G,R,A) swizzles the rasterized colour");
    else
      check(Near(l[0], 64) && Near(l[1], 128) && Near(l[2], 191) && Near(l[3], 255),
            "S4 control: ras swap table 0 = identity");
  }


  // VISUAL32: a CPU rewrite between A and I must invalidate I's GPU-copy
  // read without deleting the real registry entry. Previously A's frame
  // check accepted it, and the RAM cache returned that same rejected entry.
  {
    Cmds a;LightState(a,1,1,1);
    Quad red;red.rgba[1]=red.rgba[2]=0;
    a.quad(red);
    a.bp(0x49,0);a.bp(0x4A,15u|(7u<<10));a.bp(0x4B,kInd>>5);
    a.bp(0x4D,2);a.bp(0x52,12u<<3); // RGBA8 16x8 GPU copy
    Run(a);
    auto &tc=nwii::runtime::hle::TextureCache::get();
    const GLuint registry=tc.find_efb_texture(kInd);
    FillInd(139,249); // CPU repurposes the same memory as ordinary RGBA8
    Cmds b;BaseState(b);
    b.bp(0x28,2u|(1u<<6));b.bp(0x82,0);
    b.bp(0x8A,15u|(7u<<10)|(6u<<20));b.bp(0x96,kInd>>5);
    b.quad(Quad{});
    b.bp(0x49,0);b.bp(0x4A,639u|(479u<<10));b.bp(0x4B,0x480000u>>5);
    b.bp(0x4D,40);b.bp(0x52,1u<<14);
    struct PassHook:gx::InterpDrawHook {
      bool Pre(size_t,const GXCommand&)override{return true;}
      void PostConvert(const GXCommand&,float*,size_t)override{}
      void Post(size_t,const GXCommand&)override{}
    } hook;
    const auto before=gx::GetInterpRenderStats();
    const bool rendered=gx::RenderInterpFrame(*s_r,b.v,hook);
    std::vector<uint8_t> image;int w=0,h=0;
    const bool read=gx::ReadPresentedFull(image,w,h);
    const int pixel=read?image[((h/2)*w+w/4)*4]:-1;
    const auto after=gx::GetInterpRenderStats();
    std::printf("V32 stale copy: red=%d (want 77), stale reads +%llu, registry kept=%d\n",pixel,
      (unsigned long long)(after.stale_reads-before.stale_reads),tc.find_efb_texture(kInd)==registry);
    check(rendered&&read&&Near(pixel,77)&&after.stale_reads>before.stale_reads,
      "V32 interpolation reads rewritten RAM rather than stale GPU copy");
    check(registry&&tc.find_efb_texture(kInd)==registry,"V32 real EFB registry survives isolated stale read");
  }

  {
    Cmds c; BaseState(c);
    c.bp(0x00, 15u << 10); // unique, sixteen-stage program, absent from prior tests
    c.quad(Quad{});
    struct PassHook:gx::InterpDrawHook {
      bool Pre(size_t,const GXCommand&)override{return true;}
      void PostConvert(const GXCommand&,float*,size_t)override{}
      void Post(size_t,const GXCommand&)override{}
    } hook;
    uint8_t before[4], after[4]; Px(EW/4,EH/2,before);
    const auto initial=gx::GetInterpRenderStats();
    gx::RendererSetCompileBudget(0);
    const bool accepted=gx::RenderInterpFrame(*s_r,c.v,hook);
    const auto missed=gx::GetInterpRenderStats();
    Px(EW/4,EH/2,after);
    check(!accepted && missed.skipped_draws>initial.skipped_draws,
      "V33 incomplete interpolated frame is refused while its shader is unavailable");
    check(std::memcmp(before,after,4)==0,"V33 refused interpolation preserves the real EFB image");
    gx::RendererSetCompileBudget(1<<20);
    check(gx::RenderInterpFrame(*s_r,c.v,hook),"V33 complete interpolated frame is accepted after its shader becomes available");
  }

  {
    auto depth=[] {float d;glBindFramebuffer(GL_READ_FRAMEBUFFER,s_fbo);glReadPixels(EW/4,EH/2,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&d);return double(d);};
    auto setup=[] {Cmds c;BaseState(c);c.bp(0x88,15u|(7u<<10)|(6u<<20));c.bp(0x94,kInd>>5);c.bp(0x40,31);return c;};
    for(int y=0;y<8;++y)for(int x=0;x<16;++x)PutRGBA8(kInd,16,x,y,17,34,51,68);
    const int packed[3]={68,17+68*256,0x112233};
    for(int type=0;type<3;++type){
      auto c=setup();c.bp(0xF4,0x123400);c.bp(0xF5,8u|type);c.quad(Quad{});Run(c);
      const double expected=double(0x123400+packed[type])/16777216.0;
      std::printf("Z34 format%d depth %.8f want %.8f\n",type,depth(),expected);
      check(std::fabs(depth()-expected)<2e-7,"Z34 replace uses the correct 8/16/24-bit packing and bias");
    }
    auto c=setup();c.bp(0xF4,0xFFFFF0);c.bp(0xF5,8);c.quad(Quad{});Run(c);
    check(std::fabs(depth()-52.0/16777216.0)<2e-7,"Z34 depth plus bias wraps at 24 bits");
    auto original=setup();original.bp(0xF5,0);original.quad(Quad{});Run(original);
    const int prior=int(depth()*16777216.0);
    c=setup();c.bp(0xF4,100);c.bp(0xF5,6);c.quad(Quad{});Run(c);
    check(std::fabs(depth()-double((prior+100+0x112233)&0xFFFFFF)/16777216.0)<3e-7,"Z34 add includes interpolated geometry depth");
    c=setup();c.bp(0xF4,0);c.bp(0xF5,10);c.bp(0x43,65);c.quad(Quad{});Run(c);
    check(std::fabs(depth()-double(prior)/16777216.0)<3e-7,"Z34 early depth mode keeps geometry depth");
    c=setup();c.bp(0xF4,0);c.bp(0xF5,10);c.bp(0xC1,AC(CA_ZERO,CA_ZERO,CA_ZERO,CA_TEXA,0)|(1u<<2));c.quad(Quad{});Run(c);
    check(std::fabs(depth()-double(0x112233)/16777216.0)<2e-7,"Z34 depth reads raw texture before TEV channel swapping");
    c=setup();c.bp(0xF4,1);c.bp(0xF5,10);c.quad(Quad{});c.bp(0xF4,0x200001);c.quad(Quad{});Run(c);
    check(std::fabs(depth()-double(0x312234)/16777216.0)<2e-7,"Z34 changing only bias updates the current program uniform");
    // A farther cyan compositing quad must not cover nearer green geometry.
    for(int y=0;y<8;++y)for(int x=0;x<16;++x)PutRGBA8(kInd,16,x,y,204,204,204,255);
    c=setup();c.bp(0xF4,0);c.bp(0xF5,0);c.bp(0x40,23);c.bp(0xC0,CC(CC_ZERO,CC_ZERO,CC_ZERO,CC_RASC,0));
    Quad green;green.rgba[0]=0;green.rgba[1]=1;green.rgba[2]=0;green.rgba[3]=1;c.quad(green);
    c.bp(0xF5,10);Quad cyan=green;cyan.rgba[2]=1;c.quad(cyan);Run(c);
    uint8_t pixel[4];Px(EW/4,EH/2,pixel);
    check(pixel[0]<3&&pixel[1]>250&&pixel[2]<3,"Z34 depth-texture overlay respects scene occlusion instead of painting it cyan");
    c.bp(0xF5,0);c.quad(cyan);Run(c);Px(EW/4,EH/2,pixel);
    check(pixel[1]>250&&pixel[2]>250,"Z34 disabling depth textures restores the geometry-depth control");
  }

  std::printf("%s: light7 test, %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  s_r.reset();
  SDL_GL_DeleteContext(glc);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return fails ? 1 : 0;
}
