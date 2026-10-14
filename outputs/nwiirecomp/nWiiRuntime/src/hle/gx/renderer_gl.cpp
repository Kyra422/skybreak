#include "runtime/cpu_context.h"
#include "runtime/gx/fifo_parser.h"
#include "runtime/gx/renderer.h"
#include "runtime/gx/gl_shader_compile.h"
#include "runtime/gx/gl_texture_sampler.h"
#include "runtime/gx/gx_capture.h"
#include "runtime/gx/interp_render.h" // INTERP17
#include "runtime/gx/frame_motion.h"  // MVGEN21: warp shader + flags
#include "runtime/gx/tev_shader_gen.h"
#include "runtime/gx_state.h"
#include "runtime/texture_cache_gl.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <glad/glad.h>
#include <SDL_video.h> // PERF21: SDL_GL_GetProcAddress (glBufferStorage, GL 4.4)
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h> // PERF21: _mm_prefetch
#define PERF21_PREFETCH 1
#endif

// RENDER1 (roadmap M1.2-M1.5, M1.9/M1.10 basics). The renderer walks one cut
// frame of GX commands in stream order:
//  * BP writes go through ApplyBPRegister (BP mask, render-time register file);
//  * state is tracked per category with dirty bits and value comparison, and a
//    draw batch is flushed only when the GL-visible state really changes
//    (program, PE output state, viewport/scissor, bound textures, uniforms);
//  * BP 0x52 executes the EFB copy (texture or XFB) and then the copy-clear,
//    exactly where it sits in the stream (Dolphin BPFunctions::ClearScreen);
//  * the XFB copy is scaled into a 640x480 present framebuffer, which is what
//    the window and the screenshots show.
// References: Dolphin VideoCommon BPFunctions.cpp (scissor/viewport/clear),
// RenderState.cpp (BlendingState::Generate), BPMemory.h (register layouts),
// TextureConverterShaderGen.cpp (EFB copy formats).

namespace nwii::runtime {
extern MMU *g_mmu;
extern CPUContext *g_ctx_ptr;
} // namespace nwii::runtime

namespace nwii::runtime::hle {
// texture_cache_gl.cpp: bumped by TextureCache::clear() (its GL names die).
extern uint64_t g_texture_cache_generation;
// texture_cache_gl.cpp: runs a deferred over-capacity clear between frames.
void TextureCacheFrameBegin();
} // namespace nwii::runtime::hle

void GX_GetClearColor(unsigned char rgba[4]);

namespace nwii::runtime::gx {

static bool gx_trace() {
  static bool t = std::getenv("NWII_GXTRACE") != nullptr;
  return t;
}

static RendererStats s_stats;
const RendererStats &GetRendererStats() { return s_stats; }

// ---------------------------------------------------------------------------
// Shader programs and uniform groups
// ---------------------------------------------------------------------------
enum UGroup { UG_PROJ, UG_CHAN, UG_LIGHT, UG_TEV, UG_DSTA, UG_IND, UG_FOG, UG_COUNT };

struct GLShader {
  GLuint id = 0;
  GLint uTex[8];
  GLint uTevKColor = -1, uTevColor = -1, uProjMtx = -1;
  GLint uMatColor = -1, uAmbColor = -1, uChanCtrl = -1;
  GLint uLightCol = -1, uLightPos = -1, uLightDir = -1, uLightCosAtt = -1,
        uLightDistAtt = -1;
  GLint uDstAlpha = -1;
  GLint uIndMtx = -1, uTexDim = -1; // indirect texturing (LIGHT7)
  GLint uFogF = -1, uFogI = -1, uFogRange = -1; // fog (TEX7)
  uint32_t ver[UG_COUNT] = {}; // uniform-group versions last uploaded
  GLint uMvVp = -1;            // MVGEN21: motion variant only: half the viewport size (px)
  float mv_vp[2] = {-1, -1};
};
// Node-based map: element addresses stay valid across inserts.
static std::unordered_map<uint64_t, GLShader> s_shader_cache;
static GLShader *s_active = nullptr; // program used by the open batch
static GLuint s_bound_prog = 0;

uint64_t g_stat_hash0 = 0;
uint64_t g_stat_shaders = 0;

struct UniformValues {
  float proj[16];
  float mat[8], amb[8];
  GLint cc[4];
  float lcol[32], lpos[32], ldir[32], lcos[32], ldist[32];
  float tev[16], ktev[16];
  float dsta[4];
  GLint indm[24];  // UG_IND: BP 0x06-0x0E as Dolphin I_INDTEXMTX rows
  float tdim[32];  // UG_IND: texmap size (xy), texcoord scale (zw)
  float fogf[4];   // UG_FOG: A, C, range centre, 2*vp.wd (EFB px)
  GLint fogi[4];   // UG_FOG: colour 0xRRGGBB, B magnitude, -, B shift
  float fogr[12];  // UG_FOG: range-adjust k[0..9]
};
static UniformValues s_uv;
static uint32_t s_uver[UG_COUNT] = {1, 1, 1, 1, 1, 1, 1};

// ---------------------------------------------------------------------------
// Vertex batch
// ---------------------------------------------------------------------------
// tc[i] = XF texgen output i as (s, t, q); the fragment shader divides by q
// (Dolphin PixelShaderGen: xy / z unless z == 0). Attribute 2 = tc[0],
// attributes 4..10 = tc[1..7] (M1.8).
struct BatchVertex {
  float x, y, z;
  float r, g, b, a;
  float nx, ny, nz;
  float tc[8][3];
};
static_assert(offsetof(BatchVertex, tc) == 10 * sizeof(float) &&
                  sizeof(BatchVertex) == 34 * sizeof(float),
              "BatchVertex must be 34 packed floats (the batch copies prefixes)");

// The batch stores only the texcoords a draw generates: each vertex is the
// first 10 + 3k floats of a BatchVertex, k = max(1, XF numTexGen). Attributes
// of coordinates >= k point at tc[0] -- exactly the "texcoord >= numTexGens
// samples texcoord 0" quirk TexGen() implements -- so the 8-coordinate shader
// interface costs no upload bandwidth (a full BatchVertex is 136 bytes).
// PERF21: the open batch holds every GX vertex ONCE (s_batch, BatchFloats(s_batch_k) floats each) and
// the triangle list as indices into it (s_bidx, relative to the batch start), drawn with
// glDrawElementsBaseVertex. Before, every triangle corner was a copy of its vertex (a quad 6 copies, a
// strip 3 per triangle: 1.5-3x the bytes converted, copied and uploaded). The triangles, their order
// and every vertex value are unchanged, so the image is too (gxreplay A/B: work/claude-notes/PERF21.md).
// PodBuf: grows without std::vector's value-initialisation (ConvertVertex writes every float anyway).
template <class T> struct PodBuf {
  T *p = nullptr;
  size_t n = 0, cap = 0;
  T *grow(size_t add) {
    if (n + add > cap) {
      const size_t nc = std::max<size_t>(cap * 2, std::max<size_t>(n + add, 1u << 16));
      T *q = static_cast<T *>(std::malloc(nc * sizeof(T)));
      if (!q)
        std::abort();
      if (n)
        std::memcpy(q, p, n * sizeof(T));
      std::free(p);
      p = q;
      cap = nc;
    }
    T *r = p + n;
    n += add;
    return r;
  }
  void clear() { n = 0; }
  bool empty() const { return n == 0; }
  size_t size() const { return n; }
  T *data() { return p; }
};
static PodBuf<float> s_batch;     // PERF21: unique converted vertices of the open batch
static PodBuf<uint32_t> s_bidx;   // PERF21: its triangle list (vertex indices from the batch start)
static uint32_t s_batch_nv = 0;   // PERF21: vertices in s_batch
static int s_batch_k = 1; // texcoords per vertex in s_batch
static int s_vao_k = 0;   // layout the VAO's attribute pointers describe
static float s_proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static GLuint s_vao = 0, s_vbo = 0;
static size_t s_vbo_cap = 0, s_vbo_pos = 0; // in bytes
static GLuint s_ibo = 0;                    // PERF21: index ring (VAO element buffer)
static size_t s_ibo_cap = 0, s_ibo_pos = 0; // in bytes
static GLuint s_texture_samplers[8] = {};

static inline size_t BatchFloats(int k) { return 10 + 3 * (size_t)k; }

// MVGEN21: motion vectors of a real frame (runtime/gx/frame_motion.h; the MVGEN21 block at the end of this file).
// While the pipeline arms a real frame, perspective batches that write depth and are not blended draw with a
// motion-variant program into a second colour attachment of the EFB FBO, and each of their batch vertices
// carries 3 more floats (at BatchFloats(k)): the previous frame's view-space position (MotionHook::Prev).
// Unarmed (interpolation off, rerender/dup modes) none of this runs: layout, programs and draw buffers as before.
struct MvState {
  bool armed = false;      // the next real Render() records (MotionArm)
  bool recording = false;  // inside that Render(), before the 2D boundary
  bool attached = false;   // a motion texture is attached to the EFB FBO as COLOR_ATTACHMENT1
  bool unsupported = false;// a motion program failed to compile/link: never again this session
  bool db_two = false;     // the EFB FBO's draw buffers are {COLOR0, COLOR1}
  bool batch_on = false;   // the bound program writes motion (the open batch has the +3 layout)
  MotionArm arm{};
  float cur_proj[16] = {};  // projection + viewport of the open motion batch (set by SyncState)
  int cur_vp[4] = {};
  float best_proj[16] = {}; // ... of the frame's largest motion batch (the background motion's camera)
  int best_vp[4] = {};
  size_t best_n = 0;
  uint64_t batches = 0, fallbacks = 0, db_switches = 0;
  int compiles = 0; // motion-variant programs compiled in this real frame
};
static MvState s_mv;
static bool s_batch_mv = false; // s_batch layout carries the +3 motion floats
static bool s_vao_mv = false;   // ... and so do the VAO's attribute pointers
static size_t s_interp_begin = 0; // MVGEN21: in-between walks skip the commands before this index (warp tail)
static constexpr uint64_t kMvUidSalt = 0x4D5647454E32315Full; // motion-variant program key = uid ^ salt
static void MvDrawBuffers(bool two); // the EFB FBO's draw buffers (motion attachment in or out)
static void MvBeforeDraw();          // FlushBatch: motion state right before a motion batch's draw call
static bool MvInject(std::string &vs, std::string &fs); // TEV program source -> motion variant
// PERF21: floats per vertex in s_batch for texcoord count k (+3 with the MVGEN21 motion layout): the stride
// of ConfigureVao, FlushBatch and EmitPrimitive in one place.
static inline size_t BatchStride(int k) { return BatchFloats(k) + (s_batch_mv ? 3 : 0); }
static void MvFrameStart(); // real Render(): armed -> attach + clear the motion texture, recording on
static void MvBoundary();   // real Render() at the 2D boundary: EFB + render-state snapshot, recording off
static void MvFrameEnd();   // real Render() end: draw buffers back to {COLOR0}, disarm

static void ConfigureVao(int k) {
  const GLsizei stride = (GLsizei)(BatchStride(k) * sizeof(float)); // MVGEN21: +3 (PERF21: BatchStride)
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(BatchVertex, x));
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(BatchVertex, r));
  glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(BatchVertex, nx));
  for (int i = 0; i < 8; ++i) {
    const GLuint loc = i == 0 ? 2u : (GLuint)(3 + i);
    const int src = i < k ? i : 0;
    glVertexAttribPointer(loc, 3, GL_FLOAT, GL_FALSE, stride,
                          (void *)(offsetof(BatchVertex, tc) + src * 3 * sizeof(float)));
  }
  if (s_batch_mv) { // MVGEN21: attribute 11 = previous view-space position (motion layout only)
    glVertexAttribPointer(11, 3, GL_FLOAT, GL_FALSE, stride, (void *)(BatchFloats(k) * sizeof(float)));
    glEnableVertexAttribArray(11);
  } else if (s_vao_mv) {
    glDisableVertexAttribArray(11);
  }
  s_vao_mv = s_batch_mv;
  s_vao_k = k;
}

// PERF21: GL 4.4 persistent mapped stream rings (ARB_buffer_storage) when the context offers them:
// FlushBatch copies the batch and its indices into coherent mappings, with no map/unmap/orphan call
// per flush (four driver calls fewer per batch). One fence per 1/8 of a ring, set when the write
// position leaves that eighth (after the draws that read it), is waited on before the eighth is
// written again one lap later. The GL 3.3 path (unsynchronised map ring, orphaned on wrap) stays the
// fallback; NWII_GL_PERSISTENT=0 forces it. Same bytes reach the GPU either way.
struct PersistRing {
  static constexpr int kSeg = 8;
  GLuint buf = 0;
  uint8_t *ptr = nullptr;
  size_t cap = 0, pos = 0;
  GLsync fence[kSeg] = {};
  size_t Seg() const { return cap / kSeg; }
  void Wait(size_t s) {
    if (!fence[s])
      return;
    while (glClientWaitSync(fence[s], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) == GL_TIMEOUT_EXPIRED) {
    }
    glDeleteSync(fence[s]);
    fence[s] = nullptr;
  }
  void FenceSegs(size_t s0, size_t s1) { // segments [s0, s1)
    for (size_t s = s0; s < s1 && s < (size_t)kSeg; ++s) {
      if (fence[s])
        glDeleteSync(fence[s]);
      fence[s] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
  }
  // An offset (multiple of align) where `bytes` (> 0) can be written now.
  size_t Reserve(size_t bytes, size_t align) {
    size_t p = (pos + align - 1) / align * align;
    if (p + bytes > cap) { // wrap: the rest of this lap is done
      FenceSegs(pos / Seg(), kSeg);
      pos = 0;
      p = 0;
    }
    for (size_t s = p / Seg(); s <= (p + bytes - 1) / Seg(); ++s)
      Wait(s);
    return p;
  }
  // After the draw reading [p, p + bytes) was issued.
  void Commit(size_t p, size_t bytes) {
    const size_t s_old = pos / Seg(), end = p + bytes;
    const size_t s_new = end / Seg();
    if (s_new > s_old)
      FenceSegs(s_old, s_new);
    pos = end;
  }
};
static bool s_persist = false;
static PersistRing s_pv, s_pi; // vertices, indices
typedef void(APIENTRYP PFN_PERF21_BufferStorage)(GLenum, GLsizeiptr, const void *, GLbitfield);
static PFN_PERF21_BufferStorage s_glBufferStorage = nullptr;

static bool PersistAvailable() {
  const char *e = std::getenv("NWII_GL_PERSISTENT");
  if (e && *e == '0')
    return false;
  GLint maj = 0, mnr = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &maj);
  glGetIntegerv(GL_MINOR_VERSION, &mnr);
  bool ok = maj > 4 || (maj == 4 && mnr >= 4);
  if (!ok) {
    GLint ne = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &ne);
    for (GLint i = 0; i < ne && !ok; ++i) {
      const char *x = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
      ok = x && std::strcmp(x, "GL_ARB_buffer_storage") == 0;
    }
  }
  if (!ok)
    return false;
  s_glBufferStorage = (PFN_PERF21_BufferStorage)SDL_GL_GetProcAddress("glBufferStorage");
  return s_glBufferStorage != nullptr;
}

static bool PersistCreate(PersistRing &r, GLenum target, size_t cap) {
  constexpr GLbitfield kPersistent = 0x0040, kCoherent = 0x0080; // GL_MAP_PERSISTENT/COHERENT_BIT
  glGenBuffers(1, &r.buf);
  glBindBuffer(target, r.buf);
  s_glBufferStorage(target, (GLsizeiptr)cap, nullptr, GL_MAP_WRITE_BIT | kPersistent | kCoherent);
  r.ptr = (uint8_t *)glMapBufferRange(target, 0, (GLsizeiptr)cap, GL_MAP_WRITE_BIT | kPersistent | kCoherent);
  r.cap = cap;
  r.pos = 0;
  if (!r.ptr) {
    glDeleteBuffers(1, &r.buf);
    r.buf = 0;
    return false;
  }
  return true;
}

static void EnsureVao() {
  if (s_vao)
    return;
  glGenVertexArrays(1, &s_vao);
  glBindVertexArray(s_vao);
  s_persist = PersistAvailable();
  if (s_persist) {
    // vertex ring first (the attribute pointers name the bound array buffer), then the index ring
    s_persist = PersistCreate(s_pv, GL_ARRAY_BUFFER, 64u << 20);
    if (s_persist && !PersistCreate(s_pi, GL_ELEMENT_ARRAY_BUFFER, 16u << 20)) {
      glBindBuffer(GL_ARRAY_BUFFER, s_pv.buf);
      glUnmapBuffer(GL_ARRAY_BUFFER);
      glDeleteBuffers(1, &s_pv.buf);
      s_pv = PersistRing{};
      s_persist = false;
    }
  }
  std::printf("[PERF21] vertex streaming: %s\n",
              s_persist ? "persistent mapped rings (GL 4.4)" : "map ring (GL 3.3)");
  if (s_persist) {
    s_vbo = s_pv.buf;
    s_ibo = s_pi.buf;
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
  } else {
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    s_vbo_cap = 32u << 20;
    s_vbo_pos = 0;
    glBufferData(GL_ARRAY_BUFFER, s_vbo_cap, nullptr, GL_STREAM_DRAW);
  }
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(3);
  for (int i = 0; i < 8; ++i)
    glEnableVertexAttribArray(i == 0 ? 2u : (GLuint)(3 + i));
  ConfigureVao(1);
  if (!s_persist) {
    // PERF21: index ring, bound to the VAO (element buffer binding is VAO state)
    glGenBuffers(1, &s_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
    s_ibo_cap = 8u << 20;
    s_ibo_pos = 0;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, s_ibo_cap, nullptr, GL_STREAM_DRAW);
  } else {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
  }
}

// Streams the batch into a ring VBO (unsynchronised map of a never-used
// range, orphaned when it wraps) and draws it with the current GL state.
// PERF21: + its indices into a ring IBO; one glDrawElementsBaseVertex. With persistent rings
// (s_persist) both uploads are plain memcpys into the mappings.
static void FlushBatch() {
  if (s_bidx.empty()) {
    s_batch.clear(); // vertices of draws without a whole triangle
    s_batch_nv = 0;
    return;
  }
  EnsureVao();
  glBindVertexArray(s_vao);
  const size_t stride = BatchStride(s_batch_k) * sizeof(float); // MVGEN21: +3 (PERF21: BatchStride)
  const size_t bytes = s_batch.size() * sizeof(float);
  const size_t ni = s_bidx.size(), ibytes = ni * sizeof(uint32_t);
  size_t first = 0, ioff = 0; // base vertex, index byte offset
  if (s_persist) {
    if (bytes <= s_pv.cap / 4 && ibytes <= s_pi.cap / 4) {
      const size_t vp = s_pv.Reserve(bytes, stride); // whole vertices: the base vertex addresses it
      ioff = s_pi.Reserve(ibytes, sizeof(uint32_t));
      std::memcpy(s_pv.ptr + vp, s_batch.data(), bytes);
      std::memcpy(s_pi.ptr + ioff, s_bidx.data(), ibytes);
      first = vp / stride;
    } else {
      // A batch larger than a quarter ring (not seen in practice): wait for the GPU, restart the lap.
      glFinish();
      for (int sg = 0; sg < PersistRing::kSeg; ++sg) {
        s_pv.Wait((size_t)sg);
        s_pi.Wait((size_t)sg);
      }
      s_pv.pos = s_pi.pos = 0;
      if (bytes > s_pv.cap || ibytes > s_pi.cap) {
        std::fprintf(stderr, "[PERF21] batch of %zu bytes does not fit the stream ring: dropped\n", bytes);
        s_batch.clear();
        s_bidx.clear();
        s_batch_nv = 0;
        return;
      }
      std::memcpy(s_pv.ptr, s_batch.data(), bytes);
      std::memcpy(s_pi.ptr, s_bidx.data(), ibytes);
    }
  } else {
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    // Start on a whole vertex of this stride so the base vertex `first`
    // addresses it and the attribute pointers stay at offset 0.
    first = (s_vbo_pos + stride - 1) / stride;
    if (first * stride + bytes > s_vbo_cap) {
      if (bytes > s_vbo_cap)
        s_vbo_cap = std::max(bytes, s_vbo_cap * 2);
      glBufferData(GL_ARRAY_BUFFER, s_vbo_cap, nullptr, GL_STREAM_DRAW);
      first = 0;
    }
    void *dst = glMapBufferRange(
        GL_ARRAY_BUFFER, first * stride, bytes,
        GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
    if (dst) {
      std::memcpy(dst, s_batch.data(), bytes);
      glUnmapBuffer(GL_ARRAY_BUFFER);
    } else {
      glBufferSubData(GL_ARRAY_BUFFER, first * stride, bytes, s_batch.data());
    }
    // PERF21: indices (relative to the batch start; `first` is the base vertex)
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
    if (s_ibo_pos + ibytes > s_ibo_cap) {
      if (ibytes > s_ibo_cap)
        s_ibo_cap = std::max(ibytes, s_ibo_cap * 2);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, s_ibo_cap, nullptr, GL_STREAM_DRAW);
      s_ibo_pos = 0;
    }
    ioff = s_ibo_pos;
    void *idst = glMapBufferRange(
        GL_ELEMENT_ARRAY_BUFFER, ioff, ibytes,
        GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
    if (idst) {
      std::memcpy(idst, s_bidx.data(), ibytes);
      glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    } else {
      glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, ioff, ibytes, s_bidx.data());
    }
  }
  if (s_vao_k != s_batch_k || s_vao_mv != s_batch_mv) { // MVGEN21: or the motion layout changed
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);                // (attribute pointers name the bound buffer)
    ConfigureVao(s_batch_k);
  }
  if (s_batch_mv) // MVGEN21: draw buffers {COLOR0, COLOR1} + motion mask; else the motion attachment stays out
    MvBeforeDraw();
  else if (s_mv.db_two)
    MvDrawBuffers(false);
  glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)ni, GL_UNSIGNED_INT, (void *)ioff, (GLint)first);
  if (s_persist) {
    s_pv.Commit(first * stride, bytes);
    s_pi.Commit(ioff, ibytes);
  } else {
    s_vbo_pos = first * stride + bytes;
    s_ibo_pos = ioff + ibytes;
  }
  ++s_stats.flushes;
  s_stats.vertices += ni; // triangle corners, as before (the glDrawArrays count)
  static const bool s_gxtrace2 = std::getenv("NWII_GXTRACE") != nullptr;
  if (s_gxtrace2) {
    static int en = 0;
    if (en++ < 8)
      printf("[GXTRACE] draw: prog=%u glErr=0x%x verts=%zu\n", s_bound_prog,
             glGetError(), ni);
  }
  s_batch.clear();
  s_bidx.clear();
  s_batch_nv = 0;
}

// ---------------------------------------------------------------------------
// EFB / present / copy targets
// ---------------------------------------------------------------------------
static GLuint s_efb_fbo = 0;             // draw FBO bound by the host
static GLuint s_efb_seen_fbo = 0xFFFFFFFFu;
static GLuint s_efb_color_tex = 0;       // its colour texture (copy source)
static GLuint s_efb_depth_tex = 0;       // its depth texture (Z copies), if any
static int s_efb_w = kEfbWidth, s_efb_h = kEfbHeight; // physical (scaled) size
// RES9 (M7.3): internal resolution scale s of the bound EFB (physical size =
// GX size * s). GX coordinates (viewport, scissor, copy rects) stay in EFB px
// and are multiplied by s where they meet GL; s_efb_lw/lh = GX-sized EFB.
static int s_scale = 1;
static int s_efb_lw = kEfbWidth, s_efb_lh = kEfbHeight;
static GLuint s_present_fbo = 0, s_present_tex = 0;
static int s_present_w = kPresentWidth, s_present_h = kPresentHeight;
static GLuint s_copy_fbo = 0, s_copy_prog = 0, s_empty_vao = 0;
static GLint s_cu_efb = -1, s_cu_rect = -1, s_cu_size = -1, s_cu_scale = -1,
             s_cu_fmt = -1, s_cu_int = -1, s_cu_alpha = -1, s_cu_depth = -1,
             s_cu_zsrc = -1, s_cu_fon = -1, s_cu_filter = -1, s_cu_rowstep = -1,
             s_cu_clampy = -1, s_cu_gamma = -1;
static constexpr int kUploadUnit = 15; // texture-cache uploads bind here
static constexpr int kCopyUnit = 14;   // EFB colour texture for copies
static constexpr int kCopyZUnit = 13;  // EFB depth texture for Z copies
static std::unordered_map<GLuint, std::pair<int, int>> s_copy_tex_size;

static void QueryEfbTarget() {
  GLint fb = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
  s_efb_fbo = (GLuint)fb;
  if (s_efb_fbo == s_efb_seen_fbo)
    return;
  s_efb_seen_fbo = s_efb_fbo;
  s_efb_color_tex = 0;
  s_efb_depth_tex = 0;
  s_efb_w = kEfbWidth;
  s_efb_h = kEfbHeight;
  s_scale = 1;
  s_efb_lw = s_efb_w;
  s_efb_lh = s_efb_h;
  if (!fb)
    return;
  GLint type = 0, name = 0;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
  GLint w = 0, h = 0;
  if (type == GL_TEXTURE && name) {
    s_efb_color_tex = (GLuint)name;
    glActiveTexture(GL_TEXTURE0 + kUploadUnit);
    glBindTexture(GL_TEXTURE_2D, (GLuint)name);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
  } else if (type == GL_RENDERBUFFER && name) {
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)name);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &w);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h);
  }
  if (w > 0 && h > 0) {
    s_efb_w = w;
    s_efb_h = h;
  }
  type = name = 0;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
  if (type == GL_TEXTURE && name)
    s_efb_depth_tex = (GLuint)name;
  // RES9: an EFB whose width is an exact multiple of 640 is an internal-
  // resolution EFB (640s x 528s); anything else (e.g. an old 640x480 FBO) is
  // scale 1 with its own size, as before.
  if (s_efb_w > kEfbWidth && s_efb_w % kEfbWidth == 0) {
    const int s = s_efb_w / kEfbWidth;
    if (s <= kMaxInternalScale && s_efb_h % s == 0)
      s_scale = s;
  }
  s_efb_lw = s_efb_w / s_scale;
  s_efb_lh = s_efb_h / s_scale;
  std::printf("[GX] EFB target fbo=%d %dx%d colour_tex=%u scale=%d\n", fb, s_efb_w,
              s_efb_h, s_efb_color_tex, s_scale);
}

// The present framebuffer follows the internal scale (640s x 480s); it is
// (re)allocated and cleared to black when the scale changes.
static void EnsurePresentTarget() {
  const int pw = kPresentWidth * s_scale, ph = kPresentHeight * s_scale;
  if (s_present_fbo && pw == s_present_w && ph == s_present_h)
    return;
  if (!s_present_fbo) {
    glGenTextures(1, &s_present_tex);
    glGenFramebuffers(1, &s_present_fbo);
  }
  s_present_w = pw;
  s_present_h = ph;
  glActiveTexture(GL_TEXTURE0 + kUploadUnit);
  glBindTexture(GL_TEXTURE_2D, s_present_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_present_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         s_present_tex, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_efb_fbo);
}

int InternalScale() { return s_scale; }

unsigned PresentFramebuffer() { return s_present_fbo; }

void PresentSize(int &w, int &h) {
  w = s_present_w;
  h = s_present_h;
}

bool ReadPresentedFull(std::vector<uint8_t> &rgba, int &w, int &h) {
  w = s_present_w;
  h = s_present_h;
  rgba.assign((size_t)w * h * 4, 0);
  if (!s_present_fbo)
    return false;
  GLint prev = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_present_fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev);
  return true;
}

