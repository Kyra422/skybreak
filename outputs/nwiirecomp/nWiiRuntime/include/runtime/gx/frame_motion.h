#pragma once
// MVGEN21: motion vectors of a real frame and the GPU warp in-between frame (CPU side: no GL, no globals).
//
// With frame interpolation on and [graphics] frame_interpolation_mode = "auto" | "warp", every real frame N
// records, while it renders:
//   - per pixel the screen motion of the visible opaque 3D surface from N-1 to N (renderer_gl.cpp, MVGEN21
//     block: perspective batches that write depth and are not blended get a second colour attachment; each
//     batch vertex carries its previous-frame view-space position, computed here by PrevForDraw from the
//     INTERP17 match of N against N-1), plus its view depth now and in N-1;
//   - a snapshot of the EFB (colour, depth) and of the render-time state at the 2D boundary = the first
//     command after the last perspective draw (in SS: before the copies, bloom chain, composite and HUD).
// The in-between frame I (time t between N-1 and N) is then made after N rendered: kWarpFS warps the two
// boundary snapshots along the motion (depth-aware: the nearest surface wins, hidden-in-N areas come from
// N-1, prev/cur blended only where the surface is visible in both), and N's tail (copies, bloom, HUD, XFB
// copy) is replayed unwarped on top in INTERP17's isolated scope. CPU cost ~ the tail (2D) only.
// Design + numbers: work/claude-notes/MVGEN21.md. Unit test: outputs/pc-tests/mvgen21/mvgen21-test.cpp
// (WarpRef below mirrors kWarpFS line by line; the GPU test compares the two).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "runtime/gx/frame_interp.h"

