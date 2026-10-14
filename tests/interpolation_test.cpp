// INTERP17: unit test of the live frame interpolation core (nWiiRuntime/include/runtime/gx/frame_interp.h)
// and the present schedule (frame_pipeline.h PacePlanner). No GL: frames are built as GX command lists
// with DrawRaw snapshots and hand-decoded vertices; the renderer's vertex conversion is emulated (pre_xf
// position as converted) and PostConvert rewrites it like the live in-between pass does.
//
//   interp17-test                positive run: every check PASS
//   interp17-test --neg-lerp     element-wise matrix lerp instead of the camera-separated slerp: M2 M3 M4 fail
//   interp17-test --neg-wrap     texture scroll wrap not unwrapped: M5 fails
//   interp17-test --neg-index    naive index pairing (no keys, no re-pairing): K1 K2 K3 fail
//   interp17-test --neg-guard    cut / jump guards off: C1 C2 C3 C4 G1 G2 fail
//   interp17-test --neg-values   fades (TEV registers) not interpolated: V1 fails
//   interp17-test --neg-auto     instances re-paired by origin only: K4 fails
//   interp17-test --neg-2dloose  2D glyphs may pair on the loose tier: G3 fails
//   interp17-test --neg-version  per-draw state read at the end of the frame instead of at the draw: L1 L2 fail
//   interp17-test --neg-pace     present as soon as ready (no schedule): P1 fails
//   interp17-test --neg-kmode    camera delta per vertex on the CPU (no K in the projection): Q1 fails
#include <array>
#include <atomic>
#include <functional>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "runtime/gx/frame_interp.h"
#include "runtime/gx/frame_pipeline.h"

namespace gx = nwii::runtime::gx;
namespace ip = nwii::runtime::gx::interp;
using gx::DrawRaw;
using gx::GXCommand;
using gx::GXCommandType;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char *id, const char *what) {
  std::printf("%s: %s %s\n", ok ? "PASS" : "FAIL", id, what);
  (ok ? g_pass : g_fail)++;
}
static bool Near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) <= e; }

static bool g_neg_lerp = false, g_neg_wrap = false, g_neg_index = false, g_neg_guard = false, g_neg_values = false;
static bool g_neg_auto = false, g_neg_2dloose = false, g_neg_version = false, g_neg_pace = false, g_neg_kmode = false;

static ip::Config Cfg() {
  ip::Config c;
  c.camera_mode = !g_neg_lerp;
  c.tex_unwrap = !g_neg_wrap;
  c.index_only = g_neg_index;
  c.guards = !g_neg_guard;
  c.values = !g_neg_values;
  c.refine_auto = !g_neg_auto;
  c.ortho_loose = g_neg_2dloose;
  c.kmode = !g_neg_kmode;
  return c;
}

static ip::Aff RotY(float deg, float tx = 0, float ty = 0, float tz = 0) {
  const float r = deg * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
  ip::Aff a{};
  a.m[0] = c; a.m[2] = s; a.m[3] = tx;
  a.m[5] = 1; a.m[7] = ty;
  a.m[8] = -s; a.m[10] = c; a.m[11] = tz;
  return a;
}
static ip::Aff Trans(float x, float y, float z) { ip::Aff a = ip::AffIdentity(); a.m[3] = x; a.m[7] = y; a.m[11] = z; return a; }
static void Xf(const ip::Aff &m, const float p[3], float o[3]) {
  for (int i = 0; i < 3; ++i) o[i] = m.m[i * 4] * p[0] + m.m[i * 4 + 1] * p[1] + m.m[i * 4 + 2] * p[2] + m.m[i * 4 + 3];
}