bool ReadPresented(std::vector<uint8_t> &rgba) {
  if (s_present_w == kPresentWidth && s_present_h == kPresentHeight) {
    int w = 0, h = 0;
    return ReadPresentedFull(rgba, w, h);
  }
  // RES9: s x s box filter down to 640x480 (rounded mean per channel).
  std::vector<uint8_t> full;
  int fw = 0, fh = 0;
  const bool ok = ReadPresentedFull(full, fw, fh);
  rgba.assign((size_t)kPresentWidth * kPresentHeight * 4, 0);
  if (!ok)
    return false;
  const int sx = fw / kPresentWidth, sy = fh / kPresentHeight;
  const uint32_t n = (uint32_t)(sx * sy), half = n / 2;
  for (int y = 0; y < kPresentHeight; ++y)
    for (int x = 0; x < kPresentWidth; ++x) {
      uint32_t acc[4] = {0, 0, 0, 0};
      for (int j = 0; j < sy; ++j) {
        const uint8_t *p = &full[(((size_t)(y * sy + j)) * fw + (size_t)x * sx) * 4];
        for (int i = 0; i < sx; ++i, p += 4) {
          acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
        }
      }
      uint8_t *d = &rgba[((size_t)y * kPresentWidth + x) * 4];
      for (int c = 0; c < 4; ++c)
        d[c] = (uint8_t)((acc[c] + half) / n);
    }
  return true;
}

// EFB -> texture copy shader (Dolphin TextureConverterShaderGen semantics for
// the colour formats): texel row 0 = top row of the source rect, optional
// 2x2 box half-scale, YUV for intensity copies, per-format channel routing and
// quantisation, alpha = 1 when the EFB pixel format has no alpha.
static const char *kCopyVS = R"(#version 330 core
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
static const char *kCopyFS = R"(#version 330 core
uniform sampler2D uEfb;
uniform ivec2 uRect;     // GX source x, y (top-left origin)
uniform ivec2 uEfbSize;
uniform int uScale;      // 1, or 2 for half scale
uniform int uFmt;        // EFBCopyFormat (tp_realFormat)
uniform int uIntensity;
uniform int uHasAlpha;
uniform int uDepth;      // EFB pixel format Z24: copy depth, not colour
uniform sampler2D uEfbZ;
// PEEK10 (M3.1): vertical copy filter, BP 0x53/0x54 as Dolphin's
// GetRAMCopyFilterCoefficients (upper = w0+w1, middle = w2+w3+w4, lower =
// w5+w6, /64). uFilterOn = 0 is the identity (0, 1, 0): no extra taps, so
// such copies are bit-identical to before. Taps are one native row (uRowStep
// = internal scale) above/below, clamped to uClampY (GX rows, scaled): the
// source rect's rows when BP 0x52 clamp top/bottom are set, else the EFB.
uniform int uFilterOn;
uniform vec3 uFilter;
uniform int uRowStep;
uniform ivec2 uClampY;
uniform float uGammaRcp; // XFB copies (uFmt 16): 1 / {1.0, 1.7, 2.2, 1.0}[gamma]
out vec4 o;
vec4 efb(int x, int y) {
  x = clamp(x, 0, uEfbSize.x - 1);
  y = clamp(y, 0, uEfbSize.y - 1);
  return texelFetch(uEfb, ivec2(x, uEfbSize.y - 1 - y), 0);
}
// Filtered colour at GX (x, y); the filter applies to RGB only (Dolphin
// TextureConverterShaderGen: alpha is the centre row's).
vec4 tap(int x, int y) {
  vec4 c = efb(x, y);
  if (uFilterOn == 0) return c;
  vec4 u = efb(x, clamp(y - uRowStep, uClampY.x, uClampY.y));
  vec4 l = efb(x, clamp(y + uRowStep, uClampY.x, uClampY.y));
  return vec4(min(u.rgb * uFilter.x + c.rgb * uFilter.y + l.rgb * uFilter.z, vec3(1.0)), c.a);
}
float ztap(int x, int y) {
  int cx = clamp(x, 0, uEfbSize.x - 1);
  float z = texelFetch(uEfbZ, ivec2(cx, uEfbSize.y - 1 - clamp(y, 0, uEfbSize.y - 1)), 0).r;
  if (uFilterOn == 0) return z;
  int yu = clamp(clamp(y - uRowStep, uClampY.x, uClampY.y), 0, uEfbSize.y - 1);
  int yl = clamp(clamp(y + uRowStep, uClampY.x, uClampY.y), 0, uEfbSize.y - 1);
  float zu = texelFetch(uEfbZ, ivec2(cx, uEfbSize.y - 1 - yu), 0).r;
  float zl = texelFetch(uEfbZ, ivec2(cx, uEfbSize.y - 1 - yl), 0).r;
  return min(zu * uFilter.x + z * uFilter.y + zl * uFilter.z, 1.0);
}
float q(float v, int bits) {
  float raw = floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5);
  return floor(raw / float(1 << (8 - bits))) / float((1 << bits) - 1);
}
void main() {
  ivec2 d = ivec2(gl_FragCoord.xy);
  int sx = uRect.x + d.x * uScale;
  int sy = uRect.y + d.y * uScale;
  if (uDepth != 0) {
    // Dolphin's depth path: Z24 split into bytes (r = bits 16-23, g = 8-15,
    // b = 0-7, a = Z4), then routed per Z copy format. Our GL depth equals the
    // GX Z / 2^24 (viewport depth range), so no conversion is needed. The copy
    // filter applies to depth copies too (Dolphin: verified on hardware).
    float zf = ztap(sx, sy);
    int z = clamp(int(zf * 16777216.0), 0, 16777215);
    vec4 t = vec4(float((z >> 16) & 255), float((z >> 8) & 255), float(z & 255),
                  float((z >> 16) & 0xF0)) / 255.0;
    if (uFmt == 0)                    o = t.aaaa;              // Z4
    else if (uFmt == 1 || uFmt == 8)  o = t.rrrr;              // Z8 / Z8H
    else if (uFmt == 3)               o = t.gggr;              // Z16
    else if (uFmt == 11)              o = t.rrrg;              // Z16 reversed
    else if (uFmt == 6)               o = vec4(t.rgb, 1.0);    // Z24X8
    else if (uFmt == 9)               o = t.gggg;              // Z8M
    else if (uFmt == 10)              o = t.bbbb;              // Z8L
    else if (uFmt == 12)              o = t.gggb;              // Z16L (SS)
    else                              o = vec4(t.rgb, 1.0);
    return;
  }
  vec4 c = tap(sx, sy);
  if (uScale == 2)
    c = 0.25 * (c + tap(sx + 1, sy) + tap(sx, sy + 1) + tap(sx + 1, sy + 1));
  if (uFmt == 16) { // XFB copy (texture-only XFB, Dolphin EFBCopyFormat::XFB)
    o = vec4(pow(c.rgb, vec3(uGammaRcp)), 1.0);
    return;
  }
  if (uHasAlpha == 0) c.a = 1.0;
  if (uIntensity != 0) {
    vec3 r = floor(c.rgb * 255.0 + 0.5);
    float Y = floor((dot(r, vec3(66.0, 129.0, 25.0)) + 128.0) / 256.0) + 16.0;
    float U = floor((dot(r, vec3(-38.0, -74.0, 112.0)) + 128.0) / 256.0) + 128.0;
    float V = floor((dot(r, vec3(112.0, -94.0, -18.0)) + 128.0) / 256.0) + 128.0;
    c.rgb = clamp(vec3(Y, U, V), 0.0, 255.0) / 255.0;
  }
  vec4 r;
  if (uFmt == 0)                    r = vec4(q(c.r, 4));
  else if (uFmt == 1 || uFmt == 8)  r = c.rrrr;
  else if (uFmt == 2)               r = vec4(vec3(q(c.r, 4)), q(c.a, 4));
  else if (uFmt == 3)               r = c.rrra;
  else if (uFmt == 4)               r = vec4(q(c.r, 5), q(c.g, 6), q(c.b, 5), 1.0);
  else if (uFmt == 5)               r = vec4(q(c.r, 5), q(c.g, 5), q(c.b, 5), q(c.a, 3));
  else if (uFmt == 6)               r = c;
  else if (uFmt == 7)               r = c.aaaa;
  else if (uFmt == 9)               r = c.gggg;
  else if (uFmt == 10)              r = c.bbbb;
  else if (uFmt == 11)              r = c.rrrg;
  else if (uFmt == 12)              r = c.gggb;
  else                              r = vec4(c.rgb, 1.0);
  o = r;
}
)";

static bool EnsureCopyProgram() {
  if (s_copy_prog)
    return true;
  s_copy_prog = CompileGLShaderProgram(kCopyVS, kCopyFS);
  if (!s_copy_prog)
    return false;
  s_cu_efb = glGetUniformLocation(s_copy_prog, "uEfb");
  s_cu_rect = glGetUniformLocation(s_copy_prog, "uRect");
  s_cu_size = glGetUniformLocation(s_copy_prog, "uEfbSize");
  s_cu_scale = glGetUniformLocation(s_copy_prog, "uScale");
  s_cu_fmt = glGetUniformLocation(s_copy_prog, "uFmt");
  s_cu_int = glGetUniformLocation(s_copy_prog, "uIntensity");
  s_cu_alpha = glGetUniformLocation(s_copy_prog, "uHasAlpha");
  s_cu_depth = glGetUniformLocation(s_copy_prog, "uDepth");
  s_cu_zsrc = glGetUniformLocation(s_copy_prog, "uEfbZ");
  s_cu_fon = glGetUniformLocation(s_copy_prog, "uFilterOn");
  s_cu_filter = glGetUniformLocation(s_copy_prog, "uFilter");
  s_cu_rowstep = glGetUniformLocation(s_copy_prog, "uRowStep");
  s_cu_clampy = glGetUniformLocation(s_copy_prog, "uClampY");
  s_cu_gamma = glGetUniformLocation(s_copy_prog, "uGammaRcp");
  glGenVertexArrays(1, &s_empty_vao);
  glGenFramebuffers(1, &s_copy_fbo);
  return true;
}

// ---------------------------------------------------------------------------
// Render-state tracking
// ---------------------------------------------------------------------------
enum : uint32_t {
  D_SHADER = 1u << 0,
  D_TEVREG = 1u << 1,
  D_PE = 1u << 2,
  D_SCISSOR = 1u << 3,
  D_TEX = 1u << 4,
  D_CHAN = 1u << 5,
  D_LIGHT = 1u << 6,
  D_PROJ = 1u << 7,
  D_VIEWPORT = 1u << 8,
  D_IND = 1u << 9, // indirect matrices / texcoord scales (LIGHT7)
  D_FOG = 1u << 10, // fog BP 0xE8-0xF2 (TEX7)
  D_ALL = 0x7FFu
};
static uint32_t s_dirty = D_ALL;

static uint32_t DirtyForBP(uint8_t r) {
  if (r == 0x00)
    return D_SHADER | D_PE | D_TEX;
  if (r >= 0x28 && r <= 0x2F)
    return D_SHADER | D_TEX;
  // Indirect (LIGHT7): IND_CMD per stage, IND scale, IREF; matrices; tc scale.
  if ((r >= 0x10 && r <= 0x1F) || (r >= 0x25 && r <= 0x27))
    return D_SHADER | D_TEX;
  if ((r >= 0x06 && r <= 0x0E) || (r >= 0x30 && r <= 0x3F))
    return D_IND;
  if (r >= 0xC0 && r <= 0xDF)
    return D_SHADER;
  if (r == 0xF3 || (r >= 0xF6 && r <= 0xFD))
    return D_SHADER;
  if (r == 0xF4) return D_FOG; // depth-texture bias shares the unused fog-uniform component
  if (r == 0xF5) return D_SHADER;
  if (r >= 0xE0 && r <= 0xE7)
    return D_TEVREG;
  if (r >= 0xE8 && r <= 0xF2) // fog (TEX7): fsel/proj/range enable select the code
    return D_SHADER | D_FOG;
  if (r >= 0x40 && r <= 0x43)
    return D_PE | (r == 0x43 ? D_SHADER : 0); // depth-texture early/late test mode
  if (r == 0x20 || r == 0x21 || r == 0x59)
    return D_SCISSOR | D_VIEWPORT;
  if (r >= 0x80 && r <= 0xBF)
    return D_TEX;
  if (r == 0x64 || r == 0x65)
    return D_TEX;
  return 0;
}

static uint32_t DirtyForXF(int first, int count) {
  const int last = first + count - 1;
  auto hit = [&](int a, int b) { return first <= b && last >= a; };
  uint32_t d = 0;
  if (hit(0x600, 0x67F))
    d |= D_LIGHT;
  if (hit(0x100A, 0x1011))
    d |= D_CHAN;
  // LIGHT7: numColorChans and colour-texgen types select vertex-shader code.
  if (hit(0x1009, 0x1009) || hit(0x103F, 0x1047))
    d |= D_SHADER;
  if (hit(0x101A, 0x101F))
    d |= D_VIEWPORT;
  if (hit(0x1020, 0x1026))
    d |= D_PROJ;
  return d;
}

// GL fixed-function state derived from BP/XF. Only uint32/int32/float fields
// (no padding), zero-initialised, compared with memcmp.
struct FixedState {
  int32_t vp[4];
  float dnear, dfar;
  int32_t sc[4];
  uint32_t blend_on, eqc, eqa, sfc, dfc, sfa, dfa;
  uint32_t logic_on, logic_op;
  uint32_t cmask; // bit0-2 rgb, bit3 alpha
  uint32_t ztest, zfunc, zwrite;
  uint32_t cull_on, cull_face, front_face;
};
static FixedState s_fw{}, s_fh{};
static bool s_fixed_valid = false;

static float XfAsFloat(int idx) {
  float f;
  std::memcpy(&f, &g_state.xf[idx], 4);
  return f;
}

static inline int32_t SExt10(uint32_t v) {
  return (int32_t)((v & 0x3FF) ^ 0x200) - 0x200;
}

static bool EfbHasAlpha() { return (g_state.bp[0x43] & 7) == 1; } // RGBA6_Z24

static GLenum SrcFactor(uint32_t f, bool has_alpha) {
  switch (f & 7) {
  case 0: return GL_ZERO;
  case 1: return GL_ONE;
  case 2: return GL_DST_COLOR;
  case 3: return GL_ONE_MINUS_DST_COLOR;
  case 4: return GL_SRC1_ALPHA;
  case 5: return GL_ONE_MINUS_SRC1_ALPHA;
  case 6: return has_alpha ? GL_DST_ALPHA : GL_ONE;
  default: return has_alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ZERO;
  }
}
static GLenum DstFactor(uint32_t f, bool has_alpha) {
  switch (f & 7) {
  case 0: return GL_ZERO;
  case 1: return GL_ONE;
  case 2: return GL_SRC1_COLOR;
  case 3: return GL_ONE_MINUS_SRC1_COLOR;
  case 4: return GL_SRC1_ALPHA;
  case 5: return GL_ONE_MINUS_SRC1_ALPHA;
  case 6: return has_alpha ? GL_DST_ALPHA : GL_ONE;
  default: return has_alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ZERO;
  }
}

// PE output state: Dolphin BlendingState::Generate / DepthState::Generate.
static void ComputePE(FixedState &f) {
  const uint32_t cm = g_state.bp[0x41], zm = g_state.bp[0x40];
  const bool has_alpha = EfbHasAlpha();
  bool cu = (cm >> 3) & 1;
  bool au = ((cm >> 4) & 1) && has_alpha;
  const bool dstalpha = ((g_state.bp[0x42] >> 8) & 1) && au;
  f.blend_on = f.logic_on = 0;
  f.eqc = f.eqa = GL_FUNC_ADD;
  f.sfc = f.sfa = GL_ONE;
  f.dfc = f.dfa = GL_ZERO;
  f.logic_op = GL_COPY;
  if (cm & 1) {
    f.blend_on = 1;
    if ((cm >> 11) & 1) { // subtract: dst - src
      f.eqc = f.eqa = GL_FUNC_REVERSE_SUBTRACT;
      f.sfc = f.dfc = f.sfa = f.dfa = GL_ONE;
      if (dstalpha) {
        f.eqa = GL_FUNC_ADD;
        f.dfa = GL_ZERO;
      }
    } else {
      f.sfc = f.sfa = SrcFactor((cm >> 8) & 7, has_alpha);
      f.dfc = f.dfa = DstFactor((cm >> 5) & 7, has_alpha);
      if (dstalpha) {
        f.sfa = GL_ONE;
        f.dfa = GL_ZERO;
      }
    }
  } else if ((cm >> 1) & 1) {
    const uint32_t op = (cm >> 12) & 0xF;
    if (op == 5) { // NOOP fast path (Dolphin): only a dst-alpha write remains
      cu = false;
      au = au && dstalpha;
    } else {
      f.logic_on = 1;
      f.logic_op = GL_CLEAR + op; // GX and GL list the 16 ops in one order
    }
  }
  f.cmask = (cu ? 7u : 0u) | (au ? 8u : 0u);
  f.ztest = zm & 1;
  f.zfunc = GL_NEVER + ((zm >> 1) & 7);
  f.zwrite = f.ztest ? ((zm >> 4) & 1) : 0; // test off => no Z write
  // GEN_MODE cull, bits 14-15: 0 none, 1 back, 2 front, 3 all. The projection
  // flips Y, so console counter-clockwise arrives clockwise: front = CW.
  static const bool no_cull = std::getenv("NWII_NOCULL") != nullptr;
  const uint8_t m = g_state.cullMode;
  f.cull_on = (!no_cull && m != 0) ? 1 : 0;
  f.cull_face = m == 3 ? GL_FRONT_AND_BACK : GL_BACK;
  f.front_face = m == 2 ? GL_CCW : GL_CW;
}

// XF viewport (0x101A-0x101F) as Dolphin SetScissorAndViewport: origin minus
// the scissor offset (BP 0x59 << 1, which carries GX's +342), Y flipped to the
// GL origin, depth range [(farZ - zRange), farZ] / 2^24.
static void ComputeViewport(FixedState &f) {
  const float wd = XfAsFloat(0x1A), ht = XfAsFloat(0x1B), zr = XfAsFloat(0x1C);
  const float xo = XfAsFloat(0x1D), yo = XfAsFloat(0x1E), fz = XfAsFloat(0x1F);
  if (wd == 0.0f && ht == 0.0f) { // never set: whole EFB
    f.vp[0] = 0; f.vp[1] = 0; f.vp[2] = s_efb_w; f.vp[3] = s_efb_h;
    f.dnear = 0.0f; f.dfar = 1.0f;
    return;
  }
  const uint32_t off = g_state.bp[0x59];
  const float xoff = (float)(SExt10(off) * 2), yoff = (float)(SExt10(off >> 10) * 2);
  float x = (xo - xoff) - wd, y = (yo - yoff) + ht;
  float w = 2.0f * wd, h = -2.0f * ht;
  if (w < 0.f) { x += w; w = -w; }
  if (h < 0.f) { y += h; h = -h; }
  // RES9: EFB px -> scaled px (Dolphin EFBToScaledXf); s = 1 multiplies by 1.0f.
  const float s = (float)s_scale;
  f.vp[0] = (int32_t)std::lround(x * s);
  f.vp[2] = (int32_t)std::lround(w * s);
  f.vp[3] = (int32_t)std::lround(h * s);
  f.vp[1] = s_efb_h - (int32_t)std::lround(y * s) - f.vp[3];
  float n = (fz - zr) / 16777216.0f, fa = fz / 16777216.0f;
  f.dnear = std::clamp(n, 0.0f, 1.0f);
  f.dfar = std::clamp(fa, 0.0f, 1.0f);
}

// BP scissor 0x20/0x21 (x bits 12-22, y bits 0-10, both +342) minus the
// scissor offset, inclusive bottom-right; GL origin bottom-left.
static void ComputeScissor(FixedState &f) {
  const uint32_t tl = g_state.bp[0x20], br = g_state.bp[0x21];
  if (tl == 0 && br == 0) { // never set: whole EFB
    f.sc[0] = 0; f.sc[1] = 0; f.sc[2] = s_efb_w; f.sc[3] = s_efb_h;
    return;
  }
  const uint32_t off = g_state.bp[0x59];
  const int xoff = SExt10(off) * 2, yoff = SExt10(off >> 10) * 2;
  int x0 = (int)((tl >> 12) & 0x7FF) - xoff, y0 = (int)(tl & 0x7FF) - yoff;
  int x1 = (int)((br >> 12) & 0x7FF) - xoff + 1, y1 = (int)(br & 0x7FF) - yoff + 1;
  x0 = std::clamp(x0, 0, s_efb_lw); x1 = std::clamp(x1, 0, s_efb_lw);
  y0 = std::clamp(y0, 0, s_efb_lh); y1 = std::clamp(y1, 0, s_efb_lh);
  if (x1 <= x0 || y1 <= y0) {
    f.sc[0] = f.sc[1] = f.sc[2] = f.sc[3] = 0;
    return;
  }
  // RES9: clamped in EFB px, then scaled (whole scaled pixels per EFB pixel).
  const int s = s_scale;
  f.sc[0] = x0 * s;
  f.sc[1] = s_efb_h - y1 * s;
  f.sc[2] = (x1 - x0) * s;
  f.sc[3] = (y1 - y0) * s;
}

static void ApplyFixed(const FixedState &w) {
  const bool all = !s_fixed_valid;
  FixedState &h = s_fh;
  if (all || std::memcmp(w.vp, h.vp, sizeof w.vp))
    glViewport(w.vp[0], w.vp[1], w.vp[2], w.vp[3]);
  if (all || w.dnear != h.dnear || w.dfar != h.dfar)
    glDepthRange(w.dnear, w.dfar);
  if (all)
    glEnable(GL_SCISSOR_TEST);
  if (all || std::memcmp(w.sc, h.sc, sizeof w.sc))
    glScissor(w.sc[0], w.sc[1], w.sc[2], w.sc[3]);
  if (all || w.blend_on != h.blend_on)
    w.blend_on ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
  if (w.blend_on && (all || w.eqc != h.eqc || w.eqa != h.eqa || w.sfc != h.sfc ||
                     w.dfc != h.dfc || w.sfa != h.sfa || w.dfa != h.dfa || !h.blend_on)) {
    glBlendEquationSeparate(w.eqc, w.eqa);
    glBlendFuncSeparate(w.sfc, w.dfc, w.sfa, w.dfa);
  }
  if (all || w.logic_on != h.logic_on)
    w.logic_on ? glEnable(GL_COLOR_LOGIC_OP) : glDisable(GL_COLOR_LOGIC_OP);
  if (w.logic_on && (all || w.logic_op != h.logic_op || !h.logic_on))
    glLogicOp(w.logic_op);
  if (all || w.cmask != h.cmask)
    glColorMask(w.cmask & 1, (w.cmask >> 1) & 1, (w.cmask >> 2) & 1, (w.cmask >> 3) & 1);
  if (all || w.ztest != h.ztest)
    w.ztest ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
  if (w.ztest && (all || w.zfunc != h.zfunc || !h.ztest))
    glDepthFunc(w.zfunc);
  if (all || w.zwrite != h.zwrite)
    glDepthMask(w.zwrite ? GL_TRUE : GL_FALSE);
  if (all || w.cull_on != h.cull_on)
    w.cull_on ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
  if (w.cull_on && (all || w.cull_face != h.cull_face || w.front_face != h.front_face ||
                    !h.cull_on)) {
    glCullFace(w.cull_face);
    glFrontFace(w.front_face);
  }
  h = w;
  s_fixed_valid = true;
}

// ---- shader UID: exactly the inputs GenerateTEVShader reads ------------------
static uint64_t ShaderUid(uint8_t *used_maps) {
  uint64_t h = 1469598103934665603ull;
  auto add = [&](const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  };
  uint8_t n = g_state.numTevStages;
  if (n == 0)
    n = 1;
  if (n > 16)
    n = 16;
  add(&n, 1);
  uint8_t used = 0;
  for (int i = 0; i < n; ++i) {
    TEVStage s = g_state.tevStages[i];
    if (s.texMap == 0xFF)
      s.texCoord = 0;
    else
      used |= (uint8_t)(1u << (s.texMap & 7));
    if (s.colorChan > 1 && s.colorChan != 5 && s.colorChan != 6)
      s.colorChan = 7; // 5/6 = alpha bump (indirect), 7 = zero
    add(&s, sizeof s);
    const uint32_t ksel = g_state.bp[0xF6 + i / 2];
    const uint32_t k = (i & 1) ? ((ksel >> 14) & 0x3FF) : ((ksel >> 4) & 0x3FF);
    add(&k, 4);
    // TEV swap tables (LIGHT7): ras/tex table selected by the alpha env bits 0-3,
    // each table = BP 0xF6+2t / 0xF7+2t bits 0-3 (GenerateTEVShader swizzles).
    const uint32_t swsel = g_state.bp[0xC1 + 2 * i];
    auto swtab = [](uint32_t t) {
      return (g_state.bp[0xF6 + 2 * t] & 0xF) | ((g_state.bp[0xF7 + 2 * t] & 0xF) << 4);
    };
    const uint32_t swk = swtab(swsel & 3) | (swtab((swsel >> 2) & 3) << 8);
    add(&swk, 4);
  }
  const AlphaTest &at = g_state.alphaTest;
  if (at.comp0 == 7 && at.comp1 == 7 && at.logic == 0) {
    const uint8_t off = 0xA5;
    add(&off, 1);
  } else {
    add(&at, sizeof at);
  }
  // Last-stage destination output (D1) is covered by the TEVStage bytes.
  // Vertex shader (LIGHT7): XF numColorChans + colour-texgen sources.
  {
    uint32_t vk = g_state.xf[0x09] & 3;
    const uint32_t ntg = std::min<uint32_t>(g_state.xf[0x3F] & 0xF, 8);
    for (uint32_t i = 0; i < 8; ++i) {
      const uint32_t t = ntg ? (g_state.xf[0x40 + (i < ntg ? i : 0)] >> 4) & 7 : 0;
      vk |= (t == 2 || t == 3 ? t - 1 : 0) << (2 + 2 * i);
    }
    add(&vk, sizeof vk);
  }
  // Indirect path (LIGHT7): GenerateTEVShader also reads numtexgens,
  // numindstages, IND_CMD per stage, IREF and the indirect scales; the
  // indirect stages' texmaps must be bound too.
  if (TEVShaderUsesIndirect(g_state)) {
    uint32_t ind[20];
    ind[0] = g_state.numTexGens & 0xF;
    ind[1] = (g_state.bp[0x00] >> 16) & 7;
    ind[2] = g_state.bp[0x27];
    ind[3] = g_state.bp[0x25] & 0xFFFF;
    for (int i = 0; i < 16; ++i)
      ind[4 + i] = i < n ? ((g_state.bp[0x10 + i] & 0x1FFFFF) |
                            ((uint32_t)(g_state.tevStages[i].texCoord & 7) << 24))
                         : 0; // texcoord also feeds wrapping of map-less stages
    const uint32_t s26 = g_state.bp[0x26] & 0xFFFF;
    add(ind, sizeof ind);
    add(&s26, sizeof s26);
    used |= TEVIndirectMapsUsed(g_state);
  }
  // Fog (TEX7): EmitFog in tev_shader_gen.cpp reads fsel, proj, range enable.
  const uint32_t fsel = (g_state.bp[0xF1] >> 21) & 7;
  const uint32_t fogk = fsel ? (0x100u | fsel | (((g_state.bp[0xF1] >> 20) & 1) << 3) |
                                (((g_state.bp[0xE8] >> 10) & 1) << 4))
                             : 0u;
  add(&fogk, sizeof fogk);
  const uint32_t ztop = (g_state.bp[0xF5] >> 2) & 3;
  const uint32_t ztk = ztop ? ((g_state.bp[0xF5] & 15) | (g_state.bp[0x43] & 64)) : 0;
  add(&ztk, sizeof ztk);
  // LIGHT17: EFB output quantisation (pixel format, dither, read-modify-write).
  const uint32_t qk = TEVOutputQuantKey(g_state);
  if (qk)
    add(&qk, sizeof qk);
  // Texcoord scale (TEX7): stages sampling with scale != texmap size.
  const uint16_t tcs = TEVTexCoordScaleMismatch(g_state);
  if (tcs)
    add(&tcs, sizeof tcs);
  *used_maps = used;
  return h;
}