namespace nwii::runtime::gx::motion {
namespace ip = nwii::runtime::gx::interp;

// ------------------------------------------------------------------------------------------ prev positions
// Per-draw cache of A's live position rows (a draw reads at most 64 rows).
struct PrevCache {
  int32_t draw = -1;     // B draw the cache belongs to
  uint64_t done = 0;     // live rows looked up
  float live[64][12];
  int8_t use_a = -1;     // -1 unknown, 0 B's model data, 1 A's (vertex animation)
};

inline void AffApply(const float *m, const float *p, float *o) {
  o[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
  o[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
  o[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}

// Does the matched A draw's model-space vertex data stand for B's (vertex animation: water, cloth)?
// Same vertex count, data different, every vertex moved less than half the draw's size (INTERP17 vlerp rule).
inline bool UseAData(const GXCommand &b, const GXCommand *a) {
  if (!a || a->vertices.size() != b.vertices.size() || b.vertices.empty()) return false;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, maxd = 0;
  for (size_t i = 0; i < b.vertices.size(); ++i) {
    float dd = 0;
    for (int q = 0; q < 3; ++q) {
      const float x = b.vertices[i].raw_pos[q];
      lo[q] = std::min(lo[q], x);
      hi[q] = std::max(hi[q], x);
      const float e = x - a->vertices[i].raw_pos[q];
      dd += e * e;
    }
    maxd = std::max(maxd, dd);
  }
  if (maxd <= 0) return false;
  const float diag2 = (hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]);
  return maxd <= 0.25f * diag2 + 1e-6f;
}

// Writes, for each of B draw di's converted vertices (conv: F floats per vertex, view-space x,y,z at 0), the
// view-space position the same surface point had in A (= N-1) at conv[i * F + off .. +2]:
//   matched draw (INTERP17 plan, not rejected):  A's matrix (snapshot, skinning-bank slot or live row of the
//                                                vertex) times the model-space position (A's own vertex data
//                                                when the draw's vertices are animated);
//   unmatched / rejected / no matrix:            static in the world: dVinv * pos_B (camera delta), or pos_B
//                                                (no motion) without a camera estimate;
//   no plan (A null, cut, no history):           pos_B (zero motion; the depth still feeds the next warp).
// acmd = A's decoded command of the matched draw (may be null); vh_a / vh_b: their vertex hashes (0 unknown).
inline void PrevForDraw(const ip::FrameRec *A, const ip::FrameRec &B, const ip::Plan *P, size_t di,
                        const GXCommand &bcmd, const GXCommand *acmd, uint64_t vh_a, uint64_t vh_b, float *conv,
                        size_t F, size_t off, PrevCache &pc) {
  const size_t n = bcmd.vertices.size();
  const bool plan = A && P && !P->cut && di < B.draws.size() && di < P->dp.size();
  const float *ws = plan && P->cam.ok ? P->cam.dVinv.m : nullptr; // B view -> A view of a static point
  auto world_static = [&](float *o) {
    if (ws) AffApply(ws, o, o + off);
    else std::memcpy(o + off, o, 12);
  };
  if (!plan) {
    for (size_t i = 0; i < n; ++i) std::memcpy(conv + i * F + off, conv + i * F, 12);
    return;
  }
  const ip::DrawRec &d = B.draws[di];
  if (!(P->dp[di].flags & ip::kDpInterp) || d.match < 0 || (size_t)d.match >= A->draws.size()) {
    for (size_t i = 0; i < n; ++i) world_static(conv + i * F);
    return;
  }
  const ip::DrawRec &a = A->draws[d.match];
  if (pc.draw != (int32_t)di) {
    pc.draw = (int32_t)di;
    pc.done = 0;
    pc.use_a = -1;
  }
  if (pc.use_a < 0) pc.use_a = (vh_a && vh_b && vh_a == vh_b) ? 0 : (UseAData(bcmd, acmd) ? 1 : 0);
  const bool bank = d.kind == ip::kBank && a.kind == ip::kBank && d.bank && a.bank;
  for (size_t i = 0; i < n; ++i) {
    const VertexData &v = bcmd.vertices[i];
    float *o = conv + i * F;
    const float *m = nullptr;
    if (v.has_pos) {
      if (v.pre_xf) {
        if (d.kind == ip::kSnap && a.kind == ip::kSnap) m = a.mv.m;
        else if (bank) {
          const uint32_t s = v.posMtxIdx & 0x3F;
          if ((size_t)s * 4 + 12 <= a.bank->snap_pos_bank.size()) m = &a.bank->snap_pos_bank[(size_t)s * 4];
        }
      } else if (a.kind == ip::kLive && d.kind == ip::kLive) {
        const uint32_t s = v.posMtxIdx;
        if (s * 4 + 12 <= 256) {
          if (!(pc.done >> s & 1)) {
            for (int j = 0; j < 3; ++j) std::memcpy(&pc.live[s][j * 4], A->AtFixed(ip::kRegPos + (int)s + j, a.seq), 16);
            pc.done |= 1ull << s;
          }
          m = pc.live[s];
        }
      }
    }
    if (!m) {
      world_static(o);
      continue;
    }
    const float *raw = pc.use_a == 1 ? acmd->vertices[i].raw_pos : v.raw_pos;
    AffApply(m, raw, o + off);
  }
}

// ------------------------------------------------------------------------------------------ auto mode
// [graphics] frame_interpolation_mode = "auto": re-render the in-between frame (exact geometry) when the host
// fits both renders in the guest frame, else warp it on the GPU. The re-render cost is predicted from the real
// render (measured every frame) times the measured ratio (I + prepare) / N, so no trial frame is needed (a
// re-render over budget makes the cutter merge the next guest frame = a visible hitch). Switching back to
// re-rendering needs 1 s with a clear margin (no flip-flop between the two looks).
struct AutoDecider {
  float ratio = 1.4f;            // (render_I + prepare) / render_N, EMA of measured re-render frames
  bool rerender_ok = true;
  int64_t ok_since_us = 0;
  static constexpr float kEnter = 0.90f, kLeave = 0.80f; // of the guest period
  static constexpr int64_t kHoldUs = 1000000;
  // elapsed = cut -> now (the decode is done), rn = real render time (recent worst), period = guest period
  bool Rerender(int64_t now_us, int64_t elapsed_us, int64_t rn_us, int64_t period_us) {
    const double pred = (double)elapsed_us + (double)rn_us * (1.0 + ratio);
    if (pred > kEnter * (double)period_us) {
      rerender_ok = false;
      ok_since_us = 0;
      return false;
    }
    if (rerender_ok) return true;
    if (pred <= kLeave * (double)period_us) {
      if (!ok_since_us) ok_since_us = now_us;
      if (now_us - ok_since_us >= kHoldUs) rerender_ok = true;
    } else {
      ok_since_us = 0;
    }
    return rerender_ok;
  }
  // Recording motion costs the real render ~5-10 % (previous positions, motion attachment): in light scenes "auto"
  // records only when a warp may come soon (re-rendering above 70 % of the guest period).
  bool Close(int64_t elapsed_us, int64_t rn_us, int64_t period_us) const {
    return (double)elapsed_us + (double)rn_us * (1.0 + ratio) > 0.70 * (double)period_us;
  }
  void Measured(int64_t ri_plus_prep_us, int64_t rn_us) {
    if (rn_us <= 0) return;
    const float r = std::clamp((float)ri_plus_prep_us / (float)rn_us, 0.5f, 4.0f);
    ratio = 0.75f * ratio + 0.25f * r;
  }
};

// ------------------------------------------------------------------------------------------ warp
// Motion texture (RGBA32F, EFB size, GL rows bottom first), written by the real frame:
//   xy = prev - cur position of the visible surface (physical EFB px), z = its view depth now (0 = no opaque 3D
//   surface: background), w = its view depth in N-1 (< 0: it was behind the camera).
// Warp flags (uFlags):
enum : int { kWarpSky = 1, kWarpBlend = 2, kWarpDebug = 4, kWarpNoOcclusion = 8, kWarpNoHoles = 16, kWarpNoPrevCheck = 32 };
// kWarpNoOcclusion / kWarpNoHoles / kWarpNoPrevCheck are the negative controls of the unit test (first candidate
// wins / holes filled from N at the same place / N-1 blended in without the depth check).

struct WarpParams {
  int w = 0, h = 0;                // texture size (px)
  float t = 0.5f;                  // I's time: 0 = N-1, 1 = N
  float vp[4] = {0, 0, 0, 0};      // scene viewport (GL px x, y, w, h)
  float proj[4] = {1, 1, 0, 0};    // P[0], P[5], P[8], P[9] of the scene projection (GL column-major)
  float sky_rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // rotation part of dVinv (row-major)
  float px = 1.0f;                 // physical px per EFB px (internal scale): search radii scale with it
  int flags = kWarpSky | kWarpBlend;
};

// The fragment shader (GLSL 3.30). WarpRef below is the same algorithm on the CPU, statement by statement.
inline constexpr const char *kWarpFS = R"(#version 330 core
uniform sampler2D uCurC, uPrevC, uCurM, uPrevM;
uniform vec2 uSize;
uniform float uT, uPx;
uniform vec4 uVp, uProj;
uniform mat3 uSkyRot;
uniform int uFlags;
out vec4 oColor;
const float kInf = 1e30;
vec2 SkyMotion(vec2 p) {
  if ((uFlags & 1) == 0) return vec2(0.0);
  vec2 ndc = (p - uVp.xy) / uVp.zw * 2.0 - 1.0;
  vec3 d = vec3((ndc.x + uProj.z) / uProj.x, (ndc.y + uProj.w) / uProj.y, -1.0);
  vec3 e = uSkyRot * d;
  if (e.z > -1e-4) return vec2(0.0);
  vec2 n = vec2((uProj.x * e.x + uProj.z * e.z) / -e.z, (uProj.y * e.y + uProj.w * e.z) / -e.z);
  return (n * 0.5 + 0.5) * uVp.zw + uVp.xy - p;
}
ivec2 Texel(vec2 p) { return clamp(ivec2(floor(p)), ivec2(0), ivec2(uSize) - 1); }
vec4 Mot(vec2 p) {
  ivec2 ip = Texel(p);
  vec4 m = texelFetch(uCurM, ip, 0);
  if (m.z <= 0.0) m = vec4(SkyMotion(vec2(ip) + 0.5), kInf, kInf);
  return m;
}
float PrevDepth(vec2 p) {
  vec4 m = texelFetch(uPrevM, Texel(p), 0);
  return m.z <= 0.0 ? kInf : m.z;
}
bool InVp(vec2 p) { return p.x >= uVp.x && p.y >= uVp.y && p.x < uVp.x + uVp.z && p.y < uVp.y + uVp.w; }
bool Same(float a, float b) { return abs(a - b) <= 0.02 * min(a, b) + 1e-3; }
const vec2 kDir[8] = vec2[8](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1),
                             vec2(0.7071, 0.7071), vec2(-0.7071, 0.7071), vec2(0.7071, -0.7071), vec2(-0.7071, -0.7071));
const float kRad[4] = float[4](1.0, 3.0, 7.0, 15.0);
void main() {
  vec2 q = gl_FragCoord.xy;
  if (!InVp(q)) { oColor = texelFetch(uCurC, Texel(q), 0); return; }
  float s = 1.0 - uT;
  vec4 mq = Mot(q);
  vec4 mn = mq, mf = mq;
  for (int k = 0; k < 4; ++k)
    for (int j = 0; j < 8; ++j) {
      vec4 m = Mot(q + kDir[j] * (kRad[k] * uPx));
      if (m.z < mn.z) mn = m;
      if (m.z > mf.z) mf = m;
    }
  if ((uFlags & 4) != 0) { oColor = vec4(0.5 + mq.xy / (64.0 * uPx), mq.z < kInf ? 1.0 / (1.0 + mq.z / 2000.0) : 0.0, 1.0); return; }
  bool found = false;
  vec2 bp = q;
  vec4 bm = mq;
  float bz = 4.0 * kInf;
  for (int c = 0; c < 3; ++c) {
    vec4 m0 = c == 0 ? mq : c == 1 ? mn : mf;
    vec2 p = q - s * m0.xy;
    vec4 m = Mot(p);
    for (int i = 0; i < 3; ++i) { p = q - s * m.xy; m = Mot(p); }
    if (InVp(p) && length(p + s * m.xy - q) < 1.0 && m.z < bz) {
      found = true; bz = m.z; bp = p; bm = m;
      if ((uFlags & 8) != 0) break;
    }
  }
  if (found) {
    vec4 cc = texture(uCurC, bp / uSize);
    vec2 pp = bp + bm.xy;
    bool pv = (uFlags & 2) != 0 && bm.w > 0.0 && InVp(pp) && ((uFlags & 32) != 0 || Same(PrevDepth(pp), bm.w));
    oColor = pv ? mix(texture(uPrevC, pp / uSize), cc, uT) : cc;
  } else {
    vec2 pp = q + uT * mf.xy;
    oColor = ((uFlags & 16) == 0 && InVp(pp)) ? texture(uPrevC, pp / uSize) : texture(uCurC, q / uSize);
  }
}
)";

// ---- CPU reference of kWarpFS (unit tests; GL conventions: row 0 = bottom, pixel centres at +0.5) ----
struct Rgba8Image {
  int w = 0, h = 0;
  std::vector<uint8_t> px; // RGBA
  float At(int x, int y, int c) const { return px[((size_t)y * w + x) * 4 + c] / 255.0f; }
};
struct MotionImage {
  int w = 0, h = 0;
  std::vector<float> px; // xyzw
  const float *At(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
};
struct V4 { float x = 0, y = 0, z = 0, w = 0; };

class WarpRef {
public:
  WarpRef(const Rgba8Image &cur, const Rgba8Image &prev, const MotionImage &curm, const MotionImage &prevm,
          const WarpParams &p)
      : C_(cur), Pc_(prev), M_(curm), Pm_(prevm), p_(p) {}