// ---- frame builder ----
struct Frame {
  std::vector<GXCommand> cmds;
  std::vector<std::shared_ptr<DrawRaw>> raws;
  ip::FrameRec rec;
};
static GXCommand XF(int addr, std::vector<float> v) {
  GXCommand c{};
  c.type = GXCommandType::XFRegister;
  c.reg = (uint16_t)addr;
  c.length = (uint16_t)(v.size() - 1);
  c.payload = std::move(v);
  return c;
}
static GXCommand BP(uint8_t reg, uint32_t val) {
  GXCommand c{};
  c.type = GXCommandType::BPRegister;
  c.reg = reg;
  c.val = val;
  return c;
}
static void Persp(Frame &f) { f.cmds.push_back(XF(0x1020, {1.0f, 0.0f, 1.0f, 0.0f, -1.0f, -2.0f, 0.0f})); }
static void Ortho(Frame &f) {
  float t;
  const uint32_t one = 1;
  std::memcpy(&t, &one, 4);
  f.cmds.push_back(XF(0x1020, {2.0f / 640.0f, -1.0f, -2.0f / 480.0f, 1.0f, -1.0f, 0.0f, t}));
}
// a draw of `pts` (model space) with the single-matrix snapshot mv; `tag` makes its vertex bytes unique.
static void Draw(Frame &f, const std::vector<std::array<float, 3>> &pts, const ip::Aff &mv, uint32_t tag,
                 const std::vector<float> *bank = nullptr, const std::vector<uint8_t> *pmidx = nullptr) {
  auto r = std::make_shared<DrawRaw>();
  std::memset(&r->vat, 0, sizeof r->vat);
  r->vat.posMask = gx::VtxAttrMask::Direct;
  r->vat.posType = gx::VtxAttrType::F32;
  r->vat.posElements = true;
  r->vat.posMatIdx = bank != nullptr;
  std::memset(r->arrayBase, 0, sizeof r->arrayBase);
  std::memset(r->arrayStride, 0, sizeof r->arrayStride);
  r->defPosMtxIdx = 0;
  std::memset(r->defTexMtxIdx, 0, sizeof r->defTexMtxIdx);
  r->need_normal = false;
  r->count = (uint16_t)pts.size();
  for (size_t i = 0; i < pts.size(); ++i) {
    if (pmidx) r->bytes.push_back((*pmidx)[i]);
    const uint8_t *b = reinterpret_cast<const uint8_t *>(pts[i].data());
    r->bytes.insert(r->bytes.end(), b, b + 12);
  }
  const uint8_t *tb = reinterpret_cast<const uint8_t *>(&tag);
  r->bytes.insert(r->bytes.end(), tb, tb + 4);
  if (bank) {
    r->snap_pos_bank = *bank;
    r->snap_nrm_bank.assign(96, 0.0f);
  } else {
    r->have_snap_mtx = true;
    std::memcpy(r->snap_pos_mtx, mv.m, 48);
    const float n[9] = {mv.m[0], mv.m[1], mv.m[2], mv.m[4], mv.m[5], mv.m[6], mv.m[8], mv.m[9], mv.m[10]};
    std::memcpy(r->snap_nrm_mtx, n, 36);
  }
  GXCommand c{};
  c.type = GXCommandType::DrawPrimitive;
  c.prim_type = 0x90;
  c.raw = r;
  f.raws.push_back(r);
  // "decode": model-space raw_pos, view-space pos (pre_xf), like FifoParser::DecodeDraw
  for (size_t i = 0; i < pts.size(); ++i) {
    gx::VertexData v{};
    v.has_pos = true;
    std::memcpy(v.raw_pos, pts[i].data(), 12);
    const ip::Aff *m = &mv;
    ip::Aff bm;
    if (bank) {
      v.posMtxIdx = (*pmidx)[i];
      std::memcpy(bm.m, &(*bank)[v.posMtxIdx * 4], 48);
      m = &bm;
    }
    Xf(*m, v.raw_pos, v.pos);
    v.pre_xf = true;
    c.vertices.push_back(v);
  }
  f.cmds.push_back(std::move(c));
}
static void Record(Frame &f, uint64_t no) {
  static gx::GXState st{};
  ip::BaseState base{};
  ip::CaptureBase(st, base);
  ip::RecordFrame(f.cmds, f.raws, base, no, f.rec);
}
// The in-between position of B draw `di` (emulated renderer conversion + PostConvert).
static std::vector<std::array<float, 3>> Interp(Frame &A, Frame &B, ip::Plan &P, const ip::Config &cfg, size_t di,
                                                ip::DrawOverride *ov_out = nullptr, bool *cpu_out = nullptr) {
  const ip::DrawRec &d = B.rec.draws[di];
  const GXCommand &bc = B.cmds[d.ci];
  const GXCommand *ac = d.match >= 0 ? &A.cmds[A.rec.draws[d.match].ci] : nullptr;
  ip::DrawOverride ov;
  ip::PrepareDraw(A.rec, B.rec, P, cfg, di, bc, ac, 0, ov);
  const size_t F = 13;
  std::vector<float> conv(bc.vertices.size() * F, 0.0f);
  for (size_t i = 0; i < bc.vertices.size(); ++i) std::memcpy(&conv[i * F], bc.vertices[i].pos, 12);
  ip::BankCache bcache;
  static float live_pos[256], live_nrm[96];
  ip::PostConvert(P, cfg, ov, bc, ac, conv.data(), F, live_pos, live_nrm, bcache);
  // what the GPU draws: in K mode every perspective draw of the in-between frame goes through proj * K
  const bool k = P.kmode && !P.cut && !d.ortho;
  std::vector<std::array<float, 3>> out;
  for (size_t i = 0; i < bc.vertices.size(); ++i) {
    std::array<float, 3> q = {conv[i * F], conv[i * F + 1], conv[i * F + 2]};
    if (k) { float o[3]; Xf(P.K, q.data(), o); q = {o[0], o[1], o[2]}; }
    out.push_back(q);
  }
  if (cpu_out) *cpu_out = ov.vmode != 0 || ov.kfix;
  if (ov_out) *ov_out = ov;
  return out;
}
static const std::vector<std::array<float, 3>> kTri = {{{1, 0, 0}}, {{0, 1, 0}}, {{0, 0, 1}}};
// static world: n objects at fixed world positions, seen by view V
static void World(Frame &f, const ip::Aff &V, int n, uint32_t tag0 = 1000) {
  for (int i = 0; i < n; ++i) Draw(f, kTri, ip::AffMul(V, Trans(10.0f * (i % 7) - 30, 3.0f * (i % 3), -100.0f - 5.0f * i)), tag0 + i);
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--neg-lerp") g_neg_lerp = true;
    else if (a == "--neg-wrap") g_neg_wrap = true;
    else if (a == "--neg-index") g_neg_index = true;
    else if (a == "--neg-guard") g_neg_guard = true;
    else if (a == "--neg-values") g_neg_values = true;
    else if (a == "--neg-auto") g_neg_auto = true;
    else if (a == "--neg-2dloose") g_neg_2dloose = true;
    else if (a == "--neg-version") g_neg_version = true;
    else if (a == "--neg-pace") g_neg_pace = true;
    else if (a == "--neg-kmode") g_neg_kmode = true;
  }
  const ip::Config cfg = Cfg();
  { // M1
    const ip::Aff a = ip::AffMul(RotY(33, 1, 2, 3), Trans(4, -5, 6));
    ip::Aff inv;
    const bool ok = ip::AffInverse(a, inv);
    const ip::Aff id = ip::AffMul(a, inv);
    bool good = ok;
    for (int i = 0; i < 12; ++i) good = good && Near(id.m[i], ip::AffIdentity().m[i], 1e-5f);
    Check(good, "M1", "affine inverse * matrix = identity");
  }
  { // M2: an object turning 90 degrees about its own axis (static camera, 20 static world draws)
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 20);
    World(B, ip::AffIdentity(), 20);
    Draw(A, {{{5, 0, 0}}}, Trans(0, 0, -50), 7);
    Draw(B, {{{5, 0, 0}}}, ip::AffMul(Trans(0, 0, -50), RotY(90)), 7);
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    const auto v = Interp(A, B, P, cfg, 20);
    const float r = std::sqrt(v[0][0] * v[0][0] + (v[0][2] + 50) * (v[0][2] + 50));
    Check(!P.cut && Near(r, 5.0f, 1e-2f) && Near(v[0][0], 5 * std::cos(0.785398f), 1e-2f), "M2",
          "a 90-degree turn is slerped (radius kept, half the angle)");
  }
  { // M3 / M4: the camera turns 60 degrees about the eye; static world + a camera-locked sky dome
    Frame A, B;
    Persp(A); Persp(B);
    const ip::Aff VA = ip::AffIdentity(), VB = RotY(40);
    World(A, VA, 30);
    World(B, VB, 30);
    Draw(A, {{{0, 0, -1000}}}, VA, 9); // sky dome centred at the eye, turning with the view
    Draw(B, {{{0, 0, -1000}}}, VB, 9);
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    const auto v = Interp(A, B, P, cfg, 0);
    float want[3];
    const float p0[3] = {1, 0, 0};
    Xf(ip::AffMul(RotY(20), Trans(-30, 0, -100)), p0, want);
    Check(P.cam.ok && Near(P.cam.angle_deg, 40, 0.05f) && Near(v[0][0], want[0], 1e-2f) && Near(v[0][2], want[2], 1e-2f),
          "M3", "static world under a 40-degree camera turn sits at the half-turned camera");
    const auto s = Interp(A, B, P, cfg, 30);
    const float dist = std::sqrt(s[0][0] * s[0][0] + s[0][1] * s[0][1] + s[0][2] * s[0][2]);
    Check(Near(dist, 1000.0f, 1.0f), "M4", "eye-centred sky dome keeps its distance to the eye (a lerp would pull it in)");
  }
  { // M5: texture scroll wrap
    const float a[12] = {1, 0, 0, 0.9f, 0, 1, 0, 0, 0, 0, 1, 0}, b[12] = {1, 0, 0, 0.1f + 0.0f, 0, 1, 0, 0, 0, 0, 1, 0};
    float o[12];
    ip::InterpTex(a, b, 12, 0.5f, o, !g_neg_wrap);
    Check(Near(o[3], 0.0f, 1e-4f) || Near(o[3], 1.0f, 1e-4f), "M5", "texture scroll wrap is continued, not run backwards");
  }
  { // K1 / K2: an inserted and a removed draw; keyed matching keeps the pairs
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 4, 100);
    Draw(B, kTri, Trans(0, 0, -20), 55); // new object first
    World(B, ip::AffIdentity(), 4, 100);
    Record(A, 1); Record(B, 2);
    ip::MatchStats ms;
    ip::Match(A.rec, B.rec, ms, cfg.index_only, cfg.ortho_loose);
    Check(B.rec.draws[0].match == -1 && B.rec.draws[1].match == 0 && B.rec.draws[4].match == 3, "K1",
          "inserted draw: the others still pair with their own draw");
    Frame C, D;
    Persp(C); Persp(D);
    World(C, ip::AffIdentity(), 5, 200);
    World(D, ip::AffIdentity(), 5, 200);
    D.cmds.erase(D.cmds.begin() + 2); // the second world draw vanished
    D.raws.erase(D.raws.begin() + 1);
    Record(C, 1); Record(D, 2);
    ip::Match(C.rec, D.rec, ms, cfg.index_only, cfg.ortho_loose);
    Check(D.rec.draws[0].match == 0 && D.rec.draws[1].match == 2 && D.rec.draws[3].match == 4, "K2",
          "removed draw: later draws pair with their own draw");
  }
  { // K3: equal-key instances, one born in front (ordinal pairs would all shift by one)
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 20);
    World(B, ip::AffIdentity(), 20);
    for (int i = 0; i < 6; ++i) Draw(A, kTri, Trans(2.0f * i, 0, -40), 77);
    Draw(B, kTri, Trans(-2.0f, 0, -40), 77); // born
    for (int i = 0; i < 6; ++i) Draw(B, kTri, Trans(2.0f * i + 0.1f, 0, -40), 77);
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    bool ok = true;
    for (int i = 0; i < 6; ++i) {
      const auto v = Interp(A, B, P, cfg, 21 + i);
      ok = ok && Near(v[0][0], 1 + 2.0f * i + 0.05f, 1e-2f);
    }
    Check(ok, "K3", "instances re-paired by nearest position after a birth (no swap ghosting)");
  }
  { // K4: rays sharing one origin, each turning a little; ordinal pairs are right, origin-nearest would swap
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 20);
    World(B, ip::AffIdentity(), 20);
    for (int i = 0; i < 8; ++i) Draw(A, {{{0, 0, -3}}}, ip::AffMul(Trans(0, 0, -40), RotY(45.0f * i)), 88);
    for (int i = 0; i < 8; ++i) // the burst's draw order rotated by one: ray i of B is ray i+1 of A, turned 6 degrees
      Draw(B, {{{0, 0, -3}}}, ip::AffMul(Trans(0, 0, -40), RotY(45.0f * ((i + 1) % 8) + 6.0f)), 88);
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    bool ok = true;
    for (int i = 0; i < 8; ++i) {
      const auto v = Interp(A, B, P, cfg, 20 + i);
      float want[3];
      const float p0[3] = {0, 0, -3};
      Xf(ip::AffMul(Trans(0, 0, -40), RotY(45.0f * ((i + 1) % 8) + 3.0f)), p0, want);
      ok = ok && Near(v[0][0], want[0], 2e-2f) && Near(v[0][2], want[2], 2e-2f);
    }
    Check(ok, "K4", "rays sharing an origin re-paired by their full matrix (each turns half its own 6-degree step)");
  }
  { // C1: camera turn of 70 degrees -> cut
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 30);
    World(B, RotY(70), 30);
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    const auto v = Interp(A, B, P, cfg, 0);
    Check(P.cut && Near(v[0][0], B.cmds[B.rec.draws[0].ci].vertices[0].pos[0], 1e-6f), "C1",
          "camera turn > 50 degrees -> cut: the in-between frame is B");
  }
  { // C2 / C3: scene change; scene shrinks to a few draws
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 30, 100);
    for (int i = 0; i < 30; ++i) // another area: other geometry (4-vertex draws) elsewhere
      Draw(B, {{{0, 0, 0}}, {{1, 0, 0}}, {{1, 1, 0}}, {{0, 1, 0}}}, Trans(7.0f * i, 0, -60.0f - i), 5000 + i);
    Draw(A, kTri, Trans(0, 0, -9), 1);
    Draw(B, kTri, Trans(0, 0, -9), 1);
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    Check(P.cut, "C2", "scene change (1 of 31 draws matched) -> cut");
    Frame C, D;
    Persp(C); Persp(D);
    World(C, ip::AffIdentity(), 100);
    World(D, ip::AffIdentity(), 5);
    Record(C, 1); Record(D, 2);
    ip::MakePlan(C.rec, D.rec, 0.5f, cfg, nullptr, P);
    Check(P.cut, "C3", "scene shrank from 100 to 5 draws (5 matched) -> cut");
  }
  { // C4: spike: 200 units after 2-unit steps
    ip::Camera prev;
    prev.ok = true;
    prev.trans = 2.0f;
    prev.angle_deg = 0.5f;
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 30);
    World(B, Trans(0, 0, 200), 30);
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, &prev, P);
    Check(P.cut && P.cam.ok, "C4", "camera jump of 200 units after 2-unit steps -> cut (spike rule)");
  }
  { // G1 / G2 / G3: 2D jump, object jump, 2D strict matching
    Frame A, B;
    Ortho(A); Ortho(B);
    Draw(A, {{{0, 0, 0}}}, Trans(10, 10, 0), 1);
    Draw(B, {{{0, 0, 0}}}, Trans(400, 10, 0), 1); // cursor hop: 60% of the view
    Draw(A, {{{0, 0, 0}}}, Trans(10, 100, 0), 2);
    Draw(B, {{{0, 0, 0}}}, Trans(14, 100, 0), 2); // slide: interpolated
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    const auto j = Interp(A, B, P, cfg, 0), s = Interp(A, B, P, cfg, 1);
    Check(Near(j[0][0], 400) && Near(s[0][0], 12), "G1", "2D jump (60% of the view) -> B, a small slide is interpolated");
    Frame C, D;
    Persp(C); Persp(D);
    World(C, ip::AffIdentity(), 20);
    World(D, ip::AffIdentity(), 20);
    Draw(C, kTri, Trans(0, 0, -20), 3);
    Draw(D, kTri, Trans(300, 0, -20), 3); // teleport
    Record(C, 1); Record(D, 2);
    ip::MakePlan(C.rec, D.rec, 0.5f, cfg, nullptr, P);
    const auto t = Interp(C, D, P, cfg, 20);
    Check(Near(t[0][0], 301), "G2", "object teleport (moved > 35% of its distance) -> B");
    // G3: two words with the same font material/texture; glyph quads of different letters must not pair
    Frame E, G;
    Ortho(E); Ortho(G);
    Draw(E, {{{1, 0, 0}}}, Trans(100, 50, 0), 501); // 'E'
    Draw(G, {{{2, 0, 0}}}, Trans(110, 50, 0), 502); // 'L' (other bytes, same material/texture, elsewhere)
    Record(E, 1); Record(G, 2);
    ip::MakePlan(E.rec, G.rec, 0.5f, cfg, nullptr, P);
    const auto g = Interp(E, G, P, cfg, 0);
    Check(G.rec.draws[0].match < 0 && Near(g[0][0], 112), "G3", "2D glyphs of different words do not pair (2D strict)");
  }
  { // V1: a fade: TEV konst alpha 100 -> 200 interpolated to 150 for the in-between frame
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 20);
    World(B, ip::AffIdentity(), 20);
    A.cmds.push_back(BP(0xE0, 0x800000 | (100u << 12)));
    B.cmds.push_back(BP(0xE0, 0x800000 | (200u << 12)));
    Draw(A, kTri, Trans(0, 0, -10), 4);
    Draw(B, kTri, Trans(0, 0, -10), 4);
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    ip::DrawOverride ov;
    Interp(A, B, P, cfg, 20, &ov);
    uint32_t v = 0;
    bool found = false;
    for (int i = 0; i < ov.nw; ++i)
      if (ov.w[i].r == ip::kRegTev) { std::memcpy(&v, &ov.w[i].v[8], 4); found = true; }
    Check(found && ((v >> 12) & 0x7FF) == 150 && (v & 0x800000), "V1", "fade value (TEV konst alpha) interpolated: 100 -> 150 -> 200");
  }
  { // L1 / L2: versioned state: a matrix row and a TEV register rewritten after a draw
    Frame A;
    Persp(A);
    A.cmds.push_back(XF(30 * 4, {1, 0, 0, 0.25f, 0, 1, 0, 0, 0, 0, 1, 0})); // texture matrix slot 30
    A.cmds.push_back(BP(0xE1, 0x800000 | 0x111));
    Draw(A, kTri, Trans(0, 0, -10), 1);
    A.cmds.push_back(XF(30 * 4, {1, 0, 0, 0.75f, 0, 1, 0, 0, 0, 0, 1, 0}));
    A.cmds.push_back(BP(0xFE, 0x0000FF)); // mask: only the low byte of the next write
    A.cmds.push_back(BP(0xE1, 0x800000 | 0x0AB22));
    Draw(A, kTri, Trans(0, 0, -11), 2);
    Record(A, 1);
    const ip::DrawRec &d0 = A.rec.draws[0], &d1 = A.rec.draws[1];
    const uint32_t s0 = g_neg_version ? d1.seq : d0.seq;
    const float *row0 = A.rec.At(ip::kRegPos + 30, s0), *row1 = A.rec.At(ip::kRegPos + 30, d1.seq);
    Check(Near(row0[3], 0.25f, 1e-7f) && Near(row1[3], 0.75f, 1e-7f), "L1",
          "matrix rows are read as they were at each draw (later writes in the frame do not leak back)");
    uint32_t k0, k1;
    std::memcpy(&k0, &A.rec.Get(ip::kRegTev, g_neg_version ? d1.v_tev : d0.v_tev)[9], 4);
    std::memcpy(&k1, &A.rec.Get(ip::kRegTev, d1.v_tev)[9], 4);
    // masked write: only bits 0-7 change (0x111 -> 0x122), the bank bit stays from the old value
    Check(k0 == (0x800000u | 0x111u) && k1 == (0x800000u | 0x122u), "L2",
          "TEV konst register per draw, BP mask applied like the renderer (masked write keeps the other bits)");
  }
  { // T1: a scrolling texture matrix (slot 30, one texgen) reaches the in-between draw half-way
    Frame A, B;
    Persp(A); Persp(B);
    float one;
    const uint32_t ntg1 = 1;
    std::memcpy(&one, &ntg1, 4);
    for (Frame *f : {&A, &B}) {
      World(*f, ip::AffIdentity(), 20);
      f->cmds.push_back(XF(0x103F, {one}));
      f->cmds.push_back(XF(30 * 4, {1, 0, 0, f == &A ? 0.2f : 0.4f, 0, 1, 0, 0, 0, 0, 1, 0}));
      Draw(*f, kTri, Trans(0, 0, -10), 6);
      f->raws.back()->defTexMtxIdx[0] = 30;
      for (auto &v : f->cmds.back().vertices) v.texMtxIdx[0] = 30;
    }
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    const ip::DrawRec &d = B.rec.draws[20];
    const GXCommand &bc = B.cmds[d.ci];
    ip::DrawOverride ov;
    ip::PrepareDraw(A.rec, B.rec, P, cfg, 20, bc, &A.cmds[A.rec.draws[d.match].ci], 1, ov);
    float tx = -1;
    for (int i = 0; i < ov.nw; ++i)
      if (ov.w[i].r == ip::kRegPos + 30) tx = ov.w[i].v[3];
    Check(d.tex_slots == (1ull << 30) && (P.dp[20].flags & ip::kDpTex) && Near(tx, 0.3f, 1e-6f), "T1",
          "scrolling texture matrix of a texgen interpolated (0.2 -> 0.3 -> 0.4)");
  }
  { // E1: the parallel preparation (PrepareAll on 4 threads, LoadPrepared) equals PrepareDraw draw by draw
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 700);
    World(B, RotY(3, 1, 0, 0), 700);
    A.cmds.push_back(BP(0xE0, 0x800000 | (10u << 12)));
    B.cmds.push_back(BP(0xE0, 0x800000 | (90u << 12)));
    for (int i = 0; i < 300; ++i) {
      Draw(A, kTri, Trans(0.5f * i, 0, -50), 3000 + i);
      Draw(B, kTri, Trans(0.5f * i + (i % 3), 0, -50), 3000 + i);
    }
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    const ip::ParallelRun par = [](size_t n, size_t g, const std::function<void(size_t, size_t)> &fn) {
      std::vector<std::thread> th;
      std::atomic<size_t> next{0};
      for (int t = 0; t < 4; ++t)
        th.emplace_back([&] { for (size_t b; (b = next.fetch_add(g)) < n;) fn(b, std::min(n, b + g)); });
      for (auto &x : th) x.join();
    };
    ip::PreparedFrame pf;
    ip::PrepareAll(A.rec, B.rec, P, cfg, B.cmds, A.cmds, {}, {}, pf, &par);
    bool same = pf.d.size() == B.rec.draws.size();
    size_t with_writes = 0;
    for (size_t di = 0; di < B.rec.draws.size() && same; ++di) {
      const ip::DrawRec &d = B.rec.draws[di];
      ip::DrawOverride ref, got;
      ip::PrepareDraw(A.rec, B.rec, P, cfg, di, B.cmds[d.ci], d.match >= 0 ? &A.cmds[A.rec.draws[d.match].ci] : nullptr,
                      d.ntg, ref);
      const ip::RegionWrite *w = nullptr;
      int nw = 0;
      ip::LoadPrepared(A.rec, B.rec, P, pf, di, got, &w, &nw);
      same = ref.vmode == got.vmode && ref.kfix == got.kfix && ref.vlerp == got.vlerp && ref.nw == nw &&
             (ref.b == nullptr) == (got.b == nullptr) && (ref.vmode != 1 || !std::memcmp(ref.mv.m, got.mv.m, 48));
      for (int i = 0; i < nw && same; ++i) same = ref.w[i].r == w[i].r && !std::memcmp(ref.w[i].v, w[i].v, 64);
      with_writes += nw > 0;
    }
    // E2: the record from the caller's pre-scan (no reads of draw commands) equals the plain record
    ip::FrameScan scan;
    for (size_t ci = 0; ci < B.cmds.size(); ++ci) {
      const GXCommand &c = B.cmds[ci];
      if (c.type == GXCommandType::DrawPrimitive) { scan.idx.push_back((uint32_t)ci | 0x80000000u); scan.prim.push_back(c.prim_type); }
      else if (c.type != GXCommandType::CPRegister) scan.idx.push_back((uint32_t)ci);
    }
    ip::FrameRec R2;
    gx::GXState st0{};
    ip::BaseState base{};
    ip::CaptureBase(st0, base);
    ip::RecordFrame(B.cmds, B.raws, base, 2, R2, &par, &scan);
    bool eq = R2.draws.size() == B.rec.draws.size() && R2.pool == B.rec.pool;
    for (size_t i = 0; i < R2.draws.size() && eq; ++i) {
      const ip::DrawRec &x = R2.draws[i], &y = B.rec.draws[i];
      eq = x.ci == y.ci && x.seq == y.seq && x.kstrict == y.kstrict && x.kloose == y.kloose && x.kind == y.kind &&
           x.v_tev == y.v_tev && x.v_proj == y.v_proj && !std::memcmp(x.mv.m, y.mv.m, 48);
    }
    Check(eq, "E2", "record from the caller's pre-scan + parallel hashing = plain record");
    Check(same && (with_writes > 0 || !cfg.values), "E1", "parallel per-draw preparation (4 threads, chunked) = PrepareDraw for all 1000 draws");
  }
  { // S1: skinning bank: each bone slot slerped
    std::vector<float> ba(256, 0.0f), bb(256, 0.0f);
    const ip::Aff a0 = Trans(0, 0, -30), a1 = ip::AffMul(Trans(0, 0, -30), RotY(0));
    const ip::Aff b0 = Trans(0, 0, -30), b1 = ip::AffMul(Trans(0, 0, -30), RotY(80));
    std::memcpy(&ba[0], a0.m, 48); std::memcpy(&ba[12], a1.m, 48);
    std::memcpy(&bb[0], b0.m, 48); std::memcpy(&bb[12], b1.m, 48);
    const std::vector<uint8_t> idx = {0, 3};
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 20);
    World(B, ip::AffIdentity(), 20);
    Draw(A, {{{0, 1, 0}}, {{4, 0, 0}}}, ip::AffIdentity(), 9, &ba, &idx);
    Draw(B, {{{0, 1, 0}}, {{4, 0, 0}}}, ip::AffIdentity(), 9, &bb, &idx);
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    const auto v = Interp(A, B, P, cfg, 20);
    const float r = std::sqrt(v[1][0] * v[1][0] + (v[1][2] + 30) * (v[1][2] + 30));
    Check(B.rec.draws[20].kind == ip::kBank && Near(v[0][1], 1.0f) && Near(r, 4.0f, 1e-2f) &&
              Near(v[1][0], 4 * std::cos(0.698132f), 1e-2f),
          "S1", "skinned draw: each bone slot interpolated on its own (an 80-degree bone turns 40)");
  }
  { // Q1 / Q2: K mode: static world needs no CPU vertex work; a new (unmatched) object stays where B has it
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 30);
    World(B, RotY(10, 3, 0, 0), 30);
    Draw(B, kTri, Trans(5, 1, -40), 999); // appears in B
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    bool cpu = true;
    const auto v = Interp(A, B, P, cfg, 0, nullptr, &cpu);
    float want[3];
    const float p0[3] = {1, 0, 0};
    Xf(ip::AffMul(P.Ct, Trans(-30, 0, -100)), p0, want);
    Check(P.kmode && !cpu && Near(v[0][0], want[0], 1e-3f) && Near(v[0][2], want[2], 1e-3f), "Q1",
          "K mode: a static draw keeps B's vertices (no CPU work) and is drawn at the half-moved camera");
    const auto u = Interp(A, B, P, cfg, 30);
    const auto &bu = B.cmds[B.rec.draws[30].ci].vertices;
    Check(B.rec.draws[30].match < 0 && Near(u[0][0], bu[0].pos[0], 1e-3f) && Near(u[0][2], bu[0].pos[2], 1e-3f), "Q2",
          "K mode: an unmatched draw is still drawn at B's position (Kinv cancels K)");
  }
  { // R1: t = 1 in lerp mode reproduces B bit-exactly
    ip::Config c = cfg;
    c.camera_mode = false;
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 20);
    World(B, RotY(5, 1, 2, 3), 20);
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 1.0f, c, nullptr, P);
    bool ok = !P.cut;
    for (size_t di = 0; di < 20 && ok; ++di) {
      const auto v = Interp(A, B, P, c, di);
      const auto &bv = B.cmds[B.rec.draws[di].ci].vertices;
      for (size_t i = 0; i < bv.size(); ++i) ok = ok && std::memcmp(v[i].data(), bv[i].pos, 12) == 0;
    }
    Check(ok, "R1", "t=1 (lerp) reproduces B's vertex positions bit-exactly");
  }
  { // P1: present schedule: guest frames every 33.37 ms, host ready times jitter by up to 6 ms
    gx::PacePlanner pp;
    std::vector<int64_t> shown;
    uint32_t rng = 12345;
    auto jitter = [&] { rng = rng * 1664525u + 1013904223u; return (int64_t)(rng >> 8) % 6000; };
    int64_t prev_shown = 0;
    for (int f = 0; f < 120; ++f) {
      const int64_t cut = 1000000 + (int64_t)f * 33367;
      const int64_t ready_i = cut + 9000 + jitter(), ready_n = ready_i + 11000 + jitter();
      int64_t ti = g_neg_pace ? ready_i : pp.Plan(1, cut, 33367, ready_i);
      ti = std::max({ti, ready_i, prev_shown});
      int64_t tn = g_neg_pace ? ready_n : pp.Plan(2, cut, 33367, ready_n);
      tn = std::max({tn, ready_n, ti});
      prev_shown = tn;
      if (f >= 40) { shown.push_back(ti); shown.push_back(tn); }
    }
    int64_t worst = 0;
    for (size_t i = 1; i < shown.size(); ++i) worst = std::max<int64_t>(worst, std::llabs(shown[i] - shown[i - 1] - 16683));
    std::printf("     P1 worst spacing error %lld us, D %lld us\n", (long long)worst, (long long)pp.D());
    Check(worst <= 1000, "P1", "in-between and real frames are presented P/2 apart (<= 1 ms off) despite 6 ms host jitter");
  }
  { // C5: view-dependent direct particles cannot override the static-world camera.
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 20);
    World(B, RotY(6), 20);
    for (int i = 0; i < 100; ++i) {
      auto pa = kTri, pb = kTri;
      for (auto& v : pa) { v[0] += 5.0f * i; v[2] -= 500.0f; }
      for (auto& v : pb) { v[0] += 5.0f * i + 0.1f; v[2] -= 500.0f; }
      Draw(A, pa, ip::AffIdentity(), 9000 + i);
      Draw(B, pb, ip::AffIdentity(), 9000 + i);
    }
    Record(A, 1); Record(B, 2);
    ip::Plan P;
    ip::MakePlan(A.rec, B.rec, 0.5f, cfg, nullptr, P);
    bool loose = true;
    for (size_t i = 20; i < B.rec.draws.size(); ++i) loose &= B.rec.draws[i].tier == 3;
    Check(loose && P.cam.ok && Near(P.cam.angle_deg, 6, 0.01f), "C5",
          "100 loosely matched direct particles do not outvote 20 reliable world-camera draws");
    const auto v = Interp(A, B, P, cfg, 0);
    float expected[3];
    Xf(ip::AffMul(RotY(3), Trans(-30, 0, -100)), kTri[0].data(), expected);
    Check(Near(v[0][0], expected[0], 0.01f) && Near(v[0][2], expected[2], 0.01f), "C6",
          "static geometry remains at the half-turned camera even with a dominant particle burst");
  }
  {
    Frame A, B;
    Persp(A); Persp(B);
    World(A, ip::AffIdentity(), 20);
    World(B, RotY(6), 20);
    std::vector<std::array<float,3>> pa = {{{-10,-10,0}},{{10,-10,0}},{{0,10,0}}};
    auto pb = pa;
    for (auto& v : pb) v[0] += 5;
    Draw(A,pa,Trans(0,0,-100),88888);
    Draw(B,pb,ip::AffMul(RotY(6),Trans(0,0,-100)),88888);
    Record(A,1); Record(B,2);
    ip::Plan P;
    ip::MakePlan(A.rec,B.rec,0.5f,cfg,nullptr,P);
    ip::DrawOverride ov;
    const auto v = Interp(A,B,P,cfg,20,&ov);
    float expected[3];
    Xf(ip::AffMul(RotY(3),Trans(0,0,-100)),pb[0].data(),expected);
    Check(B.rec.draws[20].tier==3 && !ov.vlerp && Near(v[0][0],expected[0],0.01f),"C7",
          "ambiguous direct effect keeps its current geometry/world anchor while the camera interpolates");
    // Stable indexed geometry can still animate its vertices between frames.
    B.rec.draws[20].direct_pos=false;
    Interp(A,B,P,cfg,20,&ov);
    Check(ov.vlerp,"C8","indexed geometry retains vertex interpolation");
  }
  { // Repeated state writes retain per-draw values without duplicate history.
    Frame f;
    const auto live = [&] {
      GXCommand c{}; c.type = GXCommandType::DrawPrimitive; c.prim_type = 0x90;
      f.cmds.push_back(c); f.raws.push_back(nullptr);
    };
    live();
    for (int i = 0; i < 1000; ++i) { f.cmds.push_back(BP(0xE0, 0)); live(); }
    f.cmds.push_back(BP(0xE0, 123)); live();
    f.cmds.push_back(BP(0xE0, 123)); live();
    f.cmds.push_back(BP(0xE0, 0)); live();
    f.cmds.push_back(XF(0, {-0.0f})); live();
    uint32_t nanbits = 0x7fc01234; float nan;
    std::memcpy(&nan, &nanbits, 4);
    f.cmds.push_back(XF(0, {nan})); live();
    f.cmds.push_back(XF(0, {nan})); live();
    Record(f, 1);
    bool ok = true;
    for (size_t i = 0; i < f.rec.draws.size(); ++i) {
      uint32_t color = 0;
      std::memcpy(&color, f.rec.AtFixed(ip::kRegTev, f.rec.draws[i].seq), 4);
      ok &= color == ((i == 1001 || i == 1002) ? 123u : 0u);
    }
    Check(ok, "H1", "duplicate writes and value reversions preserve every draw's colour");
    Check(f.rec.hist[ip::kRegTev].size() == 3 && f.rec.hist[ip::kRegNrm].size() == 1,
          "H2", "unchanged colours and live matrix rows reuse history storage");
    uint32_t zbits = 0, nbits = 0;
    std::memcpy(&zbits, f.rec.AtFixed(ip::kRegPos, f.rec.draws[1004].seq), 4);
    std::memcpy(&nbits, f.rec.AtFixed(ip::kRegPos, f.rec.draws.back().seq), 4);
    Check(zbits == 0x80000000 && nbits == nanbits && f.rec.hist[ip::kRegPos].size() == 3,
          "H3", "history retains signed zero and exact NaN payloads");
    Record(f, 2);
    Check(f.rec.hist[ip::kRegTev].size() == 3 && f.rec.frame == 2,
          "H4", "history reuse starts from the new frame's base state");
  }
  {
    Check(gx::InterpWorkBudgetUs(50000,95)==31698 && gx::InterpWorkBudgetUs(33367,95)==31698,
          "B1", "a slowdown to 20 fps cannot enlarge the optional-work allowance");
    Check(gx::InterpWorkBudgetUs(16667,95)==15833,
          "B2", "faster game cadence retains its smaller work budget");
    Check(gx::InterpWorkBudgetUs(33367,0)==25025 && gx::InterpWorkBudgetUs(33367,100)==31698,
          "B3", "work-budget percentages stay within supported bounds");
    Check(gx::InterpPreparationCoolingDown(1000,2000,true,false) &&
          !gx::InterpPreparationCoolingDown(2000,2000,true,false),
          "B4", "preparation is skipped until the retry deadline, then allowed again");
    Check(!gx::InterpPreparationCoolingDown(1000,2000,false,false) &&
          !gx::InterpPreparationCoolingDown(1000,2000,true,true),
          "B5", "other interpolation modes and explicit forced tests retain their behavior");
    Check(gx::InterpRetryDelayUs(-10,133333)==133333 &&
          gx::InterpRetryDelayUs(10000,133333)==213333 &&
          gx::InterpRetryDelayUs(INT64_MAX,133333)==500000,
          "B6", "retry backoff is bounded and cannot overflow on a large stall");
  }
  std::printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