// ---- texture lookup with a per-frame memo ------------------------------------
struct TexMemoKey {
  uint32_t addr, w, h, fmt, tlut_off, tlut_fmt, levels;
  bool operator==(const TexMemoKey &o) const {
    return addr == o.addr && w == o.w && h == o.h && fmt == o.fmt &&
           tlut_off == o.tlut_off && tlut_fmt == o.tlut_fmt && levels == o.levels;
  }
};
struct TexMemoHash {
  size_t operator()(const TexMemoKey &k) const {
    uint64_t h = k.addr;
    h = h * 0x100000001B3ull ^ k.w;
    h = h * 0x100000001B3ull ^ k.h;
    h = h * 0x100000001B3ull ^ k.fmt;
    h = h * 0x100000001B3ull ^ k.tlut_off;
    h = h * 0x100000001B3ull ^ k.tlut_fmt;
    h = h * 0x100000001B3ull ^ k.levels;
    return (size_t)h;
  }
};
// The texture cache hashes texel + TLUT memory on every lookup; within one
// frame the result for a given stage descriptor cannot change except through
// a TLUT load (cleared) or an EFB copy (EFB textures are checked first).
static std::unordered_map<TexMemoKey, GLuint, TexMemoHash> s_tex_memo;
static uint64_t s_tex_memo_gen = ~0ull;

struct TexBind {
  GLuint tex;
  uint32_t mode0;  // TEXMODE0 wrap/filter/lod-bias bits (TEX7: 0-16)
  uint32_t mode1;  // TEXMODE1 min/max LOD (0 unless levels > 1)
  uint32_t levels; // mip levels the bound texture has (1 for EFB copies)
};
static TexBind s_tw[8], s_th[8];
static bool s_tex_valid[8];

// ---- EFB-copy texture validity (M1.9 basics) -----------------------------------
// The hardware writes an EFB copy into guest RAM; we keep it as a GL texture
// registered under its destination instead. Record a sampled hash of the RAM
// range the copy would have overwritten: if the CPU later rewrites that range
// (heap reuse: a normal texture loaded there) or another copy overlaps it, the
// registration is dropped and the address decodes from RAM again. Copies
// re-made every frame are checked at most once per frame and never against
// the frame they were made in.
struct EfbCopyInfo {
  uint32_t bytes;
  uint32_t hash;
  GLuint tex;
};
static std::unordered_map<uint32_t, EfbCopyInfo> s_efb_info;
static std::unordered_map<uint32_t, uint64_t> s_efb_checked; // dest -> frame
static uint64_t s_render_frame = 0;

// INTERP17: in-between frame scope (interp_render.h). Non-null only inside RenderInterpFrame.
static InterpDrawHook *s_interp_hook = nullptr;
// NWII_INTERP_NO_ISOLATION=1 (negative control of the leak test only): in-between frames use the real EFB
// copy registry and leave the EFB as they found it, like the INTERP16 prototype without protection.
static bool s_interp_noiso = false;
static inline bool InterpIsolated() { return s_interp_hook && !s_interp_noiso; }
struct InterpShadowCopy {
  GLuint tex = 0;
  int w = 0, h = 0;
  bool valid = false; // made in the current in-between frame
};
static std::unordered_map<uint32_t, InterpShadowCopy> s_interp_copies; // dest -> shadow texture
static std::unordered_map<uint32_t, bool> s_interp_ram_ok;             // dest -> real copy's RAM hash still ok
static InterpRenderStats s_interp_stats;
// The shadow texture for an in-between copy to `dest` (pw x ph RGBA8, reused across frames).
static GLuint InterpShadowFor(uint32_t dest, int pw, int ph) {
  InterpShadowCopy &sc = s_interp_copies[dest];
  if (!sc.tex)
    glGenTextures(1, &sc.tex);
  if (sc.w != pw || sc.h != ph) {
    glActiveTexture(GL_TEXTURE0 + kUploadUnit);
    glBindTexture(GL_TEXTURE_2D, sc.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    sc.w = pw;
    sc.h = ph;
  }
  sc.valid = true;
  return sc.tex;
}

static uint32_t RamHash(uint32_t addr, uint32_t bytes) {
  const uint8_t *p =
      nwii::runtime::g_mmu ? nwii::runtime::g_mmu->get_ptr(0x80000000u | addr, bytes) : nullptr;
  if (!p || !bytes)
    return 0;
  const uint32_t stride = bytes > 4096 * 4 ? (bytes / 4096) & ~3u : 4;
  uint32_t h = 2166136261u;
  for (uint32_t o = 0; o + 4 <= bytes; o += stride) {
    uint32_t w;
    std::memcpy(&w, p + o, 4);
    h = (h ^ w) * 16777619u;
  }
  return h;
}

static void DropEfbCopy(uint32_t dest) {
  auto it = s_efb_info.find(dest);
  if (it == s_efb_info.end())
    return;
  FlushBatch(); // the open batch may still sample this texture
  for (int i = 0; i < 8; ++i)
    if (s_th[i].tex == it->second.tex)
      s_tex_valid[i] = false;
  nwii::runtime::hle::TextureCache::get().register_efb_texture(dest, 0);
  s_copy_tex_size.erase(it->second.tex);
  glDeleteTextures(1, &it->second.tex);
  s_efb_info.erase(it);
  s_efb_checked.erase(dest);
}

// Texture format the hardware writes for an EFB copy format (texel size).
static uint32_t CopyTexFormat(uint32_t realfmt) {
  switch (realfmt) {
  case 0: return 0;                   // R4 -> I4
  case 1: case 7: case 8: case 9: case 10: return 1; // 8-bit -> I8
  case 2: return 2;                   // RA4 -> IA4
  case 3: case 11: case 12: return 3; // 16-bit pairs -> IA8
  case 4: return 4;
  case 5: return 5;
  default: return 6;                  // RGBA8 / Z24X8
  }
}

static GLuint EfbCopyFor(uint32_t addr) {
  if (InterpIsolated()) { // INTERP17: in-between frame: its own shadow copy, else the real one read-only
    auto sh = s_interp_copies.find(addr);
    if (sh != s_interp_copies.end() && sh->second.valid)
      return sh->second.tex;
    const GLuint r = nwii::runtime::hle::TextureCache::get().find_efb_texture(addr);
    auto in = s_efb_info.find(addr);
    if (!r || in == s_efb_info.end())
      return r;
    auto ok = s_interp_ram_ok.find(addr);
    if (ok == s_interp_ram_ok.end()) {
      // The CPU may rewrite a copy's RAM after A rendered and before I.
      // I does not advance s_render_frame, so A's checked-frame shortcut
      // cannot validate I's reads. Hash each destination once per I frame.
      const bool fine = RamHash(addr, in->second.bytes) == in->second.hash;
      ok = s_interp_ram_ok.emplace(addr, fine).first;
      if (!fine)
        ++s_interp_stats.stale_reads;
    }
    return ok->second ? r : 0;
  }
  const GLuint e = nwii::runtime::hle::TextureCache::get().find_efb_texture(addr);
  if (!e)
    return 0;
  auto it = s_efb_info.find(addr);
  if (it == s_efb_info.end())
    return e; // registered elsewhere (capture replay start state): trust it
  uint64_t &checked = s_efb_checked[addr];
  if (checked == s_render_frame)
    return e;
  checked = s_render_frame;
  if (RamHash(addr, it->second.bytes) == it->second.hash)
    return e;
  DropEfbCopy(addr); // the CPU rewrote the range: decode from RAM
  return 0;
}

// TEX7: *levels gets the mip level count of the returned texture (1 for EFB
// copies); 'want' is what TexLevels() asked for.
// HDTEX16: mipf = TEXMODE0 mip filter on (Dolphin name "_m"); a custom (HD) texture reports its own
// GL level count | kTexLevelsCustom (+ "_arb" LOD offset) for SetGXSamplerFromMode.
static GLuint LookupTexture(const TexStage &st, uint32_t want, uint32_t *levels, bool mipf = false) {
  *levels = 1;
  if (!st.base_addr || !st.width || !st.height || !g_ctx_ptr)
    return 0;
  if (GLuint e = EfbCopyFor(st.base_addr))
    return e;
  *levels = want;
  if (s_tex_memo_gen != nwii::runtime::hle::g_texture_cache_generation) {
    s_tex_memo.clear();
    s_tex_memo_gen = nwii::runtime::hle::g_texture_cache_generation;
  }
  const TexMemoKey key{st.base_addr, st.width, st.height, st.format,
                       st.tlut_offset, st.tlut_format, want | (mipf ? 0x80000000u : 0u)};
  auto &tc = nwii::runtime::hle::TextureCache::get();
  GLuint t = 0;
  auto it = s_tex_memo.find(key);
  if (it != s_tex_memo.end()) {
    t = it->second;
  } else {
    ++s_stats.tex_lookups;
    // A cache miss uploads (and binds) on the active unit: use a unit no draw
    // samples, so the open batch's bindings stay intact.
    glActiveTexture(GL_TEXTURE0 + kUploadUnit);
    t = tc.get_texture(*g_ctx_ptr, st, want, mipf);
    s_tex_memo[key] = t;
  }
  if (const auto *ci = tc.custom_info(t)) { // HDTEX16
    uint32_t lv = std::min<uint32_t>(std::max<uint32_t>(ci->levels, 1), 0xFF) |
                  nwii::runtime::hle::kTexLevelsCustom;
    if (ci->arb && ci->scale > 0.0f) { // Dolphin: lod_bias += log2(EFB scale / custom scale)
      const int q = (int)std::lround(std::log2((float)s_scale / ci->scale) * 32.0f);
      lv |= (uint32_t)(uint8_t)(int8_t)std::clamp(q, -128, 127) << 16;
    }
    lv |= (ci->hybrid_base & 0xFu) << 24; // HDTEX17: hybrid mip chain (game mips below the original size)
    *levels = lv;
  }
  return t;
}

static uint32_t TexMode0(int i) {
  return g_state.bp[i < 4 ? 0x80 + i : 0xA0 + (i - 4)];
}
static uint32_t TexMode1(int i) {
  return g_state.bp[i < 4 ? 0x84 + i : 0xA4 + (i - 4)];
}
// TEX7: mip levels to upload, Dolphin TextureCacheBase::Load: none unless the
// TEXMODE0 mip filter (bits 5-6) is on; (max_lod + 15) / 16 + 1, capped at
// IntLog2(max(w, h)) + 1 so the chain ends at one 1x1 level.
static uint32_t TexLevels(const TexStage &st, uint32_t mode0, uint32_t mode1) {
  if (((mode0 >> 5) & 3) == 0 || !st.width || !st.height)
    return 1;
  uint32_t lv = (((mode1 >> 8) & 0xFF) + 0xF) / 0x10 + 1;
  uint32_t m = std::max(st.width, st.height), log2 = 0;
  while (m > 1) { m >>= 1; ++log2; }
  return std::max<uint32_t>(1, std::min(lv, log2 + 1));
}

static uint8_t s_used_maps = 0;
static uint64_t s_want_uid = 0, s_have_uid = 0;
static bool s_have_uid_valid = false;
static bool s_skip_draw = false;

// ---- uniform groups ----------------------------------------------------------
static void ComputeProj(float p[16]) {
  const float *sp = g_state.projection;
  std::memset(p, 0, 16 * sizeof(float));
  if (!g_state.projSet) {
    p[0] = p[5] = p[10] = p[15] = 1;
  } else if (g_state.projType == 0) {
    // GX clip z/w spans [-1 (near), 0 (far)]; GL wants [-1, 1]: z' = 2z + w.
    p[0] = sp[0];
    p[8] = sp[1];
    p[5] = sp[2];
    p[9] = sp[3];
    p[10] = 2.0f * sp[4] - 1.0f;
    p[14] = 2.0f * sp[5];
    p[11] = -1.0f;
  } else {
    // Orthographic: same depth mapping (w = 1), so ortho draws carry real Z
    // for the Z mode instead of a constant.
    p[0] = sp[0];
    p[12] = sp[1];
    p[5] = sp[2];
    p[13] = sp[3];
    p[10] = 2.0f * sp[4];
    p[14] = 2.0f * sp[5] + 1.0f;
    p[15] = 1.0f;
  }
}

static void Unpack8888(uint32_t c, float *o) {
  o[0] = ((c >> 24) & 0xFF) / 255.f;
  o[1] = ((c >> 16) & 0xFF) / 255.f;
  o[2] = ((c >> 8) & 0xFF) / 255.f;
  o[3] = (c & 0xFF) / 255.f;
}

static bool UpdateGroup(uint32_t dirty, UniformValues &nv) {
  bool changed = false;
  auto bump = [&](int g, void *dst, const void *src, size_t n) {
    if (std::memcmp(dst, src, n) != 0) {
      std::memcpy(dst, src, n);
      ++s_uver[g];
      changed = true;
    }
  };
  if (dirty & D_PROJ) {
    ComputeProj(nv.proj);
    if (s_interp_hook) // INTERP17: in-between frame: the camera delta rides on the projection
      s_interp_hook->AdjustProj(nv.proj, g_state.projSet && g_state.projType == 0);
    bump(UG_PROJ, s_uv.proj, nv.proj, sizeof nv.proj);
  }
  if (dirty & D_CHAN) {
    Unpack8888(g_state.xf[0x00C], nv.mat);
    Unpack8888(g_state.xf[0x00D], nv.mat + 4);
    Unpack8888(g_state.xf[0x00A], nv.amb);
    Unpack8888(g_state.xf[0x00B], nv.amb + 4);
    for (int i = 0; i < 4; ++i)
      nv.cc[i] = (GLint)g_state.xf[0x00E + i];
    const bool c = std::memcmp(s_uv.mat, nv.mat, sizeof nv.mat) ||
                   std::memcmp(s_uv.amb, nv.amb, sizeof nv.amb) ||
                   std::memcmp(s_uv.cc, nv.cc, sizeof nv.cc);
    if (c) {
      std::memcpy(s_uv.mat, nv.mat, sizeof nv.mat);
      std::memcpy(s_uv.amb, nv.amb, sizeof nv.amb);
      std::memcpy(s_uv.cc, nv.cc, sizeof nv.cc);
      ++s_uver[UG_CHAN];
      changed = true;
    }
  }
  if (dirty & D_LIGHT) {
    for (int i = 0; i < 8; i++) {
      const float *L = g_state.lights[i];
      uint32_t c;
      std::memcpy(&c, &L[3], 4);
      Unpack8888(c, nv.lcol + i * 4);
      for (int k = 0; k < 3; ++k) {
        nv.lcos[i * 4 + k] = L[4 + k];
        nv.ldist[i * 4 + k] = L[7 + k];
        nv.lpos[i * 4 + k] = L[10 + k];
        nv.ldir[i * 4 + k] = L[13 + k];
      }
      nv.lcos[i * 4 + 3] = nv.ldist[i * 4 + 3] = nv.lpos[i * 4 + 3] =
          nv.ldir[i * 4 + 3] = 0.f;
    }
    const size_t n = sizeof nv.lcol * 5; // lcol..ldist are contiguous
    if (std::memcmp(s_uv.lcol, nv.lcol, n) != 0) {
      std::memcpy(s_uv.lcol, nv.lcol, n);
      ++s_uver[UG_LIGHT];
      changed = true;
    }
  }
  if (dirty & D_TEVREG) {
    auto decode11 = [](uint32_t val, int shift) -> float {
      int v = (val >> shift) & 0x7FF;
      if (v & 0x400)
        v |= ~0x7FF;
      return (float)v / 255.0f;
    };
    for (int i = 0; i < 4; i++) {
      const uint32_t ra = g_state.tevColorReg[i * 2];
      const uint32_t bg = g_state.tevColorReg[i * 2 + 1];
      const uint32_t kra = g_state.tevKonstReg[i * 2];
      const uint32_t kbg = g_state.tevKonstReg[i * 2 + 1];
      nv.tev[i * 4 + 0] = decode11(ra, 0);
      nv.tev[i * 4 + 3] = decode11(ra, 12);
      nv.tev[i * 4 + 2] = decode11(bg, 0);
      nv.tev[i * 4 + 1] = decode11(bg, 12);
      nv.ktev[i * 4 + 0] = decode11(kra, 0);
      nv.ktev[i * 4 + 3] = decode11(kra, 12);
      nv.ktev[i * 4 + 2] = decode11(kbg, 0);
      nv.ktev[i * 4 + 1] = decode11(kbg, 12);
    }
    const bool c = std::memcmp(s_uv.tev, nv.tev, sizeof nv.tev) ||
                   std::memcmp(s_uv.ktev, nv.ktev, sizeof nv.ktev);
    if (c) {
      std::memcpy(s_uv.tev, nv.tev, sizeof nv.tev);
      std::memcpy(s_uv.ktev, nv.ktev, sizeof nv.ktev);
      ++s_uver[UG_TEV];
      changed = true;
    }
  }
  if (dirty & D_PE) {
    const uint32_t da = g_state.bp[0x42];
    nv.dsta[0] = ((da >> 8) & 1) ? 1.0f : 0.0f;
    nv.dsta[1] = (da & 0xFF) / 255.0f;
    nv.dsta[2] = nv.dsta[3] = 0.0f;
    bump(UG_DSTA, s_uv.dsta, nv.dsta, sizeof nv.dsta);
  }
  if (dirty & (D_FOG | D_VIEWPORT)) {
    // Dolphin PixelShaderManager SetFogParamChanged / SetFogRangeAdjustChanged.
    auto gxf = [](uint32_t v) { // sign 19, exp 18-11, mant 10-0 -> float
      const uint32_t u = (((v >> 19) & 1) << 31) | (((v >> 11) & 0xFF) << 23) | ((v & 0x7FF) << 12);
      float f;
      std::memcpy(&f, &u, 4);
      return f;
    };
    nv.fogf[0] = gxf(g_state.bp[0xEE]);
    nv.fogf[1] = gxf(g_state.bp[0xF1]);
    const float wd = XfAsFloat(0x1A);
    const float vpw = 2.0f * wd;
    nv.fogf[2] = vpw != 0.0f ? (((float)((int)(g_state.bp[0xE8] & 0x3FF) - 342) / vpw) * 2.0f - 1.0f) : 0.0f;
    // gl_FragCoord.x is in scaled px (RES9); same expression as before at s = 1.
    nv.fogf[3] = vpw * (float)s_efb_w / (float)s_efb_lw;
    nv.fogi[0] = (GLint)(g_state.bp[0xF2] & 0xFFFFFF);
    nv.fogi[1] = (GLint)(g_state.bp[0xEF] & 0xFFFFFF);
    nv.fogi[2] = (GLint)(g_state.bp[0xF4] & 0xFFFFFF); // depth-texture bias, formerly unused
    nv.fogi[3] = (GLint)(g_state.bp[0xF0] & 0x1F);
    for (int i = 0; i < 12; ++i) {
      const uint32_t k = i < 10 ? g_state.bp[0xE9 + i / 2] : 0;
      nv.fogr[i] = (float)((i & 1) ? ((k >> 12) & 0xFFF) : (k & 0xFFF)) / 256.0f * 4.0f;
    }
    if (std::memcmp(s_uv.fogf, nv.fogf, sizeof nv.fogf) ||
        std::memcmp(s_uv.fogi, nv.fogi, sizeof nv.fogi) ||
        std::memcmp(s_uv.fogr, nv.fogr, sizeof nv.fogr)) {
      std::memcpy(s_uv.fogf, nv.fogf, sizeof nv.fogf);
      std::memcpy(s_uv.fogi, nv.fogi, sizeof nv.fogi);
      std::memcpy(s_uv.fogr, nv.fogr, sizeof nv.fogr);
      ++s_uver[UG_FOG];
      // Only fogged programs read these: flush the open batch only for them.
      if (s_active && (s_active->uFogF >= 0 || s_active->uFogI >= 0))
        changed = true;
    }
  }
  if (dirty & (D_IND | D_TEX)) {
    // Dolphin PixelShaderManager: indtexmtx[2m] = (ma, mc, me, 17 - scale),
    // [2m+1] = (mb, md, mf, 17 - scale); entries s11 (1.10 fixed point).
    auto s11 = [](uint32_t v) -> GLint { return (GLint)((int32_t)(v << 21) >> 21); };
    for (int m = 0; m < 3; ++m) {
      const uint32_t a = g_state.bp[0x06 + 3 * m], b = g_state.bp[0x07 + 3 * m],
                     c = g_state.bp[0x08 + 3 * m];
      const int scale = (int)(((a >> 22) & 3) | (((b >> 22) & 3) << 2) | (((c >> 22) & 3) << 4));
      GLint *r0 = nv.indm + 8 * m, *r1 = r0 + 4;
      r0[0] = s11(a); r0[1] = s11(b); r0[2] = s11(c);
      r1[0] = s11(a >> 11); r1[1] = s11(b >> 11); r1[2] = s11(c >> 11);
      r0[3] = r1[3] = 17 - scale;
    }
    for (int i = 0; i < 8; ++i) {
      nv.tdim[i * 4 + 0] = (float)std::max<uint32_t>(g_state.texStages[i].width, 1);
      nv.tdim[i * 4 + 1] = (float)std::max<uint32_t>(g_state.texStages[i].height, 1);
      nv.tdim[i * 4 + 2] = (float)((g_state.bp[0x30 + 2 * i] & 0xFFFF) + 1);
      nv.tdim[i * 4 + 3] = (float)((g_state.bp[0x31 + 2 * i] & 0xFFFF) + 1);
    }
    if (std::memcmp(s_uv.indm, nv.indm, sizeof nv.indm) ||
        std::memcmp(s_uv.tdim, nv.tdim, sizeof nv.tdim)) {
      std::memcpy(s_uv.indm, nv.indm, sizeof nv.indm);
      std::memcpy(s_uv.tdim, nv.tdim, sizeof nv.tdim);
      ++s_uver[UG_IND];
      // Only programs with the indirect path read these (the open batch's
      // program otherwise has both locations optimized out): no flush else.
      if (s_active && (s_active->uIndMtx >= 0 || s_active->uTexDim >= 0))
        changed = true;
    }
  }
  return changed;
}

static void UploadUniforms(GLShader &sh) {
  for (int g = 0; g < UG_COUNT; ++g) {
    if (sh.ver[g] == s_uver[g])
      continue;
    sh.ver[g] = s_uver[g];
    ++s_stats.uniform_uploads;
    switch (g) {
    case UG_PROJ:
      if (sh.uProjMtx >= 0)
        glUniformMatrix4fv(sh.uProjMtx, 1, GL_FALSE, s_uv.proj);
      break;
    case UG_CHAN:
      if (sh.uMatColor >= 0)
        glUniform4fv(sh.uMatColor, 2, s_uv.mat);
      if (sh.uAmbColor >= 0)
        glUniform4fv(sh.uAmbColor, 2, s_uv.amb);
      if (sh.uChanCtrl >= 0)
        glUniform1iv(sh.uChanCtrl, 4, s_uv.cc);
      break;
    case UG_LIGHT:
      if (sh.uLightCol >= 0)
        glUniform4fv(sh.uLightCol, 8, s_uv.lcol);
      if (sh.uLightPos >= 0)
        glUniform4fv(sh.uLightPos, 8, s_uv.lpos);
      if (sh.uLightDir >= 0)
        glUniform4fv(sh.uLightDir, 8, s_uv.ldir);
      if (sh.uLightCosAtt >= 0)
        glUniform4fv(sh.uLightCosAtt, 8, s_uv.lcos);
      if (sh.uLightDistAtt >= 0)
        glUniform4fv(sh.uLightDistAtt, 8, s_uv.ldist);
      break;
    case UG_TEV:
      if (sh.uTevKColor >= 0)
        glUniform4fv(sh.uTevKColor, 4, s_uv.ktev);
      if (sh.uTevColor >= 0)
        glUniform4fv(sh.uTevColor, 4, s_uv.tev);
      break;
    case UG_IND:
      if (sh.uIndMtx >= 0)
        glUniform4iv(sh.uIndMtx, 6, s_uv.indm);
      if (sh.uTexDim >= 0)
        glUniform4fv(sh.uTexDim, 8, s_uv.tdim);
      break;
    case UG_FOG:
      if (sh.uFogF >= 0)
        glUniform4fv(sh.uFogF, 1, s_uv.fogf);
      if (sh.uFogI >= 0)
        glUniform4iv(sh.uFogI, 1, s_uv.fogi);
      if (sh.uFogRange >= 0)
        glUniform4fv(sh.uFogRange, 3, s_uv.fogr);
      break;
    case UG_DSTA:
      if (sh.uDstAlpha >= 0)
        glUniform4fv(sh.uDstAlpha, 1, s_uv.dsta);
      break;
    }
  }
}

// ---- shader compile budget ---------------------------------------------------
// Live: compile until NWII_SHADER_BUDGET_MS (default 30) of compile time has
// been spent in this frame, but always at least kMinCompilesPerFrame; draws
// whose program is still missing are skipped and appear a frame or two later.
// Programs are cached for the whole run. gxreplay lifts the limits.
static constexpr int kMinCompilesPerFrame = 4;
static int s_compiles_this_frame = 0;
static uint64_t s_compile_us_this_frame = 0;
static int s_compile_budget = INT_MAX;          // count (RendererSetCompileBudget)
static bool s_compile_time_unlimited = false;
static uint64_t s_skipped_draws = 0;

static uint64_t CompileBudgetUs() {
  static const uint64_t us = [] {
    const char *e = std::getenv("NWII_SHADER_BUDGET_MS");
    const long v = e ? std::atol(e) : 30;
    return (uint64_t)(v > 0 ? v : 30) * 1000;
  }();
  return us;
}

// MVGEN21: mv = the motion variant (key uid ^ kMvUidSalt, source = the TEV program + MvInject); it never
// exits on a failure: null + s_mv.unsupported (the caller falls back to the plain program).
static GLShader *FindOrCompile(uint64_t uid, bool mv = false) {
  const uint64_t key = mv ? uid ^ kMvUidSalt : uid; // MVGEN21
  auto it = s_shader_cache.find(key);
  if (it != s_shader_cache.end())
    return &it->second;
  if (mv && s_mv.unsupported) // MVGEN21
    return nullptr;
  // MVGEN21: motion variants warm up slowly (a draw without one only lacks motion vectors, its colour is the
  // same): at most 2 per frame and only while this frame compiled for < 4 ms, so the real frame never hitches.
  if (mv && !s_compile_time_unlimited && (s_mv.compiles >= 2 || s_compile_us_this_frame >= 4000))
    return nullptr;
  if (s_compiles_this_frame >= s_compile_budget)
    return nullptr;
  if (!s_compile_time_unlimited && s_compiles_this_frame >= kMinCompilesPerFrame &&
      s_compile_us_this_frame >= CompileBudgetUs())
    return nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  ++s_compiles_this_frame;
  auto src = GenerateTEVShader(g_state, 0);
  if (mv && !MvInject(src.vertex_source, src.fragment_source)) { // MVGEN21
    s_mv.unsupported = true;
    std::printf("[MVGEN21] TEV program layout not recognised: no motion vectors (warp off)\n");
    return nullptr;
  }
  static const bool s_shdump1 = std::getenv("NWII_SHADERDUMP") != nullptr;
  if (s_shdump1) {
    static int sn = 0;
    if (sn++ < 2)
      printf("[SHADER %d] ==== FS ====\n%s\n============\n", sn,
             src.fragment_source.c_str());
  }
  GLShader sh;
  sh.id = CompileGLShaderProgram(src.vertex_source.c_str(), src.fragment_source.c_str());
  s_mv.compiles += mv; // MVGEN21
  if (sh.id == 0 && mv) { // MVGEN21
    s_mv.unsupported = true;
    std::printf("[MVGEN21] motion program failed to compile/link (uid=%llx): no motion vectors (warp off)\n",
                (unsigned long long)uid);
    return nullptr;
  }
  if (sh.id == 0) {
    std::fprintf(stderr, "[GX fatal] Shader program failed, uid=%llx.\n",
                 static_cast<unsigned long long>(uid));
    std::exit(EXIT_FAILURE);
  }
  for (int i = 0; i < 8; i++)
    sh.uTex[i] = glGetUniformLocation(sh.id, ("uTex" + std::to_string(i)).c_str());
  sh.uTevKColor = glGetUniformLocation(sh.id, "uTevKColor[0]");
  if (sh.uTevKColor < 0)
    sh.uTevKColor = glGetUniformLocation(sh.id, "uTevKColor");
  sh.uTevColor = glGetUniformLocation(sh.id, "uTevColor[0]");
  if (sh.uTevColor < 0)
    sh.uTevColor = glGetUniformLocation(sh.id, "uTevColor");
  sh.uProjMtx = glGetUniformLocation(sh.id, "mvp");
  sh.uMatColor = glGetUniformLocation(sh.id, "uMatColor[0]");
  sh.uAmbColor = glGetUniformLocation(sh.id, "uAmbColor[0]");
  sh.uChanCtrl = glGetUniformLocation(sh.id, "uChanCtrl[0]");
  sh.uLightCol = glGetUniformLocation(sh.id, "uLightCol[0]");
  sh.uLightPos = glGetUniformLocation(sh.id, "uLightPos[0]");
  sh.uLightDir = glGetUniformLocation(sh.id, "uLightDir[0]");
  sh.uLightCosAtt = glGetUniformLocation(sh.id, "uLightCosAtt[0]");
  sh.uLightDistAtt = glGetUniformLocation(sh.id, "uLightDistAtt[0]");
  sh.uDstAlpha = glGetUniformLocation(sh.id, "uDstAlpha");
  sh.uIndMtx = glGetUniformLocation(sh.id, "uIndMtx[0]");
  sh.uTexDim = glGetUniformLocation(sh.id, "uTexDim[0]");
  sh.uFogF = glGetUniformLocation(sh.id, "uFogF");
  sh.uFogI = glGetUniformLocation(sh.id, "uFogI");
  sh.uFogRange = glGetUniformLocation(sh.id, "uFogRange[0]");
  if (mv) // MVGEN21
    sh.uMvVp = glGetUniformLocation(sh.id, "uMvVp");
  // Constant per program: sampler units. (Texture coordinates arrive fully
  // generated per vertex -- XF texgen in EmitVertex, M1.8.)
  glUseProgram(sh.id);
  s_bound_prog = sh.id;
  for (int i = 0; i < 8; ++i)
    if (sh.uTex[i] >= 0)
      glUniform1i(sh.uTex[i], i);
  for (int g = 0; g < UG_COUNT; ++g)
    sh.ver[g] = 0; // upload everything on first use
  auto res = s_shader_cache.emplace(key, sh); // MVGEN21: key (= uid for the plain program)
  const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
  s_compile_us_this_frame += us;
  ++s_stats.compiles;
  s_stats.compile_us += us;
  if (!g_stat_hash0)
    g_stat_hash0 = uid;
  g_stat_shaders = (uint64_t)s_shader_cache.size();
  if (gx_trace())
    printf("[GXTRACE] Compiled new shader! uid: %llx\n", (unsigned long long)uid);
  return &res.first->second;
}

// Re-evaluates the dirty state categories before a draw; flushes the open
// batch only if something GL-visible changed, then applies the differences.
// ---- LIGHT17: EFB storage precision (RGBA6_Z24 / RGB565_Z16) -----------------
// Plain stores are quantised by the TEV shader (TEVOutputQuantKey). A blend or
// logic-op draw stores an 8-bit RESULT, of which the hardware keeps only the
// format's bits (after its own dither; Dolphin SW EfbInterface). EfbQuantFlush
// re-quantises the area such draws touched (union of their scissor rects) with
// their (format, dither) mode before the EFB is read (copies, XFB, peek
// snapshot) and before a draw with another format or another read-modify-write
// mode. Residuals: intermediate results of overlapping blends of one mode, and
// GL's rounding vs the hardware's truncating blend arithmetic.
// NWII_EFB_TRUECOLOR=1 = all off (8-bit EFB, Dolphin's "Force 24-bit Color").
static struct {
  bool dirty = false;
  uint32_t mode = 0; // TEVOutputQuantKey bits 0-2
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // GL px
} s_q;
static GLuint s_q_prog = 0, s_q_fbo = 0, s_q_tex = 0;
static GLint s_qu_src = -1, s_qu_mode = -1;
static int s_q_w = 0, s_q_h = 0;
uint64_t g_stat_efb_qflush = 0;
static void InvalidateGLState();
static const char *kQuantFS = R"(#version 330 core
uniform sampler2D uSrc;
uniform int uMode; // bits 0-1: 1 RGBA6, 2 RGB565; bit 2: dither
out vec4 o;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec4 v = ivec4(round(texelFetch(uSrc, p, 0) * 255.0));
  if ((uMode & 3) == 2) {
    ivec3 q = v.rgb >> ivec3(3, 2, 3);
    o = vec4(vec3(ivec3((q.r << 3) | (q.r >> 2), (q.g << 2) | (q.g >> 4), (q.b << 3) | (q.b >> 2))) / 255.0,
             float(v.a) / 255.0);
    return;
  }
  if ((uMode & 4) != 0) v.rgb = (v.rgb - (v.rgb >> 6)) + abs(((p.y + 1) & 1) * 3 - (p.x & 1) * 2);
  v >>= 2;
  o = vec4((v << 2) | (v >> 4)) / 255.0;
}
)";
// The same quantisation on CPU pixels (GL rows, RGBA8), for diagnostics.
static void EfbQuantCpu(uint8_t *px, int w, int h) {
  if (!s_q.dirty)
    return;
  for (int y = std::max(0, s_q.y0); y < std::min(h, s_q.y1); ++y)
    for (int x = std::max(0, s_q.x0); x < std::min(w, s_q.x1); ++x) {
      uint8_t *c = px + ((size_t)y * w + x) * 4;
      if ((s_q.mode & 3) == 2) {
        const int r = c[0] >> 3, g = c[1] >> 2, b = c[2] >> 3;
        c[0] = (uint8_t)((r << 3) | (r >> 2));
        c[1] = (uint8_t)((g << 2) | (g >> 4));
        c[2] = (uint8_t)((b << 3) | (b >> 2));
        continue;
      }
      const int d = (s_q.mode & 4) ? std::abs(((y + 1) & 1) * 3 - (x & 1) * 2) : 0;
      for (int k = 0; k < 4; ++k) {
        int v = c[k];
        if (k < 3 && (s_q.mode & 4))
          v = (v - (v >> 6)) + d;
        v >>= 2;
        c[k] = (uint8_t)((v << 2) | (v >> 4));
      }
    }
}
static void EfbQuantFlush() {
  if (!s_q.dirty)
    return;
  s_q.dirty = false;
  const int x0 = std::max(0, s_q.x0), y0 = std::max(0, s_q.y0);
  const int x1 = std::min(s_efb_w, s_q.x1), y1 = std::min(s_efb_h, s_q.y1);
  if (x1 <= x0 || y1 <= y0 || !s_efb_color_tex || !EnsureCopyProgram())
    return;
  if (!s_q_prog) {
    s_q_prog = CompileGLShaderProgram(kCopyVS, kQuantFS);
    if (!s_q_prog)
      return;
    s_qu_src = glGetUniformLocation(s_q_prog, "uSrc");
    s_qu_mode = glGetUniformLocation(s_q_prog, "uMode");
  }
  if (!s_q_fbo) {
    glGenTextures(1, &s_q_tex);
    glGenFramebuffers(1, &s_q_fbo);
  }
  if (s_q_w != s_efb_w || s_q_h != s_efb_h) {
    glActiveTexture(GL_TEXTURE0 + kUploadUnit);
    glBindTexture(GL_TEXTURE_2D, s_q_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_efb_w, s_efb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_q_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_q_tex, 0);
    s_q_w = s_efb_w;
    s_q_h = s_efb_h;
  }
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_q_fbo);
  glBlitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  if (s_mv.db_two) // MVGEN21: the re-quantisation pass writes colour only
    MvDrawBuffers(false);
  glEnable(GL_SCISSOR_TEST);
  glScissor(x0, y0, x1 - x0, y1 - y0);
  glDisable(GL_BLEND);
  glDisable(GL_COLOR_LOGIC_OP);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, (s_q.mode & 3) == 2 ? GL_FALSE : GL_TRUE);
  glViewport(0, 0, s_efb_w, s_efb_h);
  glUseProgram(s_q_prog);
  s_bound_prog = s_q_prog;
  glActiveTexture(GL_TEXTURE0 + kCopyUnit);
  glBindTexture(GL_TEXTURE_2D, s_q_tex);
  glBindSampler(kCopyUnit, 0);
  glUniform1i(s_qu_src, kCopyUnit);
  glUniform1i(s_qu_mode, (GLint)s_q.mode);
  glBindVertexArray(s_empty_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindTexture(GL_TEXTURE_2D, 0);
  InvalidateGLState();
  ++g_stat_efb_qflush;
}
// Called in SyncState after the batch flush: flush on a mode change, then mark
// this draw's area if it is a read-modify-write draw in a quantised format.
static void EfbQuantTrack() {
  const uint32_t qk = TEVOutputQuantKey(g_state);
  if (s_q.dirty && ((qk & 3) != (s_q.mode & 3) || ((qk & 8) && (qk & 7) != s_q.mode))) {
    FlushBatch(); // geometry of the previous state first
    EfbQuantFlush();
  }
  if (!(qk & 8) || s_fw.cmask == 0 || s_fw.sc[2] <= 0 || s_fw.sc[3] <= 0)
    return;
  const int x0 = s_fw.sc[0], y0 = s_fw.sc[1], x1 = x0 + s_fw.sc[2], y1 = y0 + s_fw.sc[3];
  if (!s_q.dirty) {
    s_q = {true, qk & 7, x0, y0, x1, y1};
  } else {
    s_q.x0 = std::min(s_q.x0, x0);
    s_q.y0 = std::min(s_q.y0, y0);
    s_q.x1 = std::max(s_q.x1, x1);
    s_q.y1 = std::max(s_q.y1, y1);
  }
}