  void Run(Rgba8Image &out) const {
    out.w = p_.w;
    out.h = p_.h;
    out.px.assign((size_t)p_.w * p_.h * 4, 0);
    for (int y = 0; y < p_.h; ++y)
      for (int x = 0; x < p_.w; ++x) {
        float c[4];
        Pixel(x + 0.5f, y + 0.5f, c);
        for (int k = 0; k < 4; ++k)
          out.px[((size_t)y * p_.w + x) * 4 + k] = (uint8_t)std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f);
      }
  }

  void Pixel(float qx, float qy, float *o) const {
    if (!InVp(qx, qy)) { Fetch(C_, qx, qy, o); return; }
    const float s = 1.0f - p_.t;
    const V4 mq = Mot(qx, qy);
    V4 mn = mq, mf = mq;
    static const float kDir[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7071f, 0.7071f}, {-0.7071f, 0.7071f},
                                     {0.7071f, -0.7071f}, {-0.7071f, -0.7071f}};
    static const float kRad[4] = {1, 3, 7, 15};
    for (int k = 0; k < 4; ++k)
      for (int j = 0; j < 8; ++j) {
        const V4 m = Mot(qx + kDir[j][0] * kRad[k] * p_.px, qy + kDir[j][1] * kRad[k] * p_.px);
        if (m.z < mn.z) mn = m;
        if (m.z > mf.z) mf = m;
      }
    bool found = false;
    float bpx = qx, bpy = qy, bz = 4.0f * kInf;
    V4 bm = mq;
    for (int c = 0; c < 3; ++c) {
      const V4 m0 = c == 0 ? mq : c == 1 ? mn : mf;
      float px = qx - s * m0.x, py = qy - s * m0.y;
      V4 m = Mot(px, py);
      for (int i = 0; i < 3; ++i) {
        px = qx - s * m.x;
        py = qy - s * m.y;
        m = Mot(px, py);
      }
      const float rx = px + s * m.x - qx, ry = py + s * m.y - qy;
      if (InVp(px, py) && std::sqrt(rx * rx + ry * ry) < 1.0f && m.z < bz) {
        found = true;
        bz = m.z;
        bpx = px;
        bpy = py;
        bm = m;
        if (p_.flags & kWarpNoOcclusion) break;
      }
    }
    if (found) {
      float cc[4];
      Bilinear(C_, bpx, bpy, cc);
      const float ppx = bpx + bm.x, ppy = bpy + bm.y;
      const bool pv = (p_.flags & kWarpBlend) && bm.w > 0.0f && InVp(ppx, ppy) &&
                      ((p_.flags & kWarpNoPrevCheck) || Same(PrevDepth(ppx, ppy), bm.w));
      if (pv) {
        float pc[4];
        Bilinear(Pc_, ppx, ppy, pc);
        for (int k = 0; k < 4; ++k) o[k] = pc[k] * (1.0f - p_.t) + cc[k] * p_.t;
      } else {
        std::memcpy(o, cc, sizeof cc);
      }
    } else {
      const float ppx = qx + p_.t * mf.x, ppy = qy + p_.t * mf.y;
      if (!(p_.flags & kWarpNoHoles) && InVp(ppx, ppy)) Bilinear(Pc_, ppx, ppy, o);
      else Bilinear(C_, qx, qy, o);
    }
  }