static void SyncState() {
  ++s_stats.state_syncs;
  const uint32_t d = s_dirty;
  s_dirty = 0;
  bool flush = false;

  // TEX7: texcoord scale / texmap size changes (D_IND / D_TEX) only re-hash
  // when they flip a stage's scaled-lookup bit (TEVTexCoordScaleMismatch).
  static uint16_t s_tcs_mask = 0;
  bool rehash = (d & D_SHADER) != 0;
  if (!rehash && (d & (D_IND | D_TEX)))
    rehash = TEVTexCoordScaleMismatch(g_state) != s_tcs_mask;
  static uint32_t s_qk_mask = 0; // LIGHT17: BP 0x41/0x43 (D_PE) change the output quantisation
  if (!rehash && (d & D_PE))
    rehash = TEVOutputQuantKey(g_state) != s_qk_mask;
  if (rehash) {
    s_want_uid = ShaderUid(&s_used_maps);
    s_tcs_mask = TEVTexCoordScaleMismatch(g_state);
    s_qk_mask = TEVOutputQuantKey(g_state);
  }
  if (!s_have_uid_valid || s_want_uid != s_have_uid)
    flush = true;

  if (d & D_PE)
    ComputePE(s_fw);
  if (d & D_VIEWPORT)
    ComputeViewport(s_fw);
  if (d & D_SCISSOR)
    ComputeScissor(s_fw);
  if (!s_fixed_valid || std::memcmp(&s_fw, &s_fh, sizeof s_fw) != 0)
    flush = true;

  if (d & (D_TEX | D_SHADER)) {
    for (int i = 0; i < 8; ++i) {
      if (!((s_used_maps >> i) & 1))
        continue;
      const uint32_t m0 = TexMode0(i), m1 = TexMode1(i);
      uint32_t lv = 1;
      s_tw[i].tex = LookupTexture(g_state.texStages[i], TexLevels(g_state.texStages[i], m0, m1), &lv,
                                  ((m0 >> 5) & 3) != 0); // HDTEX16: mip filter bit
      s_tw[i].levels = lv;
      s_tw[i].mode0 = m0 & 0x1FFFF; // wrap + filter + lod bias bits
      s_tw[i].mode1 = lv > 1 ? (m1 & 0xFFFF) : 0;
    }
  }
  for (int i = 0; i < 8; ++i)
    if (((s_used_maps >> i) & 1) &&
        (!s_tex_valid[i] || s_tw[i].tex != s_th[i].tex || s_tw[i].mode0 != s_th[i].mode0 ||
         s_tw[i].mode1 != s_th[i].mode1 || s_tw[i].levels != s_th[i].levels))
      flush = true;

  UniformValues nv;
  if (UpdateGroup(d, nv))
    flush = true;

  // MVGEN21: a recorded real frame's perspective draw that is opaque (no blend/logic op), writes colour and
  // writes depth (or draws on equal depth) also writes motion: motion-variant program + the +3 vertex layout.
  const bool want_mv = s_mv.recording && g_state.projSet && g_state.projType == 0 && !s_fw.blend_on &&
                       !s_fw.logic_on && (s_fw.cmask & 7) && s_fw.ztest && (s_fw.zwrite || s_fw.zfunc == GL_EQUAL);
  static bool s_mv_want = false; // want_mv at the last program pick (a fallback to the plain program sticks)
  if (want_mv != s_mv_want)
    flush = true;

  if (flush)
    FlushBatch();
  EfbQuantTrack(); // LIGHT17 (may run a re-quantisation pass and reset GL state)

  // Program.
  if (!s_have_uid_valid || s_want_uid != s_have_uid || !s_active || want_mv != s_mv_want) { // MVGEN21: or motion
    s_active = want_mv ? FindOrCompile(s_want_uid, true) : nullptr; // MVGEN21: the motion variant first
    s_mv.fallbacks += want_mv && !s_active;
    s_mv.batch_on = s_active != nullptr;
    s_mv_want = want_mv;
    if (!s_active)
      s_active = FindOrCompile(s_want_uid);
    s_have_uid = s_want_uid;
    s_have_uid_valid = true;
    if (!s_active) {
      s_skip_draw = true;
      return;
    }
  }
  s_skip_draw = false;
  if (s_bound_prog != s_active->id) {
    glUseProgram(s_active->id);
    s_bound_prog = s_active->id;
    ++s_stats.program_binds;
  }
  ApplyFixed(s_fw);
  if (s_mv.batch_on && s_active->uMvVp >= 0) { // MVGEN21: half the viewport: NDC delta -> px
    const float hx = 0.5f * (float)s_fw.vp[2], hy = 0.5f * (float)s_fw.vp[3];
    if (s_active->mv_vp[0] != hx || s_active->mv_vp[1] != hy) {
      glUniform2f(s_active->uMvVp, hx, hy);
      s_active->mv_vp[0] = hx;
      s_active->mv_vp[1] = hy;
    }
    std::memcpy(s_mv.cur_proj, s_uv.proj, sizeof s_mv.cur_proj); // (s_uv already holds this draw's values)
    std::memcpy(s_mv.cur_vp, s_fw.vp, sizeof s_mv.cur_vp);
  }
  for (int i = 0; i < 8; ++i) {
    if (!((s_used_maps >> i) & 1))
      continue;
    if (!s_tex_valid[i] || s_tw[i].tex != s_th[i].tex) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, s_tw[i].tex);
      ++s_stats.tex_binds;
    }
    if (s_texture_samplers[i] == 0) {
      glGenSamplers(1, &s_texture_samplers[i]);
      glBindSampler(i, s_texture_samplers[i]);
      SetGXSamplerFromMode(s_texture_samplers[i], s_tw[i].mode0, s_tw[i].mode1, s_tw[i].levels);
    } else if (!s_tex_valid[i] || s_tw[i].mode0 != s_th[i].mode0 ||
               s_tw[i].mode1 != s_th[i].mode1 || s_tw[i].levels != s_th[i].levels) {
      SetGXSamplerFromMode(s_texture_samplers[i], s_tw[i].mode0, s_tw[i].mode1, s_tw[i].levels);
      glBindSampler(i, s_texture_samplers[i]);
    }
    s_th[i] = s_tw[i];
    s_tex_valid[i] = true;
  }
  UploadUniforms(*s_active);
}

// Everything GL-side is unknown (host code ran, or a copy/clear changed it).
static void InvalidateGLState() {
  s_fixed_valid = false;
  for (bool &v : s_tex_valid)
    v = false;
  s_bound_prog = 0;
  s_active = nullptr;
  s_have_uid_valid = false;
  s_dirty = D_ALL;
}

// Leaves GL in a neutral state for the host's present/blit code.
static void NeutralGLState() {
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_COLOR_LOGIC_OP);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glDepthRange(0.0, 1.0);
  for (int i = 0; i < 8; ++i)
    glBindSampler(i, 0);
  glActiveTexture(GL_TEXTURE0);
  s_fixed_valid = false;
}

// ---------------------------------------------------------------------------
// XF texture coordinate generation (M1.8)
// ---------------------------------------------------------------------------
// XF post-transform matrix memory 0x0500-0x05FF (64 rows of 4), applied in
// stream order like the rest of XF matrix memory (Render()). GXInit loads
// GX_PTIDENTITY (rows 61-63); a gxreplay capture starts after that load, so
// those rows start as identity and every other row as zero.
static float s_post_mtx[256] = {};
[[maybe_unused]] static const bool s_post_mtx_init = [] {
  s_post_mtx[61 * 4 + 0] = s_post_mtx[62 * 4 + 1] = s_post_mtx[63 * 4 + 2] = 1.0f;
  return true;
}();

// One vertex through the XF texgen units, per Dolphin VertexShaderGen
// (GenerateVertexShaderCode texgen loop + dolphin_transform_texcoord):
//  - input: source row (XF 0x1040+i bits 7-11) GEOM = model position,
//    NORMAL = model normal, TEX0..7 = (s, t, 1); default (0, 0, 1, 1);
//    input form AB11 forces z = 1; NaN -> 1.
//  - regular: rows texMtxIdx, +1 (+2 for STQ) of XF matrix memory; ST gives q = 1.
//  - dual texture transform (XF 0x1012 bit 0): optional normalize, then the
//    post matrix rows (XF 0x1050+i bits 0-5, +1, +2; wrapping at 64).
//  - q == 0: xy = clamp(xy / 2, -1, 1).
//  - colour texgens use the vertex colour (the lit colour lives in the VS);
//    emboss copies its source coordinate (no tangent/binormal decode).
// Coordinates >= numTexGens repeat coordinate 0: a TEV stage that names a
// texcoord that is not generated samples texcoord 0 (Dolphin PixelShaderGen
// quirk); with no texgen at all every coordinate is (0, 0, 1).
static inline float Dot4(const float *m, const float c[4]) {
  return m[0] * c[0] + m[1] * c[1] + m[2] * c[2] + m[3] * c[3];
}
// Per-draw texgen configuration (XF 0x1012, 0x103F, 0x1040+i, 0x1050+i).
struct TexGenSetup {
  uint32_t n = 0;
  bool dual = false;
  struct Coord {
    uint32_t info, post;
  } c[8];
};
static TexGenSetup MakeTexGenSetup() {
  TexGenSetup t;
  t.n = std::min<uint32_t>(g_state.xf[0x3F] & 0xF, 8);
  t.dual = g_state.xf[0x12] & 1;
  for (uint32_t i = 0; i < t.n; ++i)
    t.c[i] = {g_state.xf[0x40 + i], g_state.xf[0x50 + i]};
  return t;
}
// Writes texgen outputs 0..k-1 (k <= 8) of one vertex.
static void TexGen(const TexGenSetup &t, const VertexData &vtx, float (*out)[3], uint32_t k) {
  const uint32_t n = std::min(t.n, k);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t info = t.c[i].info;
    const bool stq = (info >> 1) & 1;
    const bool abc1 = (info >> 2) & 1;
    const uint32_t type = (info >> 4) & 7;
    const uint32_t row = (info >> 7) & 0x1F;
    float *o = out[i];
    if (type == 1) { // emboss
      const uint32_t s = (info >> 12) & 7;
      if (s < i) {
        o[0] = out[s][0]; o[1] = out[s][1]; o[2] = out[s][2];
      } else {
        o[0] = o[1] = 0.0f; o[2] = 1.0f;
      }
      continue;
    }
    if (type == 2 || type == 3) { // colour0 / colour1
      o[0] = vtx.has_color ? vtx.color[0] : 1.0f;
      o[1] = vtx.has_color ? vtx.color[1] : 1.0f;
      o[2] = 1.0f;
      continue;
    }
    float c[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    if (row == 0) {
      if (vtx.has_pos) { c[0] = vtx.raw_pos[0]; c[1] = vtx.raw_pos[1]; c[2] = vtx.raw_pos[2]; }
    } else if (row == 1) {
      if (vtx.has_norm) { c[0] = vtx.raw_norm[0]; c[1] = vtx.raw_norm[1]; c[2] = vtx.raw_norm[2]; }
    } else if (row >= 5 && row <= 12) {
      if (vtx.has_tex[row - 5]) { c[0] = vtx.tex[row - 5][0]; c[1] = vtx.tex[row - 5][1]; }
    }
    if (!abc1)
      c[2] = 1.0f;
    for (int q = 0; q < 3; ++q)
      if (c[q] != c[q]) // NaN -> 1
        c[q] = 1.0f;
    const uint32_t ti = vtx.texMtxIdx[i] & 0x3F;
    float r[3] = {0.0f, 0.0f, 1.0f};
    if (ti * 4 + (stq ? 12u : 8u) <= 256u) {
      const float *m = &g_state.posMatrices[ti * 4];
      r[0] = Dot4(m, c);
      r[1] = Dot4(m + 4, c);
      r[2] = stq ? Dot4(m + 8, c) : 1.0f;
    }
    if (t.dual) {
      const uint32_t post = t.c[i].post;
      const uint32_t pi = post & 0x3F;
      if ((post >> 8) & 1) {
        const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (len > 0.0f) { r[0] /= len; r[1] /= len; r[2] /= len; }
      }
      const float *p0 = &s_post_mtx[(pi & 0x3F) * 4];
      const float *p1 = &s_post_mtx[((pi + 1) & 0x3F) * 4];
      const float *p2 = &s_post_mtx[((pi + 2) & 0x3F) * 4];
      const float v[4] = {r[0], r[1], r[2], 1.0f};
      r[0] = Dot4(p0, v);
      r[1] = Dot4(p1, v);
      r[2] = Dot4(p2, v);
    }
    if (r[2] == 0.0f) {
      r[0] = std::clamp(r[0] / 2.0f, -1.0f, 1.0f);
      r[1] = std::clamp(r[1] / 2.0f, -1.0f, 1.0f);
    }
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
  }
  for (uint32_t i = n; i < k; ++i) {
    if (n) {
      out[i][0] = out[0][0]; out[i][1] = out[0][1]; out[i][2] = out[0][2];
    } else {
      out[i][0] = out[i][1] = 0.0f; out[i][2] = 1.0f;
    }
  }
}

// ---------------------------------------------------------------------------
// Vertex emission
// ---------------------------------------------------------------------------
// One GX vertex as the first BatchFloats(k) floats of a BatchVertex.
static void ConvertVertex(const VertexData &vtx, const TexGenSetup &tg, int k, float *dst) {
  BatchVertex bv;
  bv.nx = bv.ny = bv.nz = 0.0f;
  if (vtx.has_color) {
    bv.r = vtx.color[0];
    bv.g = vtx.color[1];
    bv.b = vtx.color[2];
    bv.a = vtx.color[3];
  } else {
    bv.r = bv.g = bv.b = bv.a = 1.f;
  }
  if (vtx.has_norm && vtx.pre_xf) {
    bv.nx = vtx.norm[0]; bv.ny = vtx.norm[1]; bv.nz = vtx.norm[2];
  } else if (vtx.has_norm) {
    // Normal matrix for a position matrix index: 3 * (idx & 31), three rows
    // of three (XF normal matrix bank layout).
    int nb = 3 * (vtx.posMtxIdx & 31);
    const float *nm = g_state.normalMatrices;
    if (nb + 9 <= 96) {
      bv.nx = vtx.norm[0] * nm[nb + 0] + vtx.norm[1] * nm[nb + 1] +
              vtx.norm[2] * nm[nb + 2];
      bv.ny = vtx.norm[0] * nm[nb + 3] + vtx.norm[1] * nm[nb + 4] +
              vtx.norm[2] * nm[nb + 5];
      bv.nz = vtx.norm[0] * nm[nb + 6] + vtx.norm[1] * nm[nb + 7] +
              vtx.norm[2] * nm[nb + 8];
    } else {
      bv.nx = vtx.norm[0]; bv.ny = vtx.norm[1]; bv.nz = vtx.norm[2];
    }
  }
  TexGen(tg, vtx, bv.tc, (uint32_t)k);

  float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
  int mtx_base = vtx.posMtxIdx * 4;
  if (vtx.pre_xf) {
    // Already transformed at decode with the per-draw matrix snapshot.
    bv.x = x;
    bv.y = y;
    bv.z = z;
  } else if (vtx.has_pos && mtx_base + 12 <= 256) {
    const float *mm = g_state.posMatrices;
    bv.x = x * mm[mtx_base + 0] + y * mm[mtx_base + 1] + z * mm[mtx_base + 2] +
           mm[mtx_base + 3];
    bv.y = x * mm[mtx_base + 4] + y * mm[mtx_base + 5] + z * mm[mtx_base + 6] +
           mm[mtx_base + 7];
    bv.z = x * mm[mtx_base + 8] + y * mm[mtx_base + 9] + z * mm[mtx_base + 10] +
           mm[mtx_base + 11];
  } else {
    bv.x = x;
    bv.y = y;
    bv.z = z;
  }
  // Z is kept for orthographic draws too (the projection maps it like GX);
  // whether it is tested/written is the PE Z mode's business.
  static const bool s_vtxtrace_v = std::getenv("NWII_VTXTRACE") != nullptr;
  if (s_vtxtrace_v) {
    static int vn = 0;
    if (vn++ < 14)
      printf("[VTX] raw=(%.1f,%.1f,%.1f) view=(%.1f,%.1f,%.1f) uv=(%.3f,%.3f) "
             "tex0=%ux%u fmt=%u\n",
             vtx.pos[0], vtx.pos[1], vtx.pos[2], bv.x, bv.y, bv.z, bv.tc[0][0], bv.tc[0][1],
             g_state.texStages[0].width, g_state.texStages[0].height,
             g_state.texStages[0].format);
  }
  std::memcpy(dst, &bv.x, BatchFloats(k) * sizeof(float));
}

// PERF21: ConvertVertex for all n vertices of a draw, bit-identical to calling it per vertex: every
// output float comes from the same float operations in the same order. What changes is the loop
// structure: position/normal/colour in one pass, then each texgen coordinate in its own pass over the
// vertices, with the XF 0x1040+i / 0x1050+i decoding and the input-row selection done once per draw
// instead of once per vertex and coordinate (TexGen was the top host function in Skyloft, INTERP17).
// dst: n vertices of F floats (F >= BatchFloats(k)); floats from BatchFloats(k) on are left alone.
static void ConvertDraw(const VertexData *v, size_t n, const TexGenSetup &t, int k, float *__restrict dst,
                        size_t F) {
  const float *mm = g_state.posMatrices;
  const float *nm = g_state.normalMatrices;
  for (size_t j = 0; j < n; ++j) {
    const VertexData &vtx = v[j];
    float *o = dst + j * F;
    // colour (o[3..6]), normal (o[7..9]), position (o[0..2]) exactly as ConvertVertex
    if (vtx.has_color) {
      o[3] = vtx.color[0]; o[4] = vtx.color[1]; o[5] = vtx.color[2]; o[6] = vtx.color[3];
    } else {
      o[3] = o[4] = o[5] = o[6] = 1.f;
    }
    if (vtx.has_norm && vtx.pre_xf) {
      o[7] = vtx.norm[0]; o[8] = vtx.norm[1]; o[9] = vtx.norm[2];
    } else if (vtx.has_norm) {
      const int nb = 3 * (vtx.posMtxIdx & 31);
      if (nb + 9 <= 96) {
        o[7] = vtx.norm[0] * nm[nb + 0] + vtx.norm[1] * nm[nb + 1] + vtx.norm[2] * nm[nb + 2];
        o[8] = vtx.norm[0] * nm[nb + 3] + vtx.norm[1] * nm[nb + 4] + vtx.norm[2] * nm[nb + 5];
        o[9] = vtx.norm[0] * nm[nb + 6] + vtx.norm[1] * nm[nb + 7] + vtx.norm[2] * nm[nb + 8];
      } else {
        o[7] = vtx.norm[0]; o[8] = vtx.norm[1]; o[9] = vtx.norm[2];
      }
    } else {
      o[7] = o[8] = o[9] = 0.0f;
    }
    const float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
    const int mb = vtx.posMtxIdx * 4;
    if (vtx.pre_xf) {
      o[0] = x; o[1] = y; o[2] = z;
    } else if (vtx.has_pos && mb + 12 <= 256) {
      o[0] = x * mm[mb + 0] + y * mm[mb + 1] + z * mm[mb + 2] + mm[mb + 3];
      o[1] = x * mm[mb + 4] + y * mm[mb + 5] + z * mm[mb + 6] + mm[mb + 7];
      o[2] = x * mm[mb + 8] + y * mm[mb + 9] + z * mm[mb + 10] + mm[mb + 11];
    } else {
      o[0] = x; o[1] = y; o[2] = z;
    }
  }
  // texgen outputs 0..k-1 (TexGen): one pass per coordinate
  const uint32_t ng = std::min<uint32_t>(t.n, (uint32_t)k);
  for (uint32_t i = 0; i < ng; ++i) {
    const uint32_t info = t.c[i].info;
    const bool stq = (info >> 1) & 1;
    const bool abc1 = (info >> 2) & 1;
    const uint32_t type = (info >> 4) & 7;
    const uint32_t row = (info >> 7) & 0x1F;
    float *oi = dst + 10 + 3 * i;
    if (type == 1) { // emboss: copy of an earlier output of the same vertex
      const uint32_t s = (info >> 12) & 7;
      for (size_t j = 0; j < n; ++j) {
        float *o = oi + j * F;
        if (s < i) {
          const float *so = dst + j * F + 10 + 3 * s;
          o[0] = so[0]; o[1] = so[1]; o[2] = so[2];
        } else {
          o[0] = o[1] = 0.0f; o[2] = 1.0f;
        }
      }
      continue;
    }
    if (type == 2 || type == 3) { // colour0 / colour1
      for (size_t j = 0; j < n; ++j) {
        float *o = oi + j * F;
        o[0] = v[j].has_color ? v[j].color[0] : 1.0f;
        o[1] = v[j].has_color ? v[j].color[1] : 1.0f;
        o[2] = 1.0f;
      }
      continue;
    }
    // regular: input (s, t, u, 1) from the source row, rows texMtxIdx[i] (+1, +2 for STQ)
    const int src = row == 0 ? 0 : row == 1 ? 1 : (row >= 5 && row <= 12) ? 2 : 3;
    const uint32_t tr = row - 5;
    const bool dual = t.dual;
    const uint32_t post = t.c[i].post, pi = post & 0x3F;
    const bool pnorm = (post >> 8) & 1;
    const float *p0 = &s_post_mtx[(pi & 0x3F) * 4];
    const float *p1 = &s_post_mtx[((pi + 1) & 0x3F) * 4];
    const float *p2 = &s_post_mtx[((pi + 2) & 0x3F) * 4];
    const uint32_t need = stq ? 12u : 8u;
    for (size_t j = 0; j < n; ++j) {
      const VertexData &vtx = v[j];
      float c[4] = {0.0f, 0.0f, 1.0f, 1.0f};
      if (src == 0) {
        if (vtx.has_pos) { c[0] = vtx.raw_pos[0]; c[1] = vtx.raw_pos[1]; c[2] = vtx.raw_pos[2]; }
      } else if (src == 1) {
        if (vtx.has_norm) { c[0] = vtx.raw_norm[0]; c[1] = vtx.raw_norm[1]; c[2] = vtx.raw_norm[2]; }
      } else if (src == 2) {
        if (vtx.has_tex[tr]) { c[0] = vtx.tex[tr][0]; c[1] = vtx.tex[tr][1]; }
      }
      if (!abc1)
        c[2] = 1.0f;
      for (int q = 0; q < 3; ++q)
        if (c[q] != c[q]) // NaN -> 1
          c[q] = 1.0f;
      const uint32_t ti = vtx.texMtxIdx[i] & 0x3F;
      float r[3] = {0.0f, 0.0f, 1.0f};
      if (ti * 4 + need <= 256u) {
        const float *m = &mm[ti * 4];
        r[0] = Dot4(m, c);
        r[1] = Dot4(m + 4, c);
        r[2] = stq ? Dot4(m + 8, c) : 1.0f;
      }
      if (dual) {
        if (pnorm) {
          const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
          if (len > 0.0f) { r[0] /= len; r[1] /= len; r[2] /= len; }
        }
        const float vv[4] = {r[0], r[1], r[2], 1.0f};
        r[0] = Dot4(p0, vv);
        r[1] = Dot4(p1, vv);
        r[2] = Dot4(p2, vv);
      }
      if (r[2] == 0.0f) {
        r[0] = std::clamp(r[0] / 2.0f, -1.0f, 1.0f);
        r[1] = std::clamp(r[1] / 2.0f, -1.0f, 1.0f);
      }
      float *o = oi + j * F;
      o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
    }
  }
  for (uint32_t i = ng; i < (uint32_t)k; ++i) { // coordinates >= numTexGens repeat coordinate 0
    for (size_t j = 0; j < n; ++j) {
      float *o = dst + j * F + 10 + 3 * i;
      if (ng) {
        const float *z0 = dst + j * F + 10;
        o[0] = z0[0]; o[1] = z0[1]; o[2] = z0[2];
      } else {
        o[0] = o[1] = 0.0f; o[2] = 1.0f;
      }
    }
  }
}

static void EmitPrimitive(const GXCommand &cmd) {
  const auto &v = cmd.vertices;
  const size_t n = v.size();
  // Batch layout follows this draw's texgen count (see s_batch).
  const int k = std::max<int>(1, (int)std::min<uint32_t>(g_state.xf[0x3F] & 0xF, 8));
  if (k != s_batch_k || s_mv.batch_on != s_batch_mv) { // MVGEN21: or the motion layout
    FlushBatch();
    s_batch_k = k;
    s_batch_mv = s_mv.batch_on;
  }
  const size_t F = BatchStride(k); // MVGEN21: + previous view-space position (PERF21: BatchStride)
  const TexGenSetup tg = MakeTexGenSetup();
  static const bool cpu_clip = TEVColorNoPerspective();
  static const bool s_fast = [] { // NWII_PERF21=0: every draw through the copy path below (old corner order)
    const char *e = std::getenv("NWII_PERF21");
    return !(e && *e == '0');
  }();
  const uint8_t pt = cmd.prim_type;
  if (s_fast && !cpu_clip && (pt == 0x80 || pt == 0x88 || pt == 0x90 || pt == 0x98 || pt == 0xA0)) {
    // PERF21: triangles: every vertex converted once straight into the batch + an index list with
    // exactly the triangles (and corner order) the copy path below emits.
    const uint32_t b = s_batch_nv;
    float *dst = s_batch.grow(n * F);
    ConvertDraw(v.data(), n, tg, k, dst, F);
    s_batch_nv += (uint32_t)n;
    if (s_interp_hook) // INTERP17: in-between frame: interpolated positions/normals
      s_interp_hook->PostConvert(cmd, dst, F);
    if (s_batch_mv) // MVGEN21: where each vertex was in the previous frame (the pipeline's INTERP17 match)
      s_mv.arm.hook->Prev(cmd, dst, F, BatchFloats(k));
    switch (pt) {
    case 0x80: // quads
    case 0x88: { // quads_2 (drawn as quads, as Dolphin does)
      uint32_t *o = s_bidx.grow((n / 4) * 6);
      for (uint32_t i = 0; i + 3 < n; i += 4, o += 6) {
        o[0] = b + i; o[1] = b + i + 1; o[2] = b + i + 2;
        o[3] = b + i + 2; o[4] = b + i + 3; o[5] = b + i;
      }
      break;
    }
    case 0x90: {
      uint32_t *o = s_bidx.grow((n / 3) * 3);
      for (uint32_t i = 0; i + 2 < n; i += 3, o += 3) {
        o[0] = b + i; o[1] = b + i + 1; o[2] = b + i + 2;
      }
      break;
    }
    case 0x98:
      if (n >= 3) {
        uint32_t *o = s_bidx.grow((n - 2) * 3);
        for (uint32_t i = 2; i < n; ++i, o += 3) {
          if (i & 1) { o[0] = b + i - 1; o[1] = b + i - 2; }
          else { o[0] = b + i - 2; o[1] = b + i - 1; }
          o[2] = b + i;
        }
      }
      break;
    default: // 0xA0 fan
      if (n >= 3) {
        uint32_t *o = s_bidx.grow((n - 2) * 3);
        for (uint32_t i = 2; i < n; ++i, o += 3) {
          o[0] = b; o[1] = b + i - 1; o[2] = b + i;
        }
      }
      break;
    }
    return;
  }
  // Convert every source vertex once (quads/strips/fans reuse them).
  static std::vector<float> s_conv;
  s_conv.resize(n * F);
  for (size_t i = 0; i < n; ++i)
    ConvertVertex(v[i], tg, k, &s_conv[i * F]);
  if (s_interp_hook) // INTERP17: in-between frame: interpolated positions/normals
    s_interp_hook->PostConvert(cmd, s_conv.data(), F);
  if (s_batch_mv) // MVGEN21: where each vertex was in the previous frame (the pipeline's INTERP17 match)
    s_mv.arm.hook->Prev(cmd, s_conv.data(), F, BatchFloats(k));
  // PERF21: this path (CPU clipping, lines, points) appends a copy per emitted corner + its index.
  auto Put = [&](const float *src) {
    std::memcpy(s_batch.grow(F), src, F * sizeof(float));
    *s_bidx.grow(1) = s_batch_nv++;
  };
  auto EmitVertex = [&](const VertexData &vd) {
    Put(&s_conv[(size_t)(&vd - v.data()) * F]);
  };
  // LIGHT17 (opt-in NWII_COLOR_SCREENLINEAR=1, default off): with screen-linear colours a triangle that crosses
  // the view frustum must be clipped here, as Dolphin SW's Clipper does (near
  // plane and exactly the viewport in x/y; no guard band): new vertices get every
  // attribute lerped in clip space (perspective-correct), the inside stays
  // screen-linear. GL clips only near/far with window-space noperspective values
  // (meaningless behind the eye) and interpolates huge ground triangles across
  // their whole off-screen extent. NWII_CLIP_NEAR_ONLY=1: near plane only (A/B).
  // (cpu_clip = TEVColorNoPerspective(), above)
  static const bool near_only = std::getenv("NWII_CLIP_NEAR_ONLY") != nullptr;
  const float *P = s_uv.proj;
  struct CV {
    float a[37]; // MVGEN21: 34 + 3 motion floats
    float c[4]; // clip x y z w
  };
  auto ClipOf = [&](const float *q, float *c) {
    for (int r = 0; r < 4; ++r)
      c[r] = P[r] * q[0] + P[4 + r] * q[1] + P[8 + r] * q[2] + P[12 + r];
  };
  // plane 0: near z + w; 1..4: w - x, x + w, w - y, y + w (Dolphin CLIP_*_BIT)
  auto Dist = [](const float *c, int pl) {
    switch (pl) {
    case 0: return c[2] + c[3];
    case 1: return c[3] - c[0];
    case 2: return c[0] + c[3];
    case 3: return c[3] - c[1];
    default: return c[1] + c[3];
    }
  };
  auto EmitTri = [&](const VertexData &a, const VertexData &b, const VertexData &c) {
    if (!cpu_clip) {
      EmitVertex(a);
      EmitVertex(b);
      EmitVertex(c);
      return;
    }
    const VertexData *t3[3] = {&a, &b, &c};
    float cc[3][4];
    unsigned any = 0, all = 0x1F;
    for (int j = 0; j < 3; ++j) {
      ClipOf(&s_conv[(size_t)(t3[j] - v.data()) * F], cc[j]);
      unsigned m = 0;
      for (int pl = 0; pl < (near_only ? 1 : 5); ++pl)
        if (Dist(cc[j], pl) < 0.0f)
          m |= 1u << pl;
      any |= m;
      all &= m;
    }
    if (all) // outside one plane entirely: nothing visible
      return;
    if (!any) {
      EmitVertex(a);
      EmitVertex(b);
      EmitVertex(c);
      return;
    }
    CV buf[2][10];
    int np = 3;
    for (int j = 0; j < 3; ++j) {
      std::memcpy(buf[0][j].a, &s_conv[(size_t)(t3[j] - v.data()) * F], F * sizeof(float));
      std::memcpy(buf[0][j].c, cc[j], sizeof cc[j]);
    }
    int cur = 0;
    for (int pl = 0; pl < 5 && np >= 3; ++pl) {
      if (!((any >> pl) & 1))
        continue;
      CV *in = buf[cur], *o = buf[cur ^ 1];
      int no = 0;
      for (int j = 0; j < np && no < 9; ++j) { // Sutherland-Hodgman, lerp from the inside end
        const CV &p0 = in[j], &p1 = in[(j + 1) % np];
        const float d0 = Dist(p0.c, pl), d1 = Dist(p1.c, pl);
        if (d0 >= 0.0f)
          o[no++] = p0;
        if ((d0 >= 0.0f) != (d1 >= 0.0f)) {
          const CV &pi = d0 >= 0.0f ? p0 : p1, &po = d0 >= 0.0f ? p1 : p0;
          const float di = d0 >= 0.0f ? d0 : d1, dout = d0 >= 0.0f ? d1 : d0;
          const float t = di / (di - dout);
          CV &nv = o[no++];
          for (size_t f = 0; f < F; ++f)
            nv.a[f] = pi.a[f] + (po.a[f] - pi.a[f]) * t;
          for (int r = 0; r < 4; ++r)
            nv.c[r] = pi.c[r] + (po.c[r] - pi.c[r]) * t;
        }
      }
      np = no;
      cur ^= 1;
    }
    const CV *poly = buf[cur];
    for (int j = 1; j + 1 < np; ++j) { // fan, original winding
      Put(poly[0].a);
      Put(poly[j].a);
      Put(poly[j + 1].a);
    }
  };

  switch (cmd.prim_type) {
  case 0x80: // quads
  case 0x88: // quads_2 (drawn as quads, as Dolphin does)
    for (size_t i = 0; i + 3 < n; i += 4) {
      EmitTri(v[i], v[i + 1], v[i + 2]);
      EmitTri(v[i + 2], v[i + 3], v[i]);
    }
    break;
  case 0x90:
    for (size_t i = 0; i + 2 < n; i += 3)
      EmitTri(v[i], v[i + 1], v[i + 2]);
    break;
  case 0x98:
    for (size_t i = 2; i < n; i++) {
      if (i & 1)
        EmitTri(v[i - 1], v[i - 2], v[i]);
      else
        EmitTri(v[i - 2], v[i - 1], v[i]);
    }
    break;
  case 0xA0:
    for (size_t i = 2; i < n; i++)
      EmitTri(v[0], v[i - 1], v[i]);
    break;
  case 0xA8: // lines
  case 0xB0: // line strip
  case 0xB8: { // points
    // TEX7 (M3.4): GX lines/points become quads, as Dolphin GeometryShaderGen
    // does: a line is widened by BP 0x22 linesize/6 EFB px along the screen
    // axis it runs least along; a point is a pointsize/6 px square. The offset
    // is made in clip space (d_clip = d_ndc * w) and mapped back to the batch's
    // view-space position with the projection's x/y scale (z unchanged). Both
    // windings are emitted: GX never culls lines or points.
    const float *P = s_uv.proj;
    const float vpw = 2.0f * XfAsFloat(0x1A), vph = -2.0f * XfAsFloat(0x1B);
    if (P[0] == 0.0f || P[5] == 0.0f || vpw == 0.0f || vph == 0.0f)
      break;
    const uint32_t lp = g_state.bp[0x22];
    const float lw = (float)(lp & 0xFF) / 6.0f, ps = (float)((lp >> 8) & 0xFF) / 6.0f;
    auto clip_w = [&](const float *q) { return P[3] * q[0] + P[7] * q[1] + P[11] * q[2] + P[15]; };
    auto ndc = [&](const float *q, float &x, float &y) {
      const float w = clip_w(q);
      x = (P[0] * q[0] + P[4] * q[1] + P[8] * q[2] + P[12]) / w;
      y = (P[1] * q[0] + P[5] * q[1] + P[9] * q[2] + P[13]) / w;
    };
    float corner[4][37]; // MVGEN21: 34 + 3 motion floats (a corner keeps its vertex's motion)
    auto make = [&](int slot, size_t vi, float ox, float oy) {
      const float *src = &s_conv[vi * F];
      std::memcpy(corner[slot], src, F * sizeof(float));
      const float w = clip_w(src);
      corner[slot][0] += ox * w / P[0];
      corner[slot][1] += oy * w / P[5];
      if (s_batch_mv) { // MVGEN21: the corner moves with its vertex (same offset on the previous position)
        corner[slot][BatchFloats(k)] += ox * w / P[0];
        corner[slot][BatchFloats(k) + 1] += oy * w / P[5];
      }
    };
    auto quad = [&]() { // corners 0..3 = a-, a+, b-, b+ ; both windings
      static const int tri[12] = {0, 1, 2, 1, 3, 2, 0, 2, 1, 1, 2, 3};
      for (int t : tri)
        Put(corner[t]);
    };
    // Texcoord offsets (Dolphin GeometryShaderGen texOffset): BP 0x22 lineoff
    // (bits 16-18) / pointoff (19-21) select 0, 1/16 .. 1 (GX_TO_*); each
    // texcoord opts in with BP 0x30+2i bit 18 (lines) / 19 (points). JParticle
    // enables both for coords 0-2 so point/line particles span their texture.
    static const float kTexOff[8] = {0.f, 1.f / 16, 1.f / 8, 1.f / 4, 1.f / 2, 1.f, 1.f, 1.f};
    auto texoff = [&](int slot, uint32_t bit, float ds, float dt, float to) {
      if (to == 0.0f)
        return;
      for (int t = 0; t < k; ++t)
        if ((g_state.bp[0x30 + 2 * t] >> bit) & 1) {
          corner[slot][10 + 3 * t + 0] += ds * to;
          corner[slot][10 + 3 * t + 1] += dt * to;
        }
    };
    if (cmd.prim_type == 0xB8) {
      const float ox = ps / vpw, oy = ps / vph;
      const float to = kTexOff[(lp >> 19) & 7];
      for (size_t i = 0; i < n; ++i) {
        make(0, i, -ox, oy); make(1, i, ox, oy); make(2, i, -ox, -oy); make(3, i, ox, -oy);
        // screen top-left (0,0), top-right (1,0), bottom-left (0,1), bottom-right (1,1)
        texoff(1, 19, 1.f, 0.f, to); texoff(2, 19, 0.f, 1.f, to); texoff(3, 19, 1.f, 1.f, to);
        quad();
      }
      break;
    }
    const bool strip = cmd.prim_type == 0xB0;
    for (size_t i = 0; i + 1 < n; i += strip ? 1 : 2) {
      float ax, ay, bx, by;
      ndc(&s_conv[i * F], ax, ay);
      ndc(&s_conv[(i + 1) * F], bx, by);
      float ox = 0.0f, oy = 0.0f;
      if (vph * std::fabs(by - ay) > vpw * std::fabs(bx - ax))
        ox = lw / vpw; // mostly vertical: widen left/right
      else
        oy = -lw / vph; // mostly horizontal: widen up/down (Dolphin sign: + side is below)
      make(0, i, -ox, -oy); make(1, i, ox, oy); make(2, i + 1, -ox, -oy); make(3, i + 1, ox, oy);
      // Dolphin: the + side ("r") gets s += texOffset for line-offset coords.
      const float to = kTexOff[(lp >> 16) & 7];
      texoff(1, 18, 1.f, 0.f, to); texoff(3, 18, 1.f, 0.f, to);
      quad();
    }
    break;
  }
  }
}

static void DrawPrimitive(const GXCommand &cmd) {
  ++s_stats.draws;
  const size_t n = cmd.vertices.size();
  if (n == 0)
    return;
  static const bool s_drawlog = std::getenv("NWII_DRAWLOG") != nullptr;
  if (s_drawlog) {
    static int dn = 0;
    if (dn < 40) {
      uint8_t used = 0;
      printf("[DRAW %02d] prim=0x%02X verts=%zu uid=%llx tex0=%dx%d fmt=%d "
             "map=%d | bp00=%06X F3=%06X C0=%06X C1=%06X 28=%06X 41=%06X\n",
             dn, cmd.prim_type, n, (unsigned long long)ShaderUid(&used),
             g_state.texStages[0].width, g_state.texStages[0].height,
             g_state.texStages[0].format, (int)g_state.tevStages[0].texMap,
             g_state.bp[0x00], g_state.bp[0xF3], g_state.bp[0xC0],
             g_state.bp[0xC1], g_state.bp[0x28], g_state.bp[0x41]);
      ++dn;
    }
  }
  if (s_dirty)
    SyncState();
  if (s_skip_draw) {
    ++s_skipped_draws;
    ++s_stats.skipped;
    return; // program for this state not compiled yet
  }
  EmitPrimitive(cmd);
}

// ---------------------------------------------------------------------------
// EFB copies (BP 0x52) and copy-clear
// ---------------------------------------------------------------------------
struct CopyRect {
  int x, y, w, h; // GX EFB coordinates, top-left origin
};

static CopyRect CopySourceRect() {
  const uint32_t src = g_state.bp[0x49], wh = g_state.bp[0x4A];
  CopyRect r{(int)(src & 0x3FF), (int)((src >> 10) & 0x3FF),
             (int)(wh & 0x3FF) + 1, (int)((wh >> 10) & 0x3FF) + 1};
  r.x = std::min(r.x, s_efb_lw);
  r.y = std::min(r.y, s_efb_lh);
  r.w = std::min(r.w, s_efb_lw - r.x);
  r.h = std::min(r.h, s_efb_lh - r.y);
  return r;
}

// RES9: a GX rect in scaled EFB px (GL rows still top-left based).
static CopyRect Scaled(const CopyRect &r) {
  return {r.x * s_scale, r.y * s_scale, r.w * s_scale, r.h * s_scale};
}

// Dolphin BPFunctions::ClearScreen: only the copy source rect, only the
// channels the PE would update (alpha only for RGBA6_Z24), Z only with Z
// update on; colour from BP 0x4F/0x50, Z from BP 0x51.
//
// Strict register semantics. The former NWII_XFB_ZCLEAR workaround (XFB copy
// with clear also cleared Z) is gone: it covered stale BP 0x40 values that
// came from the native GXSetZMode seam skipping the SDK __GXData shadow. The
// SDK setters are recompiled now (GXSEAM.md), so GXCopyDisp's clear-time Z
// mode carries the update bit EGG's XfbManager::copyEFB set.
static void ClearScreen(const CopyRect &r) {
  const uint32_t cm = g_state.bp[0x41];
  const uint32_t pf = g_state.bp[0x43] & 7;
  const bool ce = (cm >> 3) & 1;
  const bool ae = ((cm >> 4) & 1) && pf == 1;
  const bool ze = (g_state.bp[0x40] >> 4) & 1;
  if (!(ce || ae || ze) || r.w <= 0 || r.h <= 0)
    return;
  uint32_t a = (g_state.bp[0x4F] >> 8) & 0xFF, rr = g_state.bp[0x4F] & 0xFF;
  uint32_t gg = (g_state.bp[0x50] >> 8) & 0xFF, bb = g_state.bp[0x50] & 0xFF;
  if (pf == 1) { // RGBA6: drop the extra precision like the hardware
    auto q6 = [](uint32_t v) { v >>= 2; return (v << 2) | (v >> 4); };
    a = q6(a); rr = q6(rr); gg = q6(gg); bb = q6(bb);
  } else if (pf == 2) { // RGB565
    auto q5 = [](uint32_t v) { v >>= 3; return (v << 3) | (v >> 2); };
    auto q6 = [](uint32_t v) { v >>= 2; return (v << 2) | (v >> 4); };
    rr = q5(rr); gg = q6(gg); bb = q5(bb);
  }
  const uint32_t z = g_state.bp[0x51] & 0xFFFFFF;
  const CopyRect sr = Scaled(r);
  if (s_mv.db_two) // MVGEN21: a copy-clear never touches the motion attachment
    MvDrawBuffers(false);
  glEnable(GL_SCISSOR_TEST);
  glScissor(sr.x, s_efb_h - sr.y - sr.h, sr.w, sr.h);
  glColorMask(ce, ce, ce, ae);
  glDepthMask(ze ? GL_TRUE : GL_FALSE);
  glClearColor(rr / 255.f, gg / 255.f, bb / 255.f, a / 255.f);
  glClearDepth(z / 16777216.0);
  glClear(((ce || ae) ? GL_COLOR_BUFFER_BIT : 0) | (ze ? GL_DEPTH_BUFFER_BIT : 0));
  s_fixed_valid = false;
  ++s_stats.clears;
}