  static constexpr float kInf = 1e30f;

private:
  bool InVp(float x, float y) const {
    return x >= p_.vp[0] && y >= p_.vp[1] && x < p_.vp[0] + p_.vp[2] && y < p_.vp[1] + p_.vp[3];
  }
  static bool Same(float a, float b) { return std::fabs(a - b) <= 0.02f * std::min(a, b) + 1e-3f; }
  void Texel(float x, float y, int &ix, int &iy) const {
    ix = std::clamp((int)std::floor(x), 0, p_.w - 1);
    iy = std::clamp((int)std::floor(y), 0, p_.h - 1);
  }
  void Sky(float x, float y, float &mx, float &my) const {
    mx = my = 0;
    if (!(p_.flags & kWarpSky)) return;
    const float nx = (x - p_.vp[0]) / p_.vp[2] * 2.0f - 1.0f, ny = (y - p_.vp[1]) / p_.vp[3] * 2.0f - 1.0f;
    const float d[3] = {(nx + p_.proj[2]) / p_.proj[0], (ny + p_.proj[3]) / p_.proj[1], -1.0f};
    float e[3];
    for (int i = 0; i < 3; ++i) e[i] = p_.sky_rot[i * 3] * d[0] + p_.sky_rot[i * 3 + 1] * d[1] + p_.sky_rot[i * 3 + 2] * d[2];
    if (e[2] > -1e-4f) return;
    const float ex = (p_.proj[0] * e[0] + p_.proj[2] * e[2]) / -e[2], ey = (p_.proj[1] * e[1] + p_.proj[3] * e[2]) / -e[2];
    mx = (ex * 0.5f + 0.5f) * p_.vp[2] + p_.vp[0] - x;
    my = (ey * 0.5f + 0.5f) * p_.vp[3] + p_.vp[1] - y;
  }
  V4 Mot(float x, float y) const {
    int ix, iy;
    Texel(x, y, ix, iy);
    const float *m = M_.At(ix, iy);
    V4 r{m[0], m[1], m[2], m[3]};
    if (r.z <= 0.0f) {
      Sky(ix + 0.5f, iy + 0.5f, r.x, r.y);
      r.z = r.w = kInf;
    }
    return r;
  }
  float PrevDepth(float x, float y) const {
    int ix, iy;
    Texel(x, y, ix, iy);
    const float z = Pm_.At(ix, iy)[2];
    return z <= 0.0f ? kInf : z;
  }
  void Fetch(const Rgba8Image &im, float x, float y, float *o) const {
    int ix, iy;
    Texel(x, y, ix, iy);
    for (int k = 0; k < 4; ++k) o[k] = im.At(ix, iy, k);
  }
  // GL_LINEAR, GL_CLAMP_TO_EDGE at texture coordinate p / size
  void Bilinear(const Rgba8Image &im, float x, float y, float *o) const {
    const float fx = x - 0.5f, fy = y - 0.5f;
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float ax = fx - (float)x0, ay = fy - (float)y0;
    auto cl = [](int v, int n) { return std::clamp(v, 0, n - 1); };
    const int xa = cl(x0, im.w), xb = cl(x0 + 1, im.w), ya = cl(y0, im.h), yb = cl(y0 + 1, im.h);
    for (int k = 0; k < 4; ++k) {
      const float top = im.At(xa, ya, k) * (1 - ax) + im.At(xb, ya, k) * ax;
      const float bot = im.At(xa, yb, k) * (1 - ax) + im.At(xb, yb, k) * ax;
      o[k] = top * (1 - ay) + bot * ay;
    }
  }
  const Rgba8Image &C_, &Pc_;
  const MotionImage &M_, &Pm_;
  WarpParams p_;
};

} // namespace nwii::runtime::gx::motion