// PEEK10 (M3.1): copy filter coefficients (BP 0x53 = w0..w3, BP 0x54 =
// w4..w6, 6 bits each), Dolphin TextureCacheBase::GetRAMCopyFilterCoefficients.
// NWII_COPY_FILTER=0 = Dolphin's "Disable Copy Filter" hack
// (GetVRAMCopyFilterCoefficients): all weight on the middle row, so a game's
// brightness scaling is kept but the vertical blur is not. Default: the
// hardware filter (SS blurs its EGG CapTexture captures with it:
// VFILTER_BLUR {21,0,0,22,0,21,0}, CapTexture::fn_8049D9D0).
// Both registers zero = never written (unit-test fixtures; GXInit always
// writes them): treated as the identity instead of black copies.
struct CopyFilter {
  float u = 0.f, m = 1.f, l = 0.f;
  bool on = false; // not the identity (0, 1, 0)
};
static CopyFilter CurrentCopyFilter() {
  static const bool disable = [] {
    const char *e = std::getenv("NWII_COPY_FILTER");
    return e && e[0] == '0';
  }();
  const uint32_t f0 = g_state.bp[0x53] & 0xFFFFFF, f1 = g_state.bp[0x54] & 0xFFFFFF;
  CopyFilter f;
  if (!f0 && !f1)
    return f;
  const uint32_t w0 = f0 & 63, w1 = (f0 >> 6) & 63, w2 = (f0 >> 12) & 63,
                 w3 = (f0 >> 18) & 63, w4 = f1 & 63, w5 = (f1 >> 6) & 63,
                 w6 = (f1 >> 12) & 63;
  f.u = (float)(w0 + w1) / 64.0f;
  f.m = (float)(w2 + w3 + w4) / 64.0f;
  f.l = (float)(w5 + w6) / 64.0f;
  if (disable) {
    f.m = f.u + f.m + f.l;
    f.u = f.l = 0.f;
  }
  f.on = !(f.u == 0.f && f.l == 0.f && f.m == 1.0f);
  return f;
}

// Filter uniforms of the copy program for source rect sr (scaled px) and BP
// 0x52 value val (bit 0 clamp top, bit 1 clamp bottom).
static void SetCopyFilterUniforms(const CopyFilter &cf, uint32_t val, const CopyRect &sr) {
  glUniform1i(s_cu_fon, cf.on ? 1 : 0);
  glUniform3f(s_cu_filter, cf.u, cf.m, cf.l);
  glUniform1i(s_cu_rowstep, s_scale);
  glUniform2i(s_cu_clampy, (val & 1) ? sr.y : 0,
              (val & 2) ? sr.y + sr.h - 1 : s_efb_h - 1);
}

static GLuint s_xfb_tmp_fbo = 0, s_xfb_tmp_tex = 0;
static int s_xfb_tmp_w = 0, s_xfb_tmp_h = 0;
uint64_t g_stat_filtered_copies = 0; // PEEK10: copies that ran the filter/gamma path

// EFB -> XFB. The XFB is texture-only (Dolphin's default "store XFB copies to
// texture only"): the source rect is scaled straight into the present
// framebuffer, so the XFB Y scale (BP 0x4E, BP 0x52 bit 10) and frame-to-field
// only change the RAM line count the VI would read and have no visible effect
// here. Gamma (BP 0x52 bits 7-8: 1.0, 1.7, 2.2) and a non-identity copy
// filter go through the copy shader (Dolphin EFBCopyFormat::XFB:
// pow(rgb, 1/gamma), alpha 1) into a temporary texture first; the common case
// (gamma 1.0, identity filter -- every SS display copy) stays a plain blit.
static void CopyToXfb(const CopyRect &r, uint32_t val) {
  EnsurePresentTarget();
  if (r.w <= 0 || r.h <= 0)
    return;
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  const CopyRect sr = Scaled(r);
  const CopyFilter cf = CurrentCopyFilter();
  const uint32_t gamma = (val >> 7) & 3;
  if ((cf.on || gamma == 1 || gamma == 2) && s_efb_color_tex && EnsureCopyProgram()) {
    static const float kGamma[4] = {1.0f, 1.7f, 2.2f, 1.0f};
    if (!s_xfb_tmp_fbo) {
      glGenTextures(1, &s_xfb_tmp_tex);
      glGenFramebuffers(1, &s_xfb_tmp_fbo);
    }
    glActiveTexture(GL_TEXTURE0 + kUploadUnit);
    glBindTexture(GL_TEXTURE_2D, s_xfb_tmp_tex);
    if (s_xfb_tmp_w != sr.w || s_xfb_tmp_h != sr.h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sr.w, sr.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      s_xfb_tmp_w = sr.w;
      s_xfb_tmp_h = sr.h;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_xfb_tmp_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           s_xfb_tmp_tex, 0);
    glDisable(GL_BLEND);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, sr.w, sr.h);
    glUseProgram(s_copy_prog);
    s_bound_prog = s_copy_prog;
    glActiveTexture(GL_TEXTURE0 + kCopyUnit);
    glBindTexture(GL_TEXTURE_2D, s_efb_color_tex);
    glBindSampler(kCopyUnit, 0);
    glUniform1i(s_cu_efb, kCopyUnit);
    glUniform1i(s_cu_zsrc, kCopyZUnit);
    glUniform2i(s_cu_rect, sr.x, sr.y);
    glUniform2i(s_cu_size, s_efb_w, s_efb_h);
    glUniform1i(s_cu_scale, 1);
    glUniform1i(s_cu_fmt, 16);
    glUniform1i(s_cu_int, 0);
    glUniform1i(s_cu_alpha, 1);
    glUniform1i(s_cu_depth, 0);
    glUniform1f(s_cu_gamma, 1.0f / kGamma[gamma]);
    SetCopyFilterUniforms(cf, val, sr);
    glBindVertexArray(s_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    // Texel row 0 is the top row of the rect: flip into the present FBO.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_xfb_tmp_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_present_fbo);
    glBlitFramebuffer(0, 0, sr.w, sr.h, 0, s_present_h, s_present_w, 0,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    InvalidateGLState();
    ++g_stat_filtered_copies;
    ++s_stats.xfb_copies;
    return;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_present_fbo);
  const int y0 = s_efb_h - sr.y - sr.h;
  glBlitFramebuffer(sr.x, y0, sr.x + sr.w, y0 + sr.h, 0, 0, s_present_w,
                    s_present_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  s_fixed_valid = false;
  ++s_stats.xfb_copies;
}

static void CopyToTexture(const CopyRect &r, uint32_t val) {
  const uint32_t dest = (g_state.bp[0x4B] & 0xFFFFFF) << 5;
  if (!dest || r.w <= 0 || r.h <= 0)
    return;
  const bool half = (val >> 9) & 1;
  const int dw = half ? std::max(1, r.w / 2) : r.w;
  const int dh = half ? std::max(1, r.h / 2) : r.h;
  if (dw > 1024 || dh > 1024)
    return;
  const uint32_t fmt = (val >> 3) & 0xF;
  const uint32_t realfmt = fmt / 2 + (fmt & 1) * 8;
  const bool intensity = ((val >> 15) & 1) && ((val >> 16) & 1);

  // RES9: the GL texture holds the copy at internal resolution (pw x ph);
  // dw x dh stays the GX size (RAM footprint, texture_data_size). Shaders
  // sample with coordinates normalised by the GX texture size (uTexDim).
  const int pw = dw * s_scale, ph = dh * s_scale;
  const CopyRect sr = Scaled(r);

  GLuint tex = InterpIsolated() ? InterpShadowFor(dest, pw, ph) // INTERP17: in-between frame copy
                             : nwii::runtime::hle::TextureCache::get().find_efb_texture(dest);
  if (!tex)
    glGenTextures(1, &tex);
  glActiveTexture(GL_TEXTURE0 + kUploadUnit);
  glBindTexture(GL_TEXTURE_2D, tex);
  auto sz = s_copy_tex_size.find(tex);
  if (!InterpIsolated() && // INTERP17: shadow textures are sized by InterpShadowFor
      (sz == s_copy_tex_size.end() || sz->second.first != pw || sz->second.second != ph)) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    s_copy_tex_size[tex] = {pw, ph};
  }

  if (s_efb_color_tex && EnsureCopyProgram()) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_copy_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, pw, ph);
    glUseProgram(s_copy_prog);
    s_bound_prog = s_copy_prog;
    glActiveTexture(GL_TEXTURE0 + kCopyUnit);
    glBindTexture(GL_TEXTURE_2D, s_efb_color_tex);
    glBindSampler(kCopyUnit, 0);
    glUniform1i(s_cu_efb, kCopyUnit);
    glUniform2i(s_cu_rect, sr.x, sr.y); // scaled: the 2x2 box stays 2x2 (RES9)
    glUniform2i(s_cu_size, s_efb_w, s_efb_h);
    glUniform1i(s_cu_scale, half ? 2 : 1);
    glUniform1i(s_cu_fmt, (GLint)realfmt);
    glUniform1i(s_cu_int, intensity ? 1 : 0);
    glUniform1i(s_cu_alpha, EfbHasAlpha() ? 1 : 0);
    const CopyFilter cf = CurrentCopyFilter();
    SetCopyFilterUniforms(cf, val, sr);
    if (cf.on)
      ++g_stat_filtered_copies;
    // Pixel format Z24 (BP 0x43) selects a depth copy (Dolphin BPStructs).
    const bool depth_copy = (g_state.bp[0x43] & 7) == 3 && s_efb_depth_tex != 0;
    glUniform1i(s_cu_depth, depth_copy ? 1 : 0);
    glActiveTexture(GL_TEXTURE0 + kCopyZUnit);
    glBindTexture(GL_TEXTURE_2D, depth_copy ? s_efb_depth_tex : 0);
    glBindSampler(kCopyZUnit, 0);
    glUniform1i(s_cu_zsrc, kCopyZUnit);
    glBindVertexArray(s_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0); // unbind the EFB depth from its unit
    glActiveTexture(GL_TEXTURE0 + kCopyUnit);
    glBindTexture(GL_TEXTURE_2D, 0); // unbind the EFB texture from the copy unit
    glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  } else {
    // No sampleable EFB colour texture: plain blit with a Y flip.
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (!s_copy_fbo)
      glGenFramebuffers(1, &s_copy_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_copy_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
    const int y0 = s_efb_h - sr.y - sr.h;
    glBlitFramebuffer(sr.x, y0, sr.x + sr.w, y0 + sr.h, 0, ph, pw, 0,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  }
  if (InterpIsolated()) { // INTERP17: no registration, RAM hash or overlap drop for in-between copies
    ++s_interp_stats.shadow_copies;
    InvalidateGLState();
    return;
  }
  if (g_rendererEfbCopyHook)
    g_rendererEfbCopyHook(dest);
  nwii::runtime::hle::TextureCache::get().register_efb_texture(dest, tex);
  {
    const uint32_t bytes = nwii::runtime::hle::texture_data_size(
        (uint32_t)dw, (uint32_t)dh, CopyTexFormat(realfmt));
    // A copy overlapping older copies' ranges overwrites them on hardware.
    std::vector<uint32_t> overlapped;
    for (const auto &kv : s_efb_info)
      if (kv.first != dest && kv.first < dest + bytes && dest < kv.first + kv.second.bytes)
        overlapped.push_back(kv.first);
    for (uint32_t o : overlapped)
      DropEfbCopy(o);
    s_efb_info[dest] = {bytes, RamHash(dest, bytes), tex};
    s_efb_checked[dest] = s_render_frame;
  }
  InvalidateGLState();
  ++s_stats.copies;
}

// ---------------------------------------------------------------------------
// PEEK10 (roadmap M3.6): CPU access to the EFB (GXPeekZ / GXPeekARGB, pokes).
// Dolphin: MMU EFB_Read/EFB_Write -> AccessEFB, which reads a native-
// resolution cache of the EFB (FramebufferManager::PopulateEFBCache,
// invalidated on draw) and converts it (24-bit Z, ARGB, alpha read mode).
//
// Here the renderer runs on the host thread about a frame behind the guest;
// making the guest wait for its own draws would serialise CPU and GPU. SS's
// only peek (d3d::fn_80017500, after GXDrawDone at the end of dSys::execute:
// 16 sun-flare points + Link's focus point) reads frame N's final EFB, and
// EGG Display::beginFrame copies exactly that EFB to the XFB (with clear) at
// the start of frame N+1. So while the guest peeks, the renderer snapshots
// the EFB right before every EFB->XFB copy: a native-size (640x528) image,
// the centre sample of each s x s block at internal scale s (RES9), depth as
// float (GL depth = GX Z / 2^24) plus RGBA8 colour when colour peeks occur,
// downloaded asynchronously (PBO + fence, polled every Render and before the
// next snapshot, never waited on). A peek returns the newest complete
// snapshot: 1-2 frames old; a new snapshot replaces the old one per frame
// (our form of Dolphin's "invalidate on draw"). Before the first snapshot a
// peek sees a cleared EFB (Z 0xFFFFFF, colour 0).
//   NWII_EFB_PEEK=0      peeks read 0 and pokes are dropped (old behaviour;
//                        = Dolphin "Skip EFB Access from CPU")
//   NWII_EFB_PEEK_LOG=N  log the first N peeks/pokes and every 300th snapshot
// ---------------------------------------------------------------------------
namespace {
const char *kSnapFS = R"(#version 330 core
uniform sampler2D uEfb;
uniform sampler2D uEfbZ;
uniform int uS;        // internal scale
uniform ivec2 uSize;   // scaled EFB size
layout(location = 0) out float oZ;
layout(location = 1) out vec4 oC;
void main() {
  ivec2 n = ivec2(gl_FragCoord.xy);            // native GL px, row 0 = bottom
  int x = min(n.x * uS + uS / 2, uSize.x - 1);
  int y = min(n.y * uS + (uS - 1 - uS / 2), uSize.y - 1); // GX row y*s + s/2
  oZ = texelFetch(uEfbZ, ivec2(x, y), 0).r;
  oC = texelFetch(uEfb, ivec2(x, y), 0);
}
)";
constexpr int kSnapTtl = 60; // XFB copies a snapshot request stays active

bool EfbPeekOff() {
  static const bool off = [] {
    const char *e = std::getenv("NWII_EFB_PEEK");
    return e && e[0] == '0';
  }();
  return off;
}
int EfbPeekLog() {
  static const int n = [] {
    const char *e = std::getenv("NWII_EFB_PEEK_LOG");
    return e ? std::max(1, std::atoi(e)) : 0;
  }();
  return n;
}

struct EfbPokeReq {
  bool depth;
  int x, y;
  uint32_t value;
};

// Shared with the CPU thread (s_efb_mtx).
std::mutex s_efb_mtx;
std::vector<float> s_pub_z;    // published snapshot: depth, GL rows (bottom first)
std::vector<uint8_t> s_pub_c;  // colour RGBA8, GL rows; empty = no colour yet
int s_pub_w = 0, s_pub_h = 0;
uint32_t s_pub_fmt = 0;        // BP 0x43 & 7 at snapshot time
std::atomic<uint32_t> s_pixfmt_now{0}; // BP 0x43 & 7 at the last XFB copy (render thread)
std::vector<EfbPokeReq> s_pokes;
std::atomic<uint32_t> s_peek_want{0}; // bit 0 depth, bit 1 colour
std::atomic<bool> s_poke_pending{false};
std::atomic<uint64_t> s_cnt[8];       // see EfbAccessStats

// Render thread only.
int s_snap_ttl_z = 0, s_snap_ttl_c = 0;
GLuint s_snap_fbo = 0, s_snap_ztex = 0, s_snap_ctex = 0, s_snap_prog = 0;
GLuint s_snap_pbo[2] = {0, 0};
GLint s_su_efb = -1, s_su_z = -1, s_su_s = -1, s_su_size = -1;
GLsync s_snap_fence = nullptr;
int s_snap_w = 0, s_snap_h = 0;        // allocated size
int s_pend_w = 0, s_pend_h = 0;        // in-flight download
bool s_pend_c = false;
uint32_t s_pend_fmt = 0;
} // namespace

EfbAccessStats GetEfbAccessStats() {
  EfbAccessStats s;
  s.peeks_z = s_cnt[0].load();
  s.peeks_c = s_cnt[1].load();
  s.peeks_before_snapshot = s_cnt[2].load();
  s.pokes = s_cnt[3].load();
  s.snapshots = s_cnt[4].load();
  s.published = s_cnt[5].load();
  s.busy_skips = s_cnt[6].load();
  return s;
}

// CPU thread. Returns false before the first published snapshot.
bool EfbPeek(bool depth, int x, int y, float *z, uint8_t rgba[4], uint32_t *pixel_format) {
  s_peek_want.fetch_or(depth ? 1u : 2u, std::memory_order_relaxed);
  ++s_cnt[depth ? 0 : 1];
  std::lock_guard<std::mutex> lk(s_efb_mtx);
  *pixel_format = s_pub_w ? s_pub_fmt : s_pixfmt_now.load(std::memory_order_relaxed);
  if (!s_pub_w || (!depth && s_pub_c.empty())) {
    ++s_cnt[2];
    return false;
  }
  x = std::clamp(x, 0, s_pub_w - 1);
  y = std::clamp(y, 0, s_pub_h - 1);
  const size_t i = (size_t)(s_pub_h - 1 - y) * s_pub_w + x;
  if (depth)
    *z = s_pub_z[i];
  else
    std::memcpy(rgba, &s_pub_c[i * 4], 4);
  return true;
}

// CPU thread. The poke is applied to the GL EFB on the render thread at its
// next Render (stream order is not kept: SS never pokes) and patched into the
// published snapshot so later peeks see it.
void EfbPoke(bool depth, int x, int y, uint32_t value) {
  ++s_cnt[3];
  std::lock_guard<std::mutex> lk(s_efb_mtx);
  s_pokes.push_back({depth, x, y, value});
  s_poke_pending.store(true, std::memory_order_release);
  if (s_pub_w && x >= 0 && y >= 0 && x < s_pub_w && y < s_pub_h) {
    const size_t i = (size_t)(s_pub_h - 1 - y) * s_pub_w + x;
    if (depth)
      s_pub_z[i] = (float)(value & 0xFFFFFF) / 16777216.0f;
    else if (!s_pub_c.empty()) {
      uint8_t *p = &s_pub_c[i * 4]; // ARGB -> RGBA
      p[0] = (value >> 16) & 0xFF; p[1] = (value >> 8) & 0xFF;
      p[2] = value & 0xFF; p[3] = value >> 24;
    }
  }
}

// Render thread: apply queued pokes as scissored clears of the s x s block.
static void EfbApplyPokes() {
  if (!s_poke_pending.exchange(false, std::memory_order_acquire))
    return;
  std::vector<EfbPokeReq> q;
  {
    std::lock_guard<std::mutex> lk(s_efb_mtx);
    q.swap(s_pokes);
  }
  if (q.empty() || !s_efb_fbo)
    return;
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  glEnable(GL_SCISSOR_TEST);
  for (const auto &p : q) {
    if (p.x < 0 || p.y < 0 || p.x >= s_efb_lw || p.y >= s_efb_lh)
      continue;
    glScissor(p.x * s_scale, s_efb_h - (p.y + 1) * s_scale, s_scale, s_scale);
    if (p.depth) {
      glDepthMask(GL_TRUE);
      glClearDepth((p.value & 0xFFFFFF) / 16777216.0);
      glClear(GL_DEPTH_BUFFER_BIT);
    } else {
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearColor(((p.value >> 16) & 0xFF) / 255.f, ((p.value >> 8) & 0xFF) / 255.f,
                   (p.value & 0xFF) / 255.f, (p.value >> 24) / 255.f);
      glClear(GL_COLOR_BUFFER_BIT);
    }
  }
  InvalidateGLState();
}

// Render thread: publish a finished download (never blocks unless wait).
static void EfbSnapshotPoll(bool wait) {
  if (!s_snap_fence)
    return;
  const GLenum r = glClientWaitSync(s_snap_fence, wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
                                    wait ? 100000000ull : 0);
  if (r == GL_TIMEOUT_EXPIRED)
    return;
  glDeleteSync(s_snap_fence);
  s_snap_fence = nullptr;
  if (r == GL_WAIT_FAILED)
    return;
  const size_t n = (size_t)s_pend_w * s_pend_h;
  GLint prev = 0;
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev);
  {
    std::lock_guard<std::mutex> lk(s_efb_mtx);
    bool ok = false;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_snap_pbo[0]);
    if (const void *p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)(n * 4),
                                         GL_MAP_READ_BIT)) {
      s_pub_z.resize(n);
      std::memcpy(s_pub_z.data(), p, n * 4);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      ok = true;
    }
    if (ok && s_pend_c) {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, s_snap_pbo[1]);
      if (const void *p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)(n * 4),
                                           GL_MAP_READ_BIT)) {
        s_pub_c.resize(n * 4);
        std::memcpy(s_pub_c.data(), p, n * 4);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      }
    } else if (ok && (size_t)s_pub_w * s_pub_h != n) {
      s_pub_c.clear(); // stale colour of another size
    }
    if (ok) {
      s_pub_w = s_pend_w;
      s_pub_h = s_pend_h;
      s_pub_fmt = s_pend_fmt;
      ++s_cnt[5];
    }
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev);
}

static bool EnsureSnapResources(int w, int h) {
  if (!EnsureCopyProgram()) // also creates s_empty_vao
    return false;
  if (!s_snap_prog) {
    s_snap_prog = CompileGLShaderProgram(kCopyVS, kSnapFS);
    if (!s_snap_prog)
      return false;
    s_su_efb = glGetUniformLocation(s_snap_prog, "uEfb");
    s_su_z = glGetUniformLocation(s_snap_prog, "uEfbZ");
    s_su_s = glGetUniformLocation(s_snap_prog, "uS");
    s_su_size = glGetUniformLocation(s_snap_prog, "uSize");
    glGenFramebuffers(1, &s_snap_fbo);
    glGenTextures(1, &s_snap_ztex);
    glGenTextures(1, &s_snap_ctex);
    glGenBuffers(2, s_snap_pbo);
  }
  if (s_snap_w == w && s_snap_h == h)
    return true;
  glActiveTexture(GL_TEXTURE0 + kUploadUnit);
  glBindTexture(GL_TEXTURE_2D, s_snap_ztex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, s_snap_ctex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_snap_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         s_snap_ztex, 0);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                         s_snap_ctex, 0);
  const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
  glDrawBuffers(2, bufs);
  const bool complete =
      glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  for (int i = 0; i < 2; ++i) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_snap_pbo[i]);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, nullptr, GL_STREAM_READ);
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  if (!complete) {
    std::printf("[PEEK10] EFB snapshot FBO incomplete; peeks stay unavailable\n");
    return false;
  }
  s_snap_w = w;
  s_snap_h = h;
  return true;
}

// Render thread, at an EFB->XFB copy before its clear.
static void EfbSnapshotAtFrameEnd() {
  s_pixfmt_now = g_state.bp[0x43] & 7;
  const uint32_t want = s_peek_want.exchange(0, std::memory_order_relaxed);
  if (want & 1)
    s_snap_ttl_z = kSnapTtl;
  if (want & 2)
    s_snap_ttl_c = kSnapTtl;
  if (s_snap_ttl_z <= 0 && s_snap_ttl_c <= 0)
    return;
  const bool colour = s_snap_ttl_c > 0;
  if (s_snap_ttl_z > 0)
    --s_snap_ttl_z;
  if (s_snap_ttl_c > 0)
    --s_snap_ttl_c;
  EfbSnapshotPoll(false);
  if (s_snap_fence) { // the previous download is still in flight
    ++s_cnt[6];
    return;
  }
  if (!s_efb_color_tex || !s_efb_depth_tex || !EnsureSnapResources(s_efb_lw, s_efb_lh))
    return;
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_snap_fbo);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_COLOR_LOGIC_OP);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, s_efb_lw, s_efb_lh);
  glUseProgram(s_snap_prog);
  s_bound_prog = s_snap_prog;
  glActiveTexture(GL_TEXTURE0 + kCopyUnit);
  glBindTexture(GL_TEXTURE_2D, s_efb_color_tex);
  glBindSampler(kCopyUnit, 0);
  glActiveTexture(GL_TEXTURE0 + kCopyZUnit);
  glBindTexture(GL_TEXTURE_2D, s_efb_depth_tex);
  glBindSampler(kCopyZUnit, 0);
  glUniform1i(s_su_efb, kCopyUnit);
  glUniform1i(s_su_z, kCopyZUnit);
  glUniform1i(s_su_s, s_scale);
  glUniform2i(s_su_size, s_efb_w, s_efb_h);
  glBindVertexArray(s_empty_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0 + kCopyUnit);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_snap_fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, s_snap_pbo[0]);
  glReadPixels(0, 0, s_efb_lw, s_efb_lh, GL_RED, GL_FLOAT, nullptr);
  if (colour) {
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_snap_pbo[1]);
    glReadPixels(0, 0, s_efb_lw, s_efb_lh, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  s_snap_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  s_pend_w = s_efb_lw;
  s_pend_h = s_efb_lh;
  s_pend_c = colour;
  s_pend_fmt = s_pixfmt_now.load(std::memory_order_relaxed);
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  InvalidateGLState();
  const uint64_t n = ++s_cnt[4];
  if (EfbPeekLog() && (n <= 3 || n % 300 == 0)) {
    const EfbAccessStats st = GetEfbAccessStats();
    std::printf("[PEEK10] snapshot #%llu %dx%d s=%d colour=%d fmt=%u peeks_z=%llu "
                "peeks_c=%llu early=%llu published=%llu busy=%llu\n",
                (unsigned long long)n, s_efb_lw, s_efb_lh, s_scale, colour ? 1 : 0,
                s_pixfmt_now.load(), (unsigned long long)st.peeks_z,
                (unsigned long long)st.peeks_c,
                (unsigned long long)st.peeks_before_snapshot,
                (unsigned long long)st.published, (unsigned long long)st.busy_skips);
  }
}

void EfbAccessPoll(bool wait) {
  if (s_snap_fence)
    EfbSnapshotPoll(wait);
}

static void ExecuteCopy() {
  FlushBatch();
  EfbQuantFlush(); // LIGHT17: the copy reads stored (format-precision) values
  const uint32_t val = g_state.bp[0x52];
  const CopyRect r = CopySourceRect();
  if (val & 0x4000) {
    if (!EfbPeekOff() && !s_interp_hook) // INTERP17: no peek snapshot of an in-between frame
      EfbSnapshotAtFrameEnd();
    CopyToXfb(r, val);
  } else {
    CopyToTexture(r, val);
  }
  if (val & 0x800)
    ClearScreen(r);
  g_state.pe_clear_pending = false;
  s_dirty |= D_ALL;
}

// ---------------------------------------------------------------------------
// THP diagnostics (NWII_THP_DRAW_TRACE)
// ---------------------------------------------------------------------------
static bool TraceTHPDraw() {
  static const bool enabled = std::getenv("NWII_THP_DRAW_TRACE") != nullptr;
  if (!enabled || g_state.numTevStages != 4 ||
      g_state.texStages[0].format != 1 || g_state.texStages[0].width != 608 ||
      g_state.texStages[0].height != 464 || g_state.texStages[1].format != 1 ||
      g_state.texStages[1].width != 304 || g_state.texStages[1].height != 232 ||
      g_state.texStages[2].format != 1 || g_state.texStages[2].width != 304 ||
      g_state.texStages[2].height != 232)
    return false;
  static unsigned draws = 0;
  ++draws;
  return draws <= 3 || draws % 120 == 0;
}

static void LogTHPDraw(const GXCommand &cmd) {
  std::ostringstream out;
  GLint program = 0, viewport[4]{};
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetIntegerv(GL_VIEWPORT, viewport);
  out << "[THP draw] program=" << program << " skip=" << s_skip_draw
      << " verts=" << cmd.vertices.size() << " batch=" << s_batch.size() / BatchFloats(s_batch_k)
      << " projection=" << g_state.projType
      << " bp41=0x" << std::hex << g_state.bp[0x41] << std::dec
      << " viewport=" << viewport[0] << ',' << viewport[1] << ','
      << viewport[2] << ',' << viewport[3] << '\n';
  for (int i = 0; i < 3; ++i) {
    const auto &stage = g_state.texStages[i];
    out << "[THP tex] unit=" << i << " addr=0x" << std::hex << stage.base_addr
        << std::dec << " boundGL=" << s_th[i].tex << " valid=" << s_tex_valid[i]
        << " efb="
        << nwii::runtime::hle::TextureCache::get().find_efb_texture(stage.base_addr)
        << '\n';
  }
  ComputeProj(s_proj);
  out << "[THP matrix]";
  for (float v : s_proj)
    out << ' ' << v;
  out << '\n';
  const auto text = out.str();
  std::fwrite(text.data(), 1, text.size(), stdout);
}

// ---------------------------------------------------------------------------
// M0.4 tooling hooks (installed by gx.cpp; null = off)
// ---------------------------------------------------------------------------
FILE *(*g_rendererFrameLogHook)(uint64_t *frame) = nullptr;
void (*g_rendererEfbCopyHook)(uint32_t dest) = nullptr;
void RendererSetCompileBudget(int n) {
  s_compile_budget = n;
  s_compile_time_unlimited = n >= (1 << 20);
}
uint64_t RendererSkippedDraws() { return s_skipped_draws; }

// NWII_GXFRAMELOG: one line per draw, in render order, with the state the
// draw is rendered with (after all preceding BP/XF writes were applied).
static void FrameLogDraw(FILE *f, uint64_t frame, unsigned ev, unsigned ci,
                         const GXCommand &cmd) {
  const size_t nv = cmd.vertices.size();
  std::fprintf(
      f,
      "f=%llu e=%u c=%u DRAW prim=0x%02X nv=%zu proj=%u projSet=%d skip=%d "
      "tev=%u ntg=%u nch=%u cull=%u gen=%06X z=%06X cm0=%06X cm1=%06X "
      "pe43=%06X vp=%g,%g,%g,%g,%g,%g sc=%06X,%06X,%06X",
      (unsigned long long)frame, ev, ci, cmd.prim_type, nv,
      g_state.projType, (int)g_state.projSet, (nv && s_skip_draw) ? 1 : 0,
      g_state.numTevStages, g_state.numTexGens, g_state.numChans,
      g_state.cullMode, g_state.bp[0x00], g_state.bp[0x40], g_state.bp[0x41],
      g_state.bp[0x42], g_state.bp[0x43], XfAsFloat(0x1A), XfAsFloat(0x1B),
      XfAsFloat(0x1C), XfAsFloat(0x1D), XfAsFloat(0x1E), XfAsFloat(0x1F),
      g_state.bp[0x20], g_state.bp[0x21], g_state.bp[0x59]);
  // '*' marks texture maps referenced by an active TEV stage.
  unsigned used = 0;
  for (unsigned s = 0; s < g_state.numTevStages && s < 16; ++s)
    if (g_state.tevStages[s].texMap < 8)
      used |= 1u << g_state.tevStages[s].texMap;
  for (int i = 0; i < 8; ++i) {
    const auto &t = g_state.texStages[i];
    if (!t.base_addr)
      continue;
    const bool efb =
        nwii::runtime::hle::TextureCache::get().find_efb_texture(t.base_addr) != 0;
    std::fprintf(f, " t%d%s=%08X/f%u/%ux%u%s", i, (used >> i) & 1 ? "*" : "",
                 t.base_addr, t.format, t.width, t.height, efb ? "/efb" : "");
  }
  std::fputc('\n', f);
}

// NWII_GXFRAMELOG: one line per BP 0x52 (PE copy execute). Field layout per
// Dolphin BPMemory UPE_Copy / BlendMode / ZMode.
static void FrameLogCopy(FILE *f, uint64_t frame, unsigned ev, unsigned ci,
                         uint32_t val) {
  const uint32_t src = g_state.bp[0x49], wh = g_state.bp[0x4A];
  const uint32_t fmt = (val >> 3) & 0xF;
  const uint32_t cm0 = g_state.bp[0x41];
  std::fprintf(
      f,
      "f=%llu e=%u c=%u COPY val=%06X src=%u,%u,%ux%u dest=%08X stride=%u "
      "fmt=%u realfmt=%u intensity=%u half=%u clear=%u xfb=%u gamma=%u "
      "clampT=%u clampB=%u cupd=%u aupd=%u zupd=%u z=%06X cm0=%06X cm1=%06X "
      "clearAR=%04X clearGB=%04X clearZ=%06X pe43=%06X",
      (unsigned long long)frame, ev, ci, val, src & 0x3FF,
      (src >> 10) & 0x3FF, (wh & 0x3FF) + 1, ((wh >> 10) & 0x3FF) + 1,
      (g_state.bp[0x4B] & 0xFFFFFF) << 5, (g_state.bp[0x4D] & 0x3FF) << 5, fmt,
      fmt / 2 + (fmt & 1) * 8, (val >> 15) & 1, (val >> 9) & 1, (val >> 11) & 1,
      (val >> 14) & 1, (val >> 7) & 3, val & 1, (val >> 1) & 1, (cm0 >> 3) & 1,
      (cm0 >> 4) & 1, (g_state.bp[0x40] >> 4) & 1, g_state.bp[0x40], cm0,
      g_state.bp[0x42], g_state.bp[0x4F] & 0xFFFF, g_state.bp[0x50] & 0xFFFF,
      g_state.bp[0x51] & 0xFFFFFF, g_state.bp[0x43]);
  // PEEK10: the vertical copy filter, only when it is not the identity (so
  // logs of identity-filter copies keep their old form), as the 64ths of the
  // upper / middle / lower rows (register values, NWII_COPY_FILTER ignored).
  const uint32_t f0 = g_state.bp[0x53] & 0xFFFFFF, f1 = g_state.bp[0x54] & 0xFFFFFF;
  const uint32_t up = (f0 & 63) + ((f0 >> 6) & 63);
  const uint32_t mid = ((f0 >> 12) & 63) + ((f0 >> 18) & 63) + (f1 & 63);
  const uint32_t lo = ((f1 >> 6) & 63) + ((f1 >> 12) & 63);
  if ((f0 || f1) && (up || lo || mid != 64))
    std::fprintf(f, " vfilter=%u/%u/%u", up, mid, lo);
  std::fputc('\n', f);
}

// ---------------------------------------------------------------------------
// Diagnostics (gxreplay; need the frame log for event numbers). Off unless set.
//   NWII_GXSNAP=<frame>:<ev>[,<ev>...]  write the raw EFB (640x528, GL rows)
//                                       after those frame-log events to
//                                       $NWII_GXSNAP_DIR/snap_<frame>_<ev>.bmp
//   NWII_GXSKIP=<a>-<b>[,<a>-<b>...]    do not draw draw-events a..b (any frame)
//   NWII_GXDUMPDRAW=<frame>:<ev>[,...]  print TEV/BP/XF state + fragment shader
//                                       of those draw events
// ---------------------------------------------------------------------------
struct DiagCfg {
  uint64_t snap_frame = 0;
  std::vector<unsigned> snap_ev;
  std::vector<std::pair<unsigned, unsigned>> skip;
  uint64_t dump_frame = 0;
  std::vector<unsigned> dump_ev;
  std::string dir = ".";
};
static const DiagCfg &Diag() {
  static const DiagCfg c = [] {
    DiagCfg d;
    if (const char *e = std::getenv("NWII_GXSNAP"); e && *e) {
      d.snap_frame = std::strtoull(e, nullptr, 10);
      if (const char *p = std::strchr(e, ':'))
        for (++p; *p;) {
          char *end = nullptr;
          d.snap_ev.push_back((unsigned)std::strtoul(p, &end, 10));
          if (!end || end == p) break;
          p = *end == ',' ? end + 1 : end;
        }
    }
    if (const char *e = std::getenv("NWII_GXDUMPDRAW"); e && *e) {
      d.dump_frame = std::strtoull(e, nullptr, 10);
      if (const char *p = std::strchr(e, ':'))
        for (++p; *p;) {
          char *end = nullptr;
          d.dump_ev.push_back((unsigned)std::strtoul(p, &end, 10));
          if (!end || end == p) break;
          p = *end == ',' ? end + 1 : end;
        }
    }
    if (const char *e = std::getenv("NWII_GXSNAP_DIR"); e && *e)
      d.dir = e;
    if (const char *e = std::getenv("NWII_GXSKIP"); e && *e)
      for (const char *p = e; *p;) {
        char *end = nullptr;
        const unsigned a = (unsigned)std::strtoul(p, &end, 10);
        unsigned b = a;
        if (end && *end == '-') b = (unsigned)std::strtoul(end + 1, &end, 10);
        d.skip.push_back({a, b});
        if (!end || !*end) break;
        p = end + 1;
      }
    return d;
  }();
  return c;
}
static bool DiagSkip(unsigned ev) {
  for (auto &r : Diag().skip)
    if (ev >= r.first && ev <= r.second)
      return true;
  return false;
}
static void DiagDump(uint64_t frame, unsigned ev, const GXCommand *cmd = nullptr) {
  const DiagCfg &d = Diag();
  if (d.dump_ev.empty() || frame != d.dump_frame ||
      std::find(d.dump_ev.begin(), d.dump_ev.end(), ev) == d.dump_ev.end())
    return;
  std::printf("[GXDUMP] f=%llu e=%u gen=%06X cm0=%06X cm1=%06X pe43=%06X z=%06X "
              "ind: 27=%06X 10..13=%06X %06X %06X %06X 25=%06X 26=%06X\n",
              (unsigned long long)frame, ev, g_state.bp[0x00], g_state.bp[0x41],
              g_state.bp[0x42], g_state.bp[0x43], g_state.bp[0x40], g_state.bp[0x27],
              g_state.bp[0x10], g_state.bp[0x11], g_state.bp[0x12], g_state.bp[0x13],
              g_state.bp[0x25], g_state.bp[0x26]);
  std::printf("[GXDUMP] indmtx 06-0E:");
  for (int i = 0x06; i <= 0x0E; ++i)
    std::printf(" %06X", g_state.bp[i]);
  std::printf("\n");
  std::printf("[GXDUMP] proj type=%u set=%d p=%g %g %g %g %g %g\n", g_state.projType,
              (int)g_state.projSet, g_state.projection[0], g_state.projection[1],
              g_state.projection[2], g_state.projection[3], g_state.projection[4],
              g_state.projection[5]);
  std::printf("[GXDUMP] tevreg (C0-C3 / K0-K3):");
  for (int i = 0; i < 8; ++i)
    std::printf(" %06X/%06X", g_state.tevColorReg[i], g_state.tevKonstReg[i]);
  std::printf("\n[GXDUMP] ksel F6-FD:");
  for (int i = 0xF6; i <= 0xFD; ++i)
    std::printf(" %06X", g_state.bp[i]);
  std::printf("\n[GXDUMP] xf chan 0A-11:");
  for (int i = 0x0A; i <= 0x11; ++i)
    std::printf(" %08X", g_state.xf[i]);
  std::printf(" texgen 40-47:");
  for (int i = 0x40; i <= 0x47; ++i)
    std::printf(" %08X", g_state.xf[i]);
  std::printf("\n[GXDUMP] tev env C0-CF:");
  for (int i = 0xC0; i <= 0xCF; ++i)
    std::printf(" %06X", g_state.bp[i]);
  std::printf(" tref 28-2B:");
  for (int i = 0x28; i <= 0x2B; ++i)
    std::printf(" %06X", g_state.bp[i]);
  std::printf("\n[GXDUMP] xf matidx 18/19=%08X/%08X numtexgen=%u dualtex=%u post 50-57:",
              g_state.xf[0x18], g_state.xf[0x19], g_state.xf[0x3F], g_state.xf[0x12]);
  for (int i = 0x50; i <= 0x57; ++i)
    std::printf(" %08X", g_state.xf[i]);
  std::printf(" bp texcoord scale 30-3F:");
  for (int i = 0x30; i <= 0x3F; ++i)
    std::printf(" %06X", g_state.bp[i]);
  std::printf("\n");
  // M1.8: the texgen inputs/outputs of the draw's vertices and the matrix rows.
  if (cmd && cmd->raw) {
    const auto &rw = *cmd->raw;
    std::printf("[GXDUMP] vcd pnmtxidx=%d texmtxidx=", rw.vat.posMatIdx ? 1 : 0);
    for (int i = 0; i < 8; ++i)
      std::printf("%d", rw.vat.texMatIdx[i] ? 1 : 0);
    std::printf(" default pos=%u tex=%u,%u,%u,%u,%u,%u,%u,%u\n", rw.defPosMtxIdx,
                rw.defTexMtxIdx[0], rw.defTexMtxIdx[1], rw.defTexMtxIdx[2], rw.defTexMtxIdx[3],
                rw.defTexMtxIdx[4], rw.defTexMtxIdx[5], rw.defTexMtxIdx[6], rw.defTexMtxIdx[7]);
  }
  if (cmd) {
    const size_t nv = std::min<size_t>(cmd->vertices.size(), 4);
    for (size_t v = 0; v < nv; ++v) {
      const VertexData &vd = cmd->vertices[v];
      float tc[8][3];
      TexGen(MakeTexGenSetup(), vd, tc, 8);
      std::printf("[GXDUMP] v%zu pos=%g,%g,%g raw=%g,%g,%g mtx=%u texmtx=", v, vd.pos[0],
                  vd.pos[1], vd.pos[2], vd.raw_pos[0], vd.raw_pos[1], vd.raw_pos[2],
                  vd.posMtxIdx);
      for (int i = 0; i < 8; ++i)
        std::printf("%u%s", vd.texMtxIdx[i], i < 7 ? "," : "");
      for (int i = 0; i < 8; ++i)
        if (vd.has_tex[i])
          std::printf(" in%d=%g,%g", i, vd.tex[i][0], vd.tex[i][1]);
      for (int i = 0; i < 4; ++i)
        std::printf(" tc%d=%g,%g,%g", i, tc[i][0], tc[i][1], tc[i][2]);
      std::printf("\n");
    }
    if (!cmd->vertices.empty()) {
      const VertexData &vd = cmd->vertices[0];
      for (uint32_t i = 0; i < std::min<uint32_t>(g_state.xf[0x3F] & 0xF, 8); ++i) {
        const uint32_t ti = vd.texMtxIdx[i] & 0x3F;
        const float *m = &g_state.posMatrices[ti * 4];
        std::printf("[GXDUMP] texmtx%u row%u: %g %g %g %g | %g %g %g %g | %g %g %g %g\n", i, ti,
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                    ti * 4 + 12 <= 256 ? m[8] : 0.f, ti * 4 + 12 <= 256 ? m[9] : 0.f,
                    ti * 4 + 12 <= 256 ? m[10] : 0.f, ti * 4 + 12 <= 256 ? m[11] : 0.f);
      }
    }
  }
  for (int i = 0; i < 8; ++i) {
    const auto &t = g_state.texStages[i];
    if (t.base_addr)
      std::printf("[GXDUMP] tex%d %08X f%u %ux%u mode0=%06X efb=%u\n", i, t.base_addr,
                  t.format, t.width, t.height, TexMode0(i),
                  nwii::runtime::hle::TextureCache::get().find_efb_texture(t.base_addr));
  }
  std::printf("[GXDUMP] FS:\n%s\n", GenerateTEVShader(g_state, 0).fragment_source.c_str());
  // The decoded textures the TEV samples, as GL holds them (texel row 0 at
  // the bottom of the BMP): $NWII_GXSNAP_DIR/dump_<frame>_<ev>_t<map>.bmp.
  uint8_t used = 0;
  ShaderUid(&used);
  for (int i = 0; i < 8; ++i) {
    if (!((used >> i) & 1))
      continue;
    uint32_t lvd = 1;
    const GLuint t = LookupTexture(g_state.texStages[i],
                                   TexLevels(g_state.texStages[i], TexMode0(i), TexMode1(i)), &lvd,
                                   ((TexMode0(i) >> 5) & 3) != 0); // HDTEX16
    if (!t)
      continue;
    glActiveTexture(GL_TEXTURE0 + kUploadUnit);
    glBindTexture(GL_TEXTURE_2D, t);
    GLint tw = 0, th = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    if (tw <= 0 || th <= 0 || tw > 1024 || th > 1024)
      continue;
    std::vector<uint8_t> px((size_t)tw * th * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    char name[96];
    std::snprintf(name, sizeof name, "/dump_%06llu_%06u_t%d", (unsigned long long)frame, ev, i);
    SaveBMP32(d.dir + name + ".bmp", px.data(), tw, th, 0);
    SaveBMP32(d.dir + name + "_a.bmp", px.data(), tw, th, 1);
  }
  s_tex_valid[0] = false; // unit binding state is unknown now
  for (bool &v : s_tex_valid)
    v = false;
}

static void DiagSnap(uint64_t frame, unsigned ev) {
  const DiagCfg &d = Diag();
  if (d.snap_ev.empty() || frame != d.snap_frame ||
      std::find(d.snap_ev.begin(), d.snap_ev.end(), ev) == d.snap_ev.end())
    return;
  FlushBatch();
  std::vector<uint8_t> px((size_t)s_efb_w * s_efb_h * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, s_efb_w, s_efb_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  EfbQuantCpu(px.data(), s_efb_w, s_efb_h); // LIGHT17: stored values (pending re-quantisation)
  char name[64];
  std::snprintf(name, sizeof name, "/snap_%06llu_%06u.bmp", (unsigned long long)frame, ev);
  SaveBMP32(d.dir + name, px.data(), s_efb_w, s_efb_h, 0);
  SaveBMP32(d.dir + std::string(name).replace(std::string(name).size() - 4, 4, "_a.bmp"),
            px.data(), s_efb_w, s_efb_h, 1);
}

// PERF21: the walk reads each draw's decoded vertices (written by the decode pool on other cores) and
// each XF payload (written by the parse worker) once, in stream order; fetching them a few commands
// ahead hides the cross-core cache misses that otherwise stall every draw. No effect on any value.
static inline void PrefetchCmd(const GXCommand &c) {
#ifdef PERF21_PREFETCH
  if (c.type == GXCommandType::DrawPrimitive) {
    const char *p = (const char *)c.vertices.data();
    const size_t bytes = std::min<size_t>(c.vertices.size() * sizeof(VertexData), 4096);
    for (size_t o = 0; o < bytes; o += 64)
      _mm_prefetch(p + o, _MM_HINT_T0);
  } else if (c.type == GXCommandType::XFRegister && !c.payload.empty()) {
    _mm_prefetch((const char *)c.payload.data(), _MM_HINT_T0);
  }
#else
  (void)c;
#endif
}
static constexpr size_t kPrefetchAhead = 24;

class RendererGL : public IRenderer {
public:
  RendererGL() = default;
  ~RendererGL() override = default;

  void Initialize(void *window) override { m_window = window; }

  void Render(const std::vector<GXCommand> &commands) override {
    QueryEfbTarget();
    InterpDrawHook *const ih = s_interp_hook; // INTERP17: in-between frame (RenderInterpFrame)
    if (!ih) {
    ++s_render_frame;
    EfbSnapshotPoll(false); // PEEK10: publish a finished EFB download
    EfbApplyPokes();
    nwii::runtime::hle::TextureCacheFrameBegin();
    s_compiles_this_frame = 0;
    s_compile_us_this_frame = 0;
    MvFrameStart(); // MVGEN21: armed: attach + clear this frame's motion texture, start recording
    }
    s_tex_memo.clear();
    InvalidateGLState();

    uint64_t flog_frame = 0;
    FILE *flog = (!ih && g_rendererFrameLogHook) ? g_rendererFrameLogHook(&flog_frame) : nullptr;
    unsigned flog_ev = 0, flog_ci = 0;
    for (const auto &cmd : commands) {
      const unsigned ci = flog_ci++;
      if (ih && ci < s_interp_begin) // MVGEN21: the warp frame's tail replay starts at the 2D boundary
        continue;
      if (!ih && s_mv.recording && ci == s_mv.arm.boundary) // MVGEN21: boundary snapshot, motion ends
        MvBoundary();
      static const bool s_pf = !std::getenv("NWII_PERF21_NOPF"); // PERF21: A/B switch
      if (s_pf && ci + kPrefetchAhead < commands.size()) // PERF21
        PrefetchCmd(commands[ci + kPrefetchAhead]);
      if (cmd.type == GXCommandType::XFRegister) {
        const int addr = cmd.reg;
        // PERF21: matrix loads (about one per draw) straight into the matrix memory: the same
        // values the per-word loop below stores, and no dirty bits (DirtyForXF of these ranges is 0).
        static const bool s_xff = !std::getenv("NWII_PERF21_NOXF"); // PERF21: A/B switch
        if (s_xff) {
          const int cnt = (int)cmd.payload.size();
          if (addr + cnt <= 0x100) {
            if (cnt > 0)
              std::memcpy(&g_state.posMatrices[addr], cmd.payload.data(), (size_t)cnt * 4);
            continue;
          }
          if (addr >= 0x400 && addr + cnt <= 0x460) {
            std::memcpy(&g_state.normalMatrices[addr - 0x400], cmd.payload.data(), (size_t)cnt * 4);
            continue;
          }
        }
        bool touched_proj = false;
        for (size_t i = 0; i < cmd.payload.size(); ++i) {
          const int a = addr + (int)i;
          if (a >= 0x1000 && a < 0x1000 + 256)
            std::memcpy(&g_state.xf[a - 0x1000], &cmd.payload[i], 4);
          if (a >= 0x0000 && a <= 0x00FF) {
            g_state.posMatrices[a] = cmd.payload[i];
          } else if (a >= 0x0400 && a <= 0x045F) {
            g_state.normalMatrices[a - 0x0400] = cmd.payload[i];
          } else if (a >= 0x0500 && a <= 0x05FF) {
            s_post_mtx[a - 0x0500] = cmd.payload[i]; // dual-texture post matrices (M1.8)
          } else if (a >= 0x0600 && a <= 0x067F) {
            const int li = (a - 0x0600) / 16;
            g_state.lights[li][(a - 0x0600) % 16] = cmd.payload[i];
          } else if (a >= 0x1020 && a <= 0x1026) {
            g_state.projection[a - 0x1020] = cmd.payload[i];
            if (a == 0x1026)
              std::memcpy(&g_state.projType, &cmd.payload[i], 4);
            touched_proj = true;
          }
        }
        if (touched_proj)
          g_state.projSet = true;
        s_dirty |= DirtyForXF(addr, (int)cmd.payload.size());
      } else if (cmd.type == GXCommandType::BPRegister) {
        const uint8_t reg = (uint8_t)cmd.reg;
        ApplyBPRegister(reg, cmd.val);
        s_dirty |= DirtyForBP(reg);
        if (reg == 0x65)
          s_tex_memo.clear(); // TLUT memory changed
        if (reg == 0x52) {
          ExecuteCopy();
          if (flog) {
            FrameLogCopy(flog, flog_frame, flog_ev, ci, g_state.bp[0x52]);
            DiagSnap(flog_frame, flog_ev++);
          }
        }
      } else if (cmd.type == GXCommandType::DrawPrimitive) {
        if (ih) { // INTERP17: per-draw interpolation overrides around the normal draw path
          if (ih->Pre(ci, cmd))
            DrawPrimitive(cmd);
          ih->Post(ci, cmd);
          continue;
        }
        if (flog)
          DiagDump(flog_frame, flog_ev, &cmd);
        if (!(flog && !Diag().skip.empty() && DiagSkip(flog_ev)))
          DrawPrimitive(cmd);
        if (flog) {
          FrameLogDraw(flog, flog_frame, flog_ev, ci, cmd);
          DiagSnap(flog_frame, flog_ev++);
        }
        if (TraceTHPDraw()) {
          LogTHPDraw(cmd);
          FlushBatch();
          GLint viewport[4]{};
          unsigned char pixel[4]{};
          glGetIntegerv(GL_VIEWPORT, viewport);
          glReadPixels(viewport[0] + viewport[2] / 2, viewport[1] + viewport[3] / 2,
                       1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
          std::printf("[THP framebuffer] center=%u,%u,%u,%u glError=0x%x\n",
                      pixel[0], pixel[1], pixel[2], pixel[3], glGetError());
        }
      }
    }
    FlushBatch();
    glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    if (!ih)
      MvFrameEnd(); // MVGEN21: motion attachment out of the draw buffers, recording off
    NeutralGLState();
  }

  void Present() override {}

private:
  void *m_window = nullptr;
};

std::unique_ptr<IRenderer> CreateRendererGL() {
  return std::make_unique<RendererGL>();
}

// ---------------------------------------------------------------------------
// INTERP17: in-between frames (frame interpolation; runtime/gx/interp_render.h)
// ---------------------------------------------------------------------------
static GLuint s_ib_fbo = 0, s_ib_color = 0, s_ib_depth = 0;
static int s_ib_w = 0, s_ib_h = 0;
static GLint s_ib_cfmt = 0, s_ib_dfmt = 0;
static GLuint s_ib_src_color = 0, s_ib_src_depth = 0;

// Copies the EFB (colour + depth) into a backup FBO (restore = false) or back (restore = true).
static bool InterpEfbBackup(bool restore) {
  if (!s_efb_fbo || !s_efb_color_tex)
    return false;
  if (!restore && (!s_ib_fbo || s_ib_w != s_efb_w || s_ib_h != s_efb_h || s_ib_src_color != s_efb_color_tex ||
                   s_ib_src_depth != s_efb_depth_tex)) {
    GLint cf = GL_RGBA8, df = 0;
    glActiveTexture(GL_TEXTURE0 + kUploadUnit);
    glBindTexture(GL_TEXTURE_2D, s_efb_color_tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &cf);
    if (s_efb_depth_tex) {
      glBindTexture(GL_TEXTURE_2D, s_efb_depth_tex);
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &df);
    }
    if (!s_ib_fbo) {
      glGenFramebuffers(1, &s_ib_fbo);
      glGenTextures(1, &s_ib_color);
      glGenTextures(1, &s_ib_depth);
    }
    glBindTexture(GL_TEXTURE_2D, s_ib_color);
    glTexImage2D(GL_TEXTURE_2D, 0, cf, s_efb_w, s_efb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const bool ds = df == GL_DEPTH24_STENCIL8 || df == GL_DEPTH32F_STENCIL8;
    if (df) {
      glBindTexture(GL_TEXTURE_2D, s_ib_depth);
      const GLenum fmt = ds ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT;
      const GLenum type = df == GL_DEPTH32F_STENCIL8   ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV
                          : df == GL_DEPTH24_STENCIL8  ? GL_UNSIGNED_INT_24_8
                          : df == GL_DEPTH_COMPONENT32F ? GL_FLOAT
                                                        : GL_UNSIGNED_INT;
      glTexImage2D(GL_TEXTURE_2D, 0, df, s_efb_w, s_efb_h, 0, fmt, type, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_ib_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_ib_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, ds ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           df ? s_ib_depth : 0, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    if (!complete) {
      std::printf("[INTERP17] EFB backup FBO incomplete (colour 0x%x depth 0x%x)\n", (unsigned)cf, (unsigned)df);
      return false;
    }
    s_ib_w = s_efb_w;
    s_ib_h = s_efb_h;
    s_ib_cfmt = cf;
    s_ib_dfmt = df;
    s_ib_src_color = s_efb_color_tex;
    s_ib_src_depth = s_efb_depth_tex;
  }
  if (!s_ib_fbo || s_ib_w != s_efb_w || s_ib_h != s_efb_h)
    return false;
  glBindFramebuffer(GL_READ_FRAMEBUFFER, restore ? s_ib_fbo : s_efb_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, restore ? s_efb_fbo : s_ib_fbo);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glBlitFramebuffer(0, 0, s_efb_w, s_efb_h, 0, 0, s_efb_w, s_efb_h,
                    GL_COLOR_BUFFER_BIT | (s_ib_dfmt ? GL_DEPTH_BUFFER_BIT : 0), GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  s_fixed_valid = false;
  return true;
}

// The render-time part of g_state (Render() and ApplyBPRegister write it). cp, vat, the array bases/
// strides and the default matrix indices in front of numTevStages belong to the parser, which runs on
// the parse worker while the host renders: they are neither saved nor restored.
static constexpr size_t kInterpTailOff = offsetof(GXState, numTevStages);
struct InterpSavedState {
  uint32_t xf[256], bp[256], tc[8], tk[8];
  uint8_t tail[sizeof(GXState) - kInterpTailOff]; // numTevStages .. tlutSrcAddr (incl. TLUT memory)
};
static void InterpSaveState(InterpSavedState &s, bool restore) {
  uint8_t *const tail = reinterpret_cast<uint8_t *>(&g_state) + kInterpTailOff;
  if (!restore) {
    std::memcpy(s.xf, g_state.xf, sizeof s.xf);
    std::memcpy(s.bp, g_state.bp, sizeof s.bp);
    std::memcpy(s.tc, g_state.tevColorReg, sizeof s.tc);
    std::memcpy(s.tk, g_state.tevKonstReg, sizeof s.tk);
    std::memcpy(s.tail, tail, sizeof s.tail);
  } else {
    std::memcpy(g_state.xf, s.xf, sizeof s.xf);
    std::memcpy(g_state.bp, s.bp, sizeof s.bp);
    std::memcpy(g_state.tevColorReg, s.tc, sizeof s.tc);
    std::memcpy(g_state.tevKonstReg, s.tk, sizeof s.tk);
    std::memcpy(tail, s.tail, sizeof s.tail);
  }
}

bool RenderInterpFrame(IRenderer &r, const std::vector<GXCommand> &commands, InterpDrawHook &hook) {
  QueryEfbTarget();
  if (!s_efb_fbo || s_interp_hook)
    return false;
  static InterpSavedState *const saved = new InterpSavedState(); // ~0.5 MB (TLUT memory): never freed
  InterpSaveState(*saved, false);
  float post[256];
  std::memcpy(post, s_post_mtx, sizeof post);
  const RendererStats stats = s_stats;
  const uint64_t skipped = s_skipped_draws, filtered = g_stat_filtered_copies;
  static const bool noiso = [] {
    const char *e = std::getenv("NWII_INTERP_NO_ISOLATION");
    return e && *e == '1';
  }();
  s_interp_noiso = noiso;
  const bool efb_saved = !noiso && InterpEfbBackup(false);
  static bool warned = false;
  if (!efb_saved && !warned && !noiso) {
    warned = true;
    std::printf("[INTERP17] no EFB backup: in-between frames run without EFB restore\n");
  }
  for (auto &kv : s_interp_copies)
    kv.second.valid = false;
  s_interp_ram_ok.clear();
  s_interp_hook = &hook;
  r.Render(commands);
  s_interp_hook = nullptr;
  const bool complete = s_skipped_draws == skipped;
  s_interp_stats.skipped_draws += s_skipped_draws - skipped;
  s_interp_stats.xfb_copies += s_stats.xfb_copies - stats.xfb_copies;
  s_interp_stats.flushes += s_stats.flushes - stats.flushes;
  s_interp_stats.state_syncs += s_stats.state_syncs - stats.state_syncs;
  s_interp_stats.uniform_uploads += s_stats.uniform_uploads - stats.uniform_uploads;
  s_interp_stats.tex_lookups += s_stats.tex_lookups - stats.tex_lookups;
  s_interp_stats.vertices += s_stats.vertices - stats.vertices;
  ++s_interp_stats.frames;
  if (efb_saved)
    InterpEfbBackup(true);
  if (!noiso) { // (the leak test's negative control keeps the in-between frame's end state, like INTERP16's proto)
    InterpSaveState(*saved, true);
    std::memcpy(s_post_mtx, post, sizeof post);
  }
  s_stats = stats;
  s_skipped_draws = skipped;
  g_stat_filtered_copies = filtered;
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  InvalidateGLState();
  NeutralGLState();
  // A shader still compiling can leave whole objects out of I. Showing that
  // partial image between complete real frames looks like geometry flashing.
  // Keep the compiled shaders, restore real-frame state, and let the caller
  // present N alone until every draw in I is available.
  return complete;
}

void InterpWriteXF(int addr, const float *v, int n) {
  bool touched_proj = false;
  for (int i = 0; i < n; ++i) { // the stream mapping of RendererGL::Render
    const int a = addr + i;
    if (a >= 0x1000 && a < 0x1000 + 256)
      std::memcpy(&g_state.xf[a - 0x1000], &v[i], 4);
    if (a >= 0x0000 && a <= 0x00FF) {
      g_state.posMatrices[a] = v[i];
    } else if (a >= 0x0400 && a <= 0x045F) {
      g_state.normalMatrices[a - 0x0400] = v[i];
    } else if (a >= 0x0500 && a <= 0x05FF) {
      s_post_mtx[a - 0x0500] = v[i];
    } else if (a >= 0x0600 && a <= 0x067F) {
      g_state.lights[(a - 0x0600) / 16][(a - 0x0600) % 16] = v[i];
    } else if (a >= 0x1020 && a <= 0x1026) {
      g_state.projection[a - 0x1020] = v[i];
      if (a == 0x1026)
        std::memcpy(&g_state.projType, &v[i], 4);
      touched_proj = true;
    }
  }
  if (touched_proj)
    g_state.projSet = true;
  s_dirty |= DirtyForXF(addr, n);
}

void InterpWriteBP(uint8_t reg, uint32_t val) {
  ApplyBPRegister(reg, val);
  s_dirty |= DirtyForBP(reg);
}

const float *InterpLivePosMatrices() { return g_state.posMatrices; }
const float *InterpLiveNrmMatrices() { return g_state.normalMatrices; }
InterpRenderStats GetInterpRenderStats() { return s_interp_stats; }
[[maybe_unused]] static const bool s_interp_api_registered = [] { // interp_render.h InterpRendererApi
  g_interp_renderer.render = &RenderInterpFrame;
  g_interp_renderer.write_xf = &InterpWriteXF;
  g_interp_renderer.write_bp = &InterpWriteBP;
  g_interp_renderer.live_pos = &InterpLivePosMatrices;
  g_interp_renderer.live_nrm = &InterpLiveNrmMatrices;
  g_interp_renderer.stats = &GetInterpRenderStats;
  return true;
}();

// ---------------------------------------------------------------------------
// MVGEN21: motion vectors of a real frame + the GPU warp in-between frame (runtime/gx/frame_motion.h,
// interp_render.h MotionArm / WarpRequest). Two slots (N-1, N), each: motion texture (RGBA32F: prev - cur px,
// view depth now, view depth in N-1), the EFB colour + depth at the 2D boundary, the render-time state there.
// ---------------------------------------------------------------------------
namespace mvgen21 {
namespace mo = nwii::runtime::gx::motion;
struct Slot {
  GLuint mtex = 0, mfbo = 0;           // motion texture + an FBO of its own (clear)
  GLuint bfbo = 0, bcol = 0, bdep = 0; // boundary snapshot
  uint64_t frame = 0;
  bool valid = false;                  // recorded and snapshotted at the boundary
  size_t boundary = 0;
  InterpSavedState *state = nullptr;   // render-time g_state at the boundary (~0.5 MB, kept)
  float post[256] = {};
  decltype(s_q) q{};
  float proj[16] = {};
  int vp[4] = {};
  bool proj_ok = false;
};
static Slot s_ms[2];
static int s_cur = 0; // slot of the latest recording
static int s_w = 0, s_h = 0;
static GLuint s_src_c = 0, s_src_d = 0;
static GLint s_cfmt = 0, s_dfmt = 0;
static GLuint s_wprog = 0, s_wfbo = 0, s_wfbo_tex = 0, s_psave_fbo = 0, s_psave_tex = 0;
static int s_psave_w = 0, s_psave_h = 0;
static GLint s_wu_cur = -1, s_wu_prev = -1, s_wu_curm = -1, s_wu_prevm = -1, s_wu_size = -1, s_wu_t = -1,
             s_wu_px = -1, s_wu_vp = -1, s_wu_proj = -1, s_wu_rot = -1, s_wu_flags = -1;
static WarpStats s_st;
static GLuint s_tq[4][4] = {}; // GPU timestamps (NWII_MVGEN_GPUTIME=1): ring of [start, pass start, pass end, end]
static bool s_tq_busy[4] = {};
static unsigned s_tq_i = 0;
static bool GpuTime() {
  static const bool on = [] { const char *e = std::getenv("NWII_MVGEN_GPUTIME"); return e && *e == '1'; }();
  return on;
}
// NEGATIVE control of the warp leak test (warp_leak_test.py --neg-restore): the EFB and the present framebuffer are
// not put back after the warp frame, so the real frames change.
static bool NoRestore() {
  static const bool on = [] { const char *e = std::getenv("NWII_MVGEN_NO_RESTORE"); return e && *e == '1'; }();
  return on;
}
static int DefaultFlags() {
  static const int f = [] { const char *e = std::getenv("NWII_MVGEN_FLAGS"); return e && *e ? std::atoi(e) : mo::kWarpSky | mo::kWarpBlend; }();
  return f;
}

struct TailHook final : InterpDrawHook { // the tail is drawn exactly as the real frame drew it
  bool Pre(size_t, const GXCommand &) override { return true; }
  void PostConvert(const GXCommand &, float *, size_t) override {}
  void Post(size_t, const GXCommand &) override {}
};
static TailHook s_tail_hook;

static void MakeTex(GLuint t, GLint ifmt, GLenum fmt, GLenum type, GLint filter) {
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, ifmt, s_w, s_h, 0, fmt, type, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// The two slots at the EFB's size and formats (recreated when the EFB changes; slots then invalid).
static bool Ensure() {
  if (!s_efb_fbo || !s_efb_color_tex)
    return false;
  if (s_ms[0].mtex && s_w == s_efb_w && s_h == s_efb_h && s_src_c == s_efb_color_tex && s_src_d == s_efb_depth_tex)
    return true;
  GLint cf = GL_RGBA8, df = 0;
  glActiveTexture(GL_TEXTURE0 + kUploadUnit);
  glBindTexture(GL_TEXTURE_2D, s_efb_color_tex);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &cf);
  if (s_efb_depth_tex) {
    glBindTexture(GL_TEXTURE_2D, s_efb_depth_tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &df);
  }
  s_w = s_efb_w;
  s_h = s_efb_h;
  s_cfmt = cf;
  s_dfmt = df;
  const bool ds = df == GL_DEPTH24_STENCIL8 || df == GL_DEPTH32F_STENCIL8;
  bool ok = true;
  for (Slot &sl : s_ms) {
    if (!sl.mtex) {
      glGenTextures(1, &sl.mtex);
      glGenFramebuffers(1, &sl.mfbo);
      glGenTextures(1, &sl.bcol);
      glGenTextures(1, &sl.bdep);
      glGenFramebuffers(1, &sl.bfbo);
      sl.state = new InterpSavedState();
    }
    MakeTex(sl.mtex, GL_RGBA32F, GL_RGBA, GL_FLOAT, GL_NEAREST);
    MakeTex(sl.bcol, cf, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR); // the warp samples it bilinear
    if (df) {
      const GLenum type = df == GL_DEPTH32F_STENCIL8   ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV
                          : df == GL_DEPTH24_STENCIL8  ? GL_UNSIGNED_INT_24_8
                          : df == GL_DEPTH_COMPONENT32F ? GL_FLOAT
                                                        : GL_UNSIGNED_INT;
      MakeTex(sl.bdep, df, ds ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT, type, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sl.mfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sl.mtex, 0);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, sl.bfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sl.bcol, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, ds ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           df ? sl.bdep : 0, 0);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    sl.valid = false;
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  s_src_c = s_efb_color_tex;
  s_src_d = s_efb_depth_tex;
  s_mv.attached = false; // re-attached (fresh storage) by the next armed frame
  std::printf("[MVGEN21] motion targets %dx%d (colour 0x%x depth 0x%x)%s\n", s_w, s_h, (unsigned)cf, (unsigned)df,
              ok ? "" : ": FBO INCOMPLETE, warp off");
  if (!ok)
    s_mv.unsupported = true;
  return ok;
}

static bool WarpProgram() {
  if (s_wprog)
    return true;
  if (s_mv.unsupported || !EnsureCopyProgram())
    return false;
  s_wprog = CompileGLShaderProgram(kCopyVS, mo::kWarpFS);
  if (!s_wprog) {
    std::printf("[MVGEN21] warp program failed to compile: warp off\n");
    s_mv.unsupported = true;
    return false;
  }
  s_wu_cur = glGetUniformLocation(s_wprog, "uCurC");
  s_wu_prev = glGetUniformLocation(s_wprog, "uPrevC");
  s_wu_curm = glGetUniformLocation(s_wprog, "uCurM");
  s_wu_prevm = glGetUniformLocation(s_wprog, "uPrevM");
  s_wu_size = glGetUniformLocation(s_wprog, "uSize");
  s_wu_t = glGetUniformLocation(s_wprog, "uT");
  s_wu_px = glGetUniformLocation(s_wprog, "uPx");
  s_wu_vp = glGetUniformLocation(s_wprog, "uVp");
  s_wu_proj = glGetUniformLocation(s_wprog, "uProj");
  s_wu_rot = glGetUniformLocation(s_wprog, "uSkyRot");
  s_wu_flags = glGetUniformLocation(s_wprog, "uFlags");
  return true;
}

static void PollTimers() {
  for (int i = 0; i < 4; ++i) {
    if (!s_tq_busy[i])
      continue;
    GLuint avail = 0;
    glGetQueryObjectuiv(s_tq[i][3], GL_QUERY_RESULT_AVAILABLE, &avail);
    if (!avail)
      continue;
    GLuint64 t[4] = {};
    for (int k = 0; k < 4; ++k)
      glGetQueryObjectui64v(s_tq[i][k], GL_QUERY_RESULT, &t[k]);
    s_st.gpu_ms += (double)(t[3] - t[0]) / 1e6;
    s_st.pass_ms += (double)(t[2] - t[1]) / 1e6;
    ++s_st.gpu_n;
    s_tq_busy[i] = false;
  }
}

// The warp pass: the EFB colour := warp of the two boundary snapshots (EFB-sized, full-screen triangle).
static void WarpPass(const Slot &cur, const Slot &prev, const WarpRequest &q) {
  if (!s_wfbo)
    glGenFramebuffers(1, &s_wfbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_wfbo);
  if (s_wfbo_tex != s_efb_color_tex) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_efb_color_tex, 0);
    s_wfbo_tex = s_efb_color_tex;
  }
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_COLOR_LOGIC_OP);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, s_w, s_h);
  glUseProgram(s_wprog);
  s_bound_prog = s_wprog;
  const GLuint tex[4] = {cur.bcol, prev.bcol, cur.mtex, prev.mtex};
  const GLint loc[4] = {s_wu_cur, s_wu_prev, s_wu_curm, s_wu_prevm};
  for (int i = 0; i < 4; ++i) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, tex[i]);
    glBindSampler((GLuint)i, 0);
    glUniform1i(loc[i], i);
  }
  int flags = q.flags >= 0 ? q.flags : DefaultFlags();
  float vp[4] = {0, 0, (float)s_w, (float)s_h}, pj[4] = {1, 1, 0, 0};
  if (cur.proj_ok) {
    for (int i = 0; i < 4; ++i) vp[i] = (float)cur.vp[i];
    pj[0] = cur.proj[0];
    pj[1] = cur.proj[5];
    pj[2] = cur.proj[8];
    pj[3] = cur.proj[9];
  }
  if (!cur.proj_ok || !q.sky_ok || pj[0] == 0.0f || pj[1] == 0.0f)
    flags &= ~mo::kWarpSky;
  glUniform2f(s_wu_size, (float)s_w, (float)s_h);
  glUniform1f(s_wu_t, q.t);
  glUniform1f(s_wu_px, (float)s_scale);
  glUniform4fv(s_wu_vp, 1, vp);
  glUniform4fv(s_wu_proj, 1, pj);
  glUniformMatrix3fv(s_wu_rot, 1, GL_TRUE, q.sky_rot); // row-major
  glUniform1i(s_wu_flags, flags);
  glBindVertexArray(s_empty_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  for (int i = 0; i < 4; ++i) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  glActiveTexture(GL_TEXTURE0);
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
}

static void Blit(GLuint from, GLuint to, int w, int h, GLbitfield bits) {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, from);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, bits, GL_NEAREST);
}

bool RenderWarp(IRenderer &r, const std::vector<GXCommand> &commands, const WarpRequest &q) {
  QueryEfbTarget();
  Slot &cur = s_ms[s_cur], &prev = s_ms[s_cur ^ 1];
  if (s_interp_hook || s_mv.unsupported || !cur.valid || !prev.valid || cur.frame != q.cur_frame ||
      prev.frame != q.prev_frame || s_w != s_efb_w || s_h != s_efb_h || s_src_c != s_efb_color_tex ||
      cur.boundary >= commands.size() || !WarpProgram()) {
    ++s_st.failed;
    return false;
  }
  if (GpuTime()) {
    PollTimers();
    if (!s_tq[0][0])
      glGenQueries(16, &s_tq[0][0]);
  }
  const unsigned ti = s_tq_i++ & 3;
  const bool timed = GpuTime() && !s_tq_busy[ti];
  if (timed)
    glQueryCounter(s_tq[ti][0], GL_TIMESTAMP);
  // the real frame's image, handed to the present thread after I (WarpDone puts it back)
  EnsurePresentTarget();
  if (!s_psave_fbo) {
    glGenTextures(1, &s_psave_tex);
    glGenFramebuffers(1, &s_psave_fbo);
  }
  if (s_psave_w != s_present_w || s_psave_h != s_present_h) {
    glActiveTexture(GL_TEXTURE0 + kUploadUnit);
    glBindTexture(GL_TEXTURE_2D, s_psave_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_present_w, s_present_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_psave_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_psave_tex, 0);
    s_psave_w = s_present_w;
    s_psave_h = s_present_h;
  }
  Blit(s_present_fbo, s_psave_fbo, s_present_w, s_present_h, GL_COLOR_BUFFER_BIT);
  // INTERP17's isolated scope (RenderInterpFrame), entered at the boundary
  static InterpSavedState *const saved = new InterpSavedState();
  InterpSaveState(*saved, false);
  float post[256];
  std::memcpy(post, s_post_mtx, sizeof post);
  const auto q_end = s_q;
  const RendererStats stats = s_stats;
  const uint64_t skipped = s_skipped_draws, filtered = g_stat_filtered_copies;
  s_interp_noiso = false;
  const bool efb_saved = InterpEfbBackup(false);
  if (!efb_saved) { // no way to put N's EFB back: no warp frame (nothing changed yet)
    glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    InvalidateGLState();
    NeutralGLState();
    ++s_st.failed;
    return false;
  }
  // EFB := N at the boundary; its colour := the warp of N-1 and N
  Blit(cur.bfbo, s_efb_fbo, s_w, s_h, GL_COLOR_BUFFER_BIT | (s_dfmt ? GL_DEPTH_BUFFER_BIT : 0));
  if (timed)
    glQueryCounter(s_tq[ti][1], GL_TIMESTAMP);
  WarpPass(cur, prev, q);
  if (timed)
    glQueryCounter(s_tq[ti][2], GL_TIMESTAMP);
  InterpSaveState(*cur.state, true);
  std::memcpy(s_post_mtx, cur.post, sizeof cur.post);
  s_q = cur.q;
  for (auto &kv : s_interp_copies)
    kv.second.valid = false;
  s_interp_ram_ok.clear();
  s_interp_begin = cur.boundary;
  s_interp_hook = &s_tail_hook;
  r.Render(commands);
  s_interp_hook = nullptr;
  s_interp_begin = 0;
  if (efb_saved && !NoRestore())
    InterpEfbBackup(true);
  InterpSaveState(*saved, true);
  std::memcpy(s_post_mtx, post, sizeof post);
  s_q = q_end;
  s_stats = stats;
  s_skipped_draws = skipped;
  g_stat_filtered_copies = filtered;
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  InvalidateGLState();
  NeutralGLState();
  if (timed) {
    glQueryCounter(s_tq[ti][3], GL_TIMESTAMP);
    s_tq_busy[ti] = true;
  }
  ++s_st.warps;
  return true; // GPU time: whole = blits + warp pass + tail replay + restores; pass = the warp pass alone
}

void WarpDone() {
  if (!s_psave_fbo || s_psave_w != s_present_w || s_psave_h != s_present_h || NoRestore())
    return;
  Blit(s_psave_fbo, s_present_fbo, s_present_w, s_present_h, GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  s_fixed_valid = false;
}

void Arm(const MotionArm &a) {
  s_mv.arm = a;
  s_mv.armed = a.hook != nullptr && !s_mv.unsupported;
}
bool Recorded(uint64_t frame) {
  for (const Slot &sl : s_ms)
    if (sl.valid && sl.frame == frame)
      return true;
  return false;
}
WarpStats Stats() {
  WarpStats w = s_st;
  w.mv_batches = s_mv.batches;
  w.mv_fallbacks = s_mv.fallbacks;
  w.db_switches = s_mv.db_switches;
  w.unsupported = s_mv.unsupported;
  return w;
}
} // namespace mvgen21

static void MvDrawBuffers(bool two) {
  static const GLenum kOne[2] = {GL_COLOR_ATTACHMENT0, GL_NONE};
  static const GLenum kTwo[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
  glDrawBuffers(2, two ? kTwo : kOne);
  s_mv.db_two = two;
  ++s_mv.db_switches;
}

static void MvBeforeDraw() {
  if (!s_mv.db_two) {
    MvDrawBuffers(true);
    // blending is off for motion batches, but the GL blend function may still name the second (dual-source)
    // colour from an earlier blended draw; some drivers check it against MAX_DUAL_SOURCE_DRAW_BUFFERS (1).
    // ApplyFixed sets the function again whenever blending turns on.
    glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
  }
  glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); // ApplyFixed's glColorMask sets every draw buffer
  ++s_mv.batches;
  const size_t nv = s_batch.size() / (BatchFloats(s_batch_k) + 3);
  if (nv > s_mv.best_n) { // the scene camera for the background motion: the largest motion batch's
    s_mv.best_n = nv;
    std::memcpy(s_mv.best_proj, s_mv.cur_proj, sizeof s_mv.best_proj);
    std::memcpy(s_mv.best_vp, s_mv.cur_vp, sizeof s_mv.best_vp);
  }
}

static bool MvInject(std::string &vs, std::string &fs) {
  static const char *kVsDecl = "uniform mat4 mvp;\n";
  static const char *kVsPos = "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n";
  static const char *kFsDecl = "layout(location = 0, index = 1) out vec4 FragColor1;\n";
  const size_t a = vs.find(kVsDecl), b = vs.find(kVsPos), c = fs.find(kFsDecl), e = fs.rfind('}');
  if (a == std::string::npos || b == std::string::npos || c == std::string::npos || e == std::string::npos)
    return false;
  vs.insert(b + std::strlen(kVsPos), "    vMvCur = gl_Position;\n    vMvPrev = mvp * vec4(aMvPrev, 1.0);\n");
  vs.insert(a + std::strlen(kVsDecl), "layout(location = 11) in vec3 aMvPrev;\nout vec4 vMvCur;\nout vec4 vMvPrev;\n");
  fs.insert(e, "    {\n    vec2 mvC = vMvCur.xy / vMvCur.w, mvP = vMvPrev.xy / vMvPrev.w;\n"
               "    oMvMotion = vMvPrev.w > 1e-6 ? vec4((mvP - mvC) * uMvVp, vMvCur.w, vMvPrev.w)"
               " : vec4(0.0, 0.0, vMvCur.w, -1.0);\n    }\n");
  // These variants only render opaque, unblended draws. Retaining the
  // secondary blend-source output forbids a second colour attachment on
  // drivers with one dual-source draw buffer. Keep its assignments local.
  fs.replace(c, std::strlen(kFsDecl),
             "vec4 FragColor1;\nlayout(location = 1) out vec4 oMvMotion;\nin vec4 vMvCur;\nin vec4 vMvPrev;\nuniform vec2 uMvVp;\n");
  return true;
}

static void MvFrameStart() {
  s_mv.compiles = 0;
  if (!s_mv.armed) {
    if (s_mv.attached) { // interpolation off / another mode: the host's EFB FBO as it was
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
      s_mv.attached = false;
    }
    return;
  }
  if (!mvgen21::Ensure()) {
    s_mv.armed = false;
    return;
  }
  using mvgen21::s_ms;
  const int slot = mvgen21::s_cur ^ 1;
  mvgen21::Slot &sl = s_ms[slot];
  sl.valid = false;
  sl.frame = s_mv.arm.frame;
  sl.boundary = s_mv.arm.boundary;
  sl.proj_ok = false;
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sl.mfbo);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  static const float kZero[4] = {0, 0, 0, 0};
  glClearBufferfv(GL_COLOR, 0, kZero);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_efb_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sl.mtex, 0);
  s_mv.attached = true;
  MvDrawBuffers(false);
  mvgen21::s_cur = slot;
  s_mv.recording = true;
  s_mv.best_n = 0;
  ++mvgen21::s_st.recorded;
  s_fixed_valid = false;
}

static void MvBoundary() {
  FlushBatch();
  if (s_mv.db_two)
    MvDrawBuffers(false);
  mvgen21::Slot &sl = mvgen21::s_ms[mvgen21::s_cur];
  mvgen21::Blit(s_efb_fbo, sl.bfbo, mvgen21::s_w, mvgen21::s_h,
                GL_COLOR_BUFFER_BIT | (mvgen21::s_dfmt ? GL_DEPTH_BUFFER_BIT : 0));
  glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
  InterpSaveState(*sl.state, false);
  std::memcpy(sl.post, s_post_mtx, sizeof sl.post);
  sl.q = s_q;
  sl.proj_ok = s_mv.best_n > 0;
  std::memcpy(sl.proj, s_mv.best_proj, sizeof sl.proj);
  std::memcpy(sl.vp, s_mv.best_vp, sizeof sl.vp);
  sl.valid = true;
  s_mv.recording = false;
  s_dirty |= D_SHADER; // the next draw picks its (plain) program again
  s_fixed_valid = false;
}

static void MvFrameEnd() {
  if (s_mv.db_two)
    MvDrawBuffers(false);
  s_mv.recording = false; // (boundary never reached: the slot stays invalid)
  s_mv.armed = false;
}

[[maybe_unused]] static const bool s_mv_api_registered = [] { // MVGEN21: interp_render.h InterpRendererApi
  g_interp_renderer.mv_arm = &mvgen21::Arm;
  g_interp_renderer.mv_recorded = &mvgen21::Recorded;
  g_interp_renderer.warp = &mvgen21::RenderWarp;
  g_interp_renderer.warp_done = &mvgen21::WarpDone;
  g_interp_renderer.warp_stats = &mvgen21::Stats;
  return true;
}();

} // namespace nwii::runtime::gx

uint64_t nwii_stat_hash0() { return nwii::runtime::gx::g_stat_hash0; }
uint64_t nwii_stat_shaders() { return nwii::runtime::gx::g_stat_shaders; }
// SOAK14 diagnostic ([STAT] texcache= efbcopy=): decoded-texture cache entries and clears (generation), live EFB-copy
// GL textures and their size in bytes (RGBA8 at internal scale).
void nwii_stat_texmem(uint64_t& cache_entries, uint64_t& cache_clears, uint64_t& efb_textures, uint64_t& efb_bytes) {
  cache_entries = nwii::runtime::hle::TextureCache::get().size();
  cache_clears = nwii::runtime::hle::g_texture_cache_generation;
  efb_textures = nwii::runtime::gx::s_copy_tex_size.size();
  efb_bytes = 0;
  for (const auto& e : nwii::runtime::gx::s_copy_tex_size)
    efb_bytes += uint64_t(e.second.first) * uint64_t(e.second.second) * 4u;
}

namespace nwii::runtime::gx {}
