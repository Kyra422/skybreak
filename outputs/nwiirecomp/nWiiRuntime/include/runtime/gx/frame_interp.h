#pragma once
// INTERP17: frame interpolation core for the live renderer (no GL, no globals).
//
// Ported from the INTERP16 offline prototype (outputs/pc-tests/interp16/frame_interp.h). Design and
// prototype numbers: work/claude-notes/INTERP16-plan.md; live implementation notes: INTERP17.md.
// Used by src/hle/gx/interp_live.h (gx.cpp, the live pipeline) and by the unit test
// outputs/pc-tests/interp17/interp17-test.cpp.
//
// Per cut guest frame:
//  1. RecordFrame walks the pre-decode command list once (on the interpolation worker, in parallel
//     with the decode pool). Per draw it keeps the match keys, the parse-time matrix snapshot (or the
//     skinning bank via the DrawRaw) and a sequence number into a versioned log of the XF/BP state the
//     renderer reads at draw time (matrix rows, normal rows, lights, projection, XF colours, TEV
//     registers). No command or vertex is copied.
//  2. MakePlan matches frame B (= N) against A (= N-1): three keyed passes (strict, geometry, loose),
//     camera delta from static geometry, auto instance re-pairing, cut guards; then precomputes the
//     interpolated matrix of every matched single-matrix draw (camera-separated slerp).
//  3. While the renderer walks B's commands for the in-between frame, PrepareDraw gives per draw the
//     XF/BP region values to use (projection, texture matrices, live slots, lights, fades) and
//     PostConvert re-transforms the converted batch vertices from the decoded model-space positions.
// Unmatched draws, rejected moves and cut frames use B unchanged.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "runtime/gx_command.h"
#include "runtime/gx_state.h"
#include "runtime/gx/fifo_parser.h"

namespace nwii::runtime::gx::interp {

// ------------------------------------------------------------------ maths ----
struct Aff { float m[12]; }; // GX 3x4 row-major: x' = m0 x + m1 y + m2 z + m3

inline Aff AffIdentity() { Aff a{}; a.m[0] = a.m[5] = a.m[10] = 1.0f; return a; }
inline Aff AffMul(const Aff &a, const Aff &b) {
  Aff r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 4; ++j) {
      float v = a.m[i * 4 + 0] * b.m[0 * 4 + j] + a.m[i * 4 + 1] * b.m[1 * 4 + j] + a.m[i * 4 + 2] * b.m[2 * 4 + j];
      if (j == 3) v += a.m[i * 4 + 3];
      r.m[i * 4 + j] = v;
    }
  return r;
}
inline bool AffInverse(const Aff &a, Aff &out) {
  const float *m = a.m;
  const double c00 = (double)m[5] * m[10] - (double)m[6] * m[9];
  const double c01 = (double)m[6] * m[8] - (double)m[4] * m[10];
  const double c02 = (double)m[4] * m[9] - (double)m[5] * m[8];
  const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
  if (std::fabs(det) < 1e-20) return false;
  const double id = 1.0 / det;
  double r[9];
  r[0] = c00 * id;
  r[1] = ((double)m[2] * m[9] - (double)m[1] * m[10]) * id;
  r[2] = ((double)m[1] * m[6] - (double)m[2] * m[5]) * id;
  r[3] = c01 * id;
  r[4] = ((double)m[0] * m[10] - (double)m[2] * m[8]) * id;
  r[5] = ((double)m[2] * m[4] - (double)m[0] * m[6]) * id;
  r[6] = c02 * id;
  r[7] = ((double)m[1] * m[8] - (double)m[0] * m[9]) * id;
  r[8] = ((double)m[0] * m[5] - (double)m[1] * m[4]) * id;
  for (int i = 0; i < 3; ++i) {
    out.m[i * 4 + 0] = (float)r[i * 3 + 0];
    out.m[i * 4 + 1] = (float)r[i * 3 + 1];
    out.m[i * 4 + 2] = (float)r[i * 3 + 2];
    out.m[i * 4 + 3] = (float)-(r[i * 3 + 0] * m[3] + r[i * 3 + 1] * m[7] + r[i * 3 + 2] * m[11]);
  }
  return true;
}
inline void M3Mul(const float a[9], const float b[9], float r[9]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}
inline void M3T(const float a[9], float r[9]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[j * 3 + i];
}
inline void M3FromAff(const Aff &a, float r[9]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i * 3 + j] = a.m[i * 4 + j];
}
inline float Lerp(float a, float b, float t) { return a * (1.0f - t) + b * t; } // t = 1 -> b exactly

struct Quat { double w, x, y, z; };
inline Quat QuatFromRot(const float r[9]) { // r orthonormal, det +1
  Quat q;
  const double tr = (double)r[0] + r[4] + r[8];
  if (tr > 0) {
    const double s = std::sqrt(tr + 1.0) * 2;
    q.w = 0.25 * s; q.x = (r[7] - r[5]) / s; q.y = (r[2] - r[6]) / s; q.z = (r[3] - r[1]) / s;
  } else if (r[0] > r[4] && r[0] > r[8]) {
    const double s = std::sqrt(1.0 + r[0] - r[4] - r[8]) * 2;
    q.w = (r[7] - r[5]) / s; q.x = 0.25 * s; q.y = (r[1] + r[3]) / s; q.z = (r[2] + r[6]) / s;
  } else if (r[4] > r[8]) {
    const double s = std::sqrt(1.0 + r[4] - r[0] - r[8]) * 2;
    q.w = (r[2] - r[6]) / s; q.x = (r[1] + r[3]) / s; q.y = 0.25 * s; q.z = (r[5] + r[7]) / s;
  } else {
    const double s = std::sqrt(1.0 + r[8] - r[0] - r[4]) * 2;
    q.w = (r[3] - r[1]) / s; q.x = (r[2] + r[6]) / s; q.y = (r[5] + r[7]) / s; q.z = 0.25 * s;
  }
  const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  q.w /= n; q.x /= n; q.y /= n; q.z /= n;
  return q;
}
inline void RotFromQuat(const Quat &q, float r[9]) {
  const double w = q.w, x = q.x, y = q.y, z = q.z;
  r[0] = (float)(1 - 2 * (y * y + z * z)); r[1] = (float)(2 * (x * y - z * w)); r[2] = (float)(2 * (x * z + y * w));
  r[3] = (float)(2 * (x * y + z * w)); r[4] = (float)(1 - 2 * (x * x + z * z)); r[5] = (float)(2 * (y * z - x * w));
  r[6] = (float)(2 * (x * z - y * w)); r[7] = (float)(2 * (y * z + x * w)); r[8] = (float)(1 - 2 * (x * x + y * y));
}
inline Quat Slerp(Quat a, Quat b, double t) {
  double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
  if (d < 0) { b.w = -b.w; b.x = -b.x; b.y = -b.y; b.z = -b.z; d = -d; }
  double ka, kb;
  if (d > 0.9995) { ka = 1 - t; kb = t; }
  else {
    const double th = std::acos(d), s = std::sin(th);
    ka = std::sin((1 - t) * th) / s; kb = std::sin(t * th) / s;
  }
  Quat q{a.w * ka + b.w * kb, a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb};
  const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  q.w /= n; q.x /= n; q.y /= n; q.z /= n;
  return q;
}
inline double QuatAngleDeg(const Quat &q) { return 2.0 * std::acos(std::min(1.0, std::fabs(q.w))) * 57.29577951308232; }
// m = R * diag(s) (scaled columns, a mirror folded into s[0]). False with shear / degenerate.
inline bool DecomposeRS(const float m[9], float r[9], float s[3]) {
  for (int j = 0; j < 3; ++j) {
    s[j] = std::sqrt(m[j] * m[j] + m[3 + j] * m[3 + j] + m[6 + j] * m[6 + j]);
    if (s[j] < 1e-12f) return false;
  }
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i * 3 + j] = m[i * 3 + j] / s[j];
  const float det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) +
                    r[2] * (r[3] * r[7] - r[4] * r[6]);
  if (det < 0) { s[0] = -s[0]; r[0] = -r[0]; r[3] = -r[3]; r[6] = -r[6]; }
  for (int a = 0; a < 3; ++a)
    for (int b = a + 1; b < 3; ++b) {
      const float d = r[a] * r[b] + r[3 + a] * r[3 + b] + r[6 + a] * r[6 + b];
      if (std::fabs(d) > 2e-3f) return false;
    }
  return true;
}
// 3x3 linear part: rotation slerp + scale lerp when both are R*S, element-wise otherwise.
inline void InterpM3(const float a[9], const float b[9], float t, float out[9]) {
  float ra[9], rb[9], sa[3], sb[3];
  if (DecomposeRS(a, ra, sa) && DecomposeRS(b, rb, sb) && (sa[0] > 0) == (sb[0] > 0)) {
    float r[9];
    RotFromQuat(Slerp(QuatFromRot(ra), QuatFromRot(rb), t), r);
    const float s[3] = {Lerp(sa[0], sb[0], t), Lerp(sa[1], sb[1], t), Lerp(sa[2], sb[2], t)};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) out[i * 3 + j] = r[i * 3 + j] * s[j];
    return;
  }
  for (int i = 0; i < 9; ++i) out[i] = Lerp(a[i], b[i], t);
}
// Texture matrix rows: element-wise; a translation that jumped by ~an integer (scroll wrap) is unwrapped.
inline void InterpTex(const float *a, const float *b, int n, float t, float *out, bool unwrap = true) {
  for (int i = 0; i < n; ++i) {
    float av = a[i];
    if (unwrap && i % 4 == 3) {
      const float d = b[i] - av, r = std::round(d);
      if (std::fabs(r) >= 1.0f && std::fabs(d - r) < 0.25f) av += r;
    }
    out[i] = Lerp(av, b[i], t);
  }
}

// ---------------------------------------------------------------- hashing ----
// 64-bit word hash (match keys only: a collision just pairs two draws that the guards then judge).
struct H64 {
  uint64_t h = 0x243F6A8885A308D3ull;
  uint64_t acc = 0;
  int na = 0;
  void w64(uint64_t v) { h ^= v; h *= 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
  void flush() { if (na) { w64(acc ^ ((uint64_t)na << 56)); acc = 0; na = 0; } }
  void u8(uint8_t b) { acc = (acc << 8) | b; if (++na == 7) flush(); }
  void u32(uint32_t v) { flush(); w64(v); }
  void bytes(const uint8_t *p, size_t n) {
    flush();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; std::memcpy(&v, p + i, 8); w64(v); }
    uint64_t t = 0xA5;
    for (; i < n; ++i) t = (t << 8) | p[i];
    w64(t ^ (uint64_t)n);
  }
  uint64_t get() { flush(); return h; }
};
inline void HashVat(H64 &h, const VATSlot &v) {
  h.u8(v.posMatIdx); for (int i = 0; i < 8; ++i) h.u8(v.texMatIdx[i]);
  h.u8((uint8_t)v.posMask); h.u8((uint8_t)v.posType); h.u8(v.posShift); h.u8(v.posElements);
  h.u8((uint8_t)v.nrmMask); h.u8((uint8_t)v.nrmType); h.u8(v.nrmElements); h.u8(v.nrmIndex3);
  for (int i = 0; i < 2; ++i) { h.u8((uint8_t)v.clrMask[i]); h.u8((uint8_t)v.clrType[i]); h.u8(v.clrElements[i]); }
  for (int i = 0; i < 8; ++i) { h.u8((uint8_t)v.texMask[i]); h.u8((uint8_t)v.texType[i]); h.u8(v.texShift[i]); h.u8(v.texElements[i]); }
}
inline bool Indexed(VtxAttrMask m) { return m == VtxAttrMask::Index8 || m == VtxAttrMask::Index16; }
inline uint64_t Combine(uint64_t a, uint64_t b, uint64_t c = 0) { H64 h; h.w64(a); h.w64(b); h.w64(c); return h.get(); }

// ------------------------------------------------------- versioned state ----
// Rows 0-29 hold the position matrices (GX_PNMTX0-9), reloaded before nearly every draw; only "live" draws
// (no parse-time snapshot) read them at render time, so RecordFrame versions them (and the normal rows) at
// those draws only. Texture matrices (rows 30+) are versioned at every write.
constexpr int kTexRow0 = 30;
// Regions of the render-time XF/BP state an in-between draw may need from A or B.
enum : int {
  kRegPos = 0,    // 64 rows of XF matrix memory 0x000-0x0FF (4 floats)
  kRegNrm = 64,   // 32 rows of the normal matrix memory 0x400-0x45F (3 floats)
  kRegLight = 96, // 8 lights 0x600-0x67F (16 floats)
  kRegProj = 104, // projection 0x1020-0x1026 (7 floats)
  kRegXfCol = 105,// XF ambient/material colours 0x100A-0x100D (4 raw words)
  kRegTev = 106,  // TEV colour regs [0..7] + konst regs [8..15] (raw BP values, renderer banks)
  kRegCount = 107
};
inline int RegFloats(int r) {
  return r < kRegNrm ? 4 : r < kRegLight ? 3 : r < kRegProj ? 16 : r == kRegProj ? 7 : r == kRegXfCol ? 4 : 16;
}

// The renderer state at the start of a frame (g_state after the previous real frame).
struct BaseState {
  float pos[256], nrm[96], lights[128], proj[7];
  uint32_t xf[256], bp[256], tev[16];
};
inline void CaptureBase(const GXState &st, BaseState &b) {
  std::memcpy(b.pos, st.posMatrices, sizeof b.pos);
  std::memcpy(b.nrm, st.normalMatrices, sizeof b.nrm);
  std::memcpy(b.lights, st.lights, sizeof b.lights);
  std::memcpy(b.proj, st.projection, sizeof b.proj);
  std::memcpy(b.xf, st.xf, sizeof b.xf);
  std::memcpy(b.bp, st.bp, sizeof b.bp);
  std::memcpy(b.tev, st.tevColorReg, 32);
  std::memcpy(b.tev + 8, st.tevKonstReg, 32);
}

enum : uint8_t { kLive = 0, kSnap = 1, kBank = 2 };
struct DrawRec {
  uint32_t ci = 0;  // index of the DrawPrimitive in the frame's command list
  uint32_t seq = 0; // state version at the draw (FrameRec::At)
  uint64_t kgeom = 0, kmat = 0, ktex = 0, kloose = 0, kstrict = 0;
  Aff mv{};
  float nrm[9]{};
  std::shared_ptr<DrawRaw> bank; // kBank: owns snap_pos_bank / snap_nrm_bank
  uint32_t v_proj = 0, v_tev = 0, v_xfcol = 0, v_light[8] = {}; // version indices (FrameRec::Get)
  uint8_t kind = kLive, prim = 0, lightmask = 0;
  uint16_t count = 0;
  bool ortho = false, direct_pos = false, bpmask_pending = false, keyed = false;
  bool pervtx_tex = false; // the VAT carries per-vertex texture-matrix indices
  uint8_t ntg = 0;          // XF numTexGens at the draw
  uint64_t tex_slots = 0;   // texture-matrix slots its texgens read (defTexMtxIdx; !pervtx_tex)
  uint64_t bank_hash = 0;   // kBank: hash of the frozen matrix bank (equal banks share interpolated slots)
  int32_t match = -1; // (on B) index into A.draws
  uint8_t tier = 0;
};
struct Ver { uint32_t seq, off; };
struct FrameRec {
  uint64_t frame = 0;
  std::vector<DrawRec> draws;
  std::vector<float> pool;          // region contents (raw words for kRegXfCol / kRegTev)
  std::vector<Ver> hist[kRegCount]; // per region: (sequence, pool offset); entry 0 = frame start
  mutable size_t cur[kRegCount] = {};
  double us_hash = 0, us_walk = 0; // RecordFrame phases (diagnostics)
  // Index of the version of region r in force at sequence seq (cursor: monotonic queries are O(1)).
  size_t VerIndex(int r, uint32_t seq) const {
    const std::vector<Ver> &h = hist[r];
    size_t i = cur[r] < h.size() ? cur[r] : 0;
    if (h[i].seq > seq) {
      i = (size_t)(std::upper_bound(h.begin(), h.begin() + (ptrdiff_t)i, seq,
                                    [](uint32_t s, const Ver &v) { return s < v.seq; }) - h.begin()) - 1;
    } else {
      int steps = 0;
      while (i + 1 < h.size() && h[i + 1].seq <= seq) {
        if (++steps > 8) {
          i = (size_t)(std::upper_bound(h.begin() + (ptrdiff_t)i, h.end(), seq,
                                        [](uint32_t s, const Ver &v) { return s < v.seq; }) - h.begin()) - 1;
          break;
        }
        ++i;
      }
    }
    cur[r] = i;
    return i;
  }
  const float *At(int r, uint32_t seq) const { return &pool[hist[r][VerIndex(r, seq)].off]; }
  const float *Get(int r, uint32_t vi) const { return &pool[hist[r][vi].off]; }
  // binary search without the cursor (thread-safe; the plan's parallel loop)
  const float *AtFixed(int r, uint32_t seq) const {
    const std::vector<Ver> &h = hist[r];
    const size_t i = (size_t)(std::upper_bound(h.begin(), h.end(), seq,
                                               [](uint32_t s, const Ver &v) { return s < v.seq; }) - h.begin()) - 1;
    return &pool[h[i].off];
  }
  void Clear() {
    draws.clear();
    pool.clear();
    for (auto &h : hist) h.clear();
    for (auto &c : cur) c = 0;
  }
};

// BP registers in the material key (shader/blend/texture setup) and the texture key (image addresses).
inline bool BpInMat(uint32_t r) {
  return r == 0x00 || (r >= 0x28 && r <= 0x2F) || (r >= 0xC0 && r <= 0xDF) || r == 0xF3 || (r >= 0xF6 && r <= 0xFD) ||
         (r >= 0x40 && r <= 0x43) || (r >= 0x80 && r <= 0x83) || (r >= 0xA0 && r <= 0xA3) ||
         (r >= 0x88 && r <= 0x8B) || (r >= 0xA8 && r <= 0xAB);
}
inline bool BpInTex(uint32_t r) { return (r >= 0x94 && r <= 0x97) || (r >= 0xB4 && r <= 0xB7); }
inline bool XfInMat(uint32_t r) { return r == 0x09 || (r >= 0x0E && r <= 0x11) || (r >= 0x3F && r <= 0x47) || (r >= 0x50 && r <= 0x57); }

// Geometry keys of one draw: out[0] = kgeom (prim, count, VAT, indexed array bases/strides, default
// matrix indices, vertex bytes), out[1] = the same without the vertex bytes (loose key part), out[2] = a hash
// of the skinning bank (0 without one).
inline void GeometryKeys(uint8_t prim, const DrawRaw &r, uint64_t out[2]) {
  H64 g;
  g.u8(prim);
  g.u32(r.count);
  HashVat(g, r.vat);
  const VtxAttrMask masks[12] = {r.vat.posMask, r.vat.nrmMask, r.vat.clrMask[0], r.vat.clrMask[1],
                                 r.vat.texMask[0], r.vat.texMask[1], r.vat.texMask[2], r.vat.texMask[3],
                                 r.vat.texMask[4], r.vat.texMask[5], r.vat.texMask[6], r.vat.texMask[7]};
  for (int q = 0; q < 12; ++q)
    if (Indexed(masks[q])) { g.u32(r.arrayBase[q]); g.u32(r.arrayStride[q]); }
  g.u8(r.defPosMtxIdx);
  for (int q = 0; q < 8; ++q) g.u8(r.defTexMtxIdx[q]);
  H64 lo = g;
  g.bytes(r.bytes.data(), r.bytes.size());
  out[0] = g.get();
  out[1] = lo.get();
  out[2] = 0;
  if (!r.snap_pos_bank.empty()) {
    H64 b;
    b.bytes(reinterpret_cast<const uint8_t *>(r.snap_pos_bank.data()), r.snap_pos_bank.size() * 4);
    b.bytes(reinterpret_cast<const uint8_t *>(r.snap_nrm_bank.data()), r.snap_nrm_bank.size() * 4);
    out[2] = b.get() | 1; // never 0
  }
}
// Optional pre-scan of the frame, built by the caller while it collects the raws: the indices of the XF, BP and
// draw commands (draws: bit 31 set) and the draws' primitive types. With it the record never reads the draw
// commands, which the decode pool writes at the same time (their vertex vectors share a cache line with the
// command header).
struct FrameScan {
  std::vector<uint32_t> idx;
  std::vector<uint8_t> prim; // per draw
  void Clear() { idx.clear(); prim.clear(); }
};
// Optional parallel runner: par(n, grain, fn) calls fn(begin, end) over [0, n) (e.g. a thread pool).
using ParallelRun = std::function<void(size_t, size_t, const std::function<void(size_t, size_t)> &)>;

// Records one cut frame (pre-decode command list). raws[k] = the DrawRaw of the frame's k-th
// DrawPrimitive, taken before decode drops it (null = not keyable: the draw never matches).
// par: the per-draw geometry hashes run in parallel first (the walk itself is sequential).
inline void RecordFrame(const std::vector<GXCommand> &frame, const std::vector<std::shared_ptr<DrawRaw>> &raws,
                        const BaseState &base, uint64_t frame_no, FrameRec &R, const ParallelRun *par = nullptr,
                        const FrameScan *scan = nullptr) {
  std::vector<uint64_t> geo_buf;
  const uint64_t *geo = nullptr;
  const auto now_us = [] {
    return (double)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  const double h0 = now_us();
  if (par && raws.size() > 256) {
    std::vector<uint8_t> prims;
    if (scan) prims = scan->prim;
    else {
      prims.reserve(raws.size());
      for (const auto &c : frame)
        if (c.type == GXCommandType::DrawPrimitive) prims.push_back(c.prim_type);
    }
    if (prims.size() == raws.size()) {
      geo_buf.assign(raws.size() * 3, 0);
      (*par)(raws.size(), 256, [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i)
          if (raws[i]) GeometryKeys(prims[i], *raws[i], &geo_buf[3 * i]);
      });
      geo = geo_buf.data();
    }
  }
  const double h1 = now_us();
  R.Clear();
  R.us_hash = h1 - h0;
  R.frame = frame_no;
  R.draws.reserve(raws.size());
  R.pool.reserve(4096 + raws.size() * 8);
  BaseState s = base; // the walking mirror
  uint32_t stamp[kRegCount] = {};
  uint32_t touched[kRegCount];
  int ntouched = 0;
  uint32_t cmd_id = 1;
  const auto push = [&](int r, uint32_t seq) {
    const uint32_t off = (uint32_t)R.pool.size();
    const int n = RegFloats(r);
    R.pool.resize(off + (size_t)n);
    float *dst = &R.pool[off];
    if (r < kRegNrm) std::memcpy(dst, &s.pos[r * 4], 16);
    else if (r < kRegLight) std::memcpy(dst, &s.nrm[(r - kRegNrm) * 3], 12);
    else if (r < kRegProj) std::memcpy(dst, &s.lights[(r - kRegLight) * 16], 64);
    else if (r == kRegProj) std::memcpy(dst, s.proj, 28);
    else if (r == kRegXfCol) std::memcpy(dst, &s.xf[0x0A], 16);
    else std::memcpy(dst, s.tev, 64);
    R.hist[r].push_back({seq, off});
  };
  const auto touch = [&](int r) {
    if (stamp[r] != cmd_id) { stamp[r] = cmd_id; touched[ntouched++] = (uint32_t)r; }
  };
  for (int r = 0; r < kRegCount; ++r) push(r, 0);
  uint32_t seq = 0, bpmask = 0xFFFFFF;
  // material / texture keys: XOR of a mixed (register, value) word per register in the set, updated
  // incrementally on every change (the game rewrites materials per draw; a full rehash per draw was the
  // record's biggest cost)
  const auto mixk = [](uint64_t id, uint32_t v) {
    uint64_t x = (id << 32) ^ v ^ 0x9E3779B97F4A7C15ull;
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27; x *= 0x94D049BB133111EBull; x ^= x >> 31;
    return x;
  };
  uint64_t kmat = 0, ktex = 0;
  for (uint32_t r = 0; r < 256; ++r) {
    if (BpInMat(r)) kmat ^= mixk(r, s.bp[r]);
    if (BpInTex(r)) ktex ^= mixk(r, s.bp[r]);
    if (XfInMat(r)) kmat ^= mixk(0x1000 + r, s.xf[r]);
  }
  size_t k = 0;
  const size_t nev = scan ? scan->idx.size() : frame.size();
  for (size_t ei = 0; ei < nev; ++ei) {
    size_t ci = ei;
    bool is_draw;
    uint8_t prim = 0;
    if (scan) {
      ci = scan->idx[ei] & 0x7FFFFFFFu;
      is_draw = (scan->idx[ei] >> 31) != 0;
      if (is_draw) prim = k < scan->prim.size() ? scan->prim[k] : 0;
    } else {
      is_draw = frame[ci].type == GXCommandType::DrawPrimitive;
      if (is_draw) prim = frame[ci].prim_type;
    }
    const GXCommand &c = frame[ci]; // header only read for XF / BP commands
    if (!is_draw && c.type == GXCommandType::XFRegister) {
      ++cmd_id;
      ntouched = 0;
      const int addr = c.reg;
      const int np = (int)c.payload.size();
      // matrix loads (the bulk of the XF traffic): block copy + row touches
      if (np && addr + np - 1 <= 0xFF) {
        std::memcpy(&s.pos[addr], c.payload.data(), (size_t)np * 4);
        for (int r = std::max(addr >> 2, kTexRow0); r <= (addr + np - 1) >> 2; ++r) touch(r);
      } else if (np && addr >= 0x400 && addr + np - 1 <= 0x45F) {
        std::memcpy(&s.nrm[addr - 0x400], c.payload.data(), (size_t)np * 4); // versioned at live draws only
      } else if (np && addr >= 0x600 && addr + np - 1 <= 0x67F) {
        std::memcpy(&s.lights[addr - 0x600], c.payload.data(), (size_t)np * 4);
        for (int r = (addr - 0x600) >> 4; r <= (addr - 0x600 + np - 1) >> 4; ++r) touch(kRegLight + r);
      } else
      for (size_t i = 0; i < c.payload.size(); ++i) {
        const int a = addr + (int)i;
        const float v = c.payload[i];
        if (a >= 0x1000 && a < 0x1100) {
          uint32_t u;
          std::memcpy(&u, &v, 4);
          if (u != s.xf[a - 0x1000] && XfInMat((uint32_t)(a - 0x1000)))
            kmat ^= mixk(0x1000 + (a - 0x1000), s.xf[a - 0x1000]) ^ mixk(0x1000 + (a - 0x1000), u);
          s.xf[a - 0x1000] = u;
          if (a >= 0x100A && a <= 0x100D) touch(kRegXfCol);
        }
        if (a >= 0 && a <= 0xFF) { s.pos[a] = v; if ((a >> 2) >= kTexRow0) touch(a >> 2); }
        else if (a >= 0x400 && a <= 0x45F) { s.nrm[a - 0x400] = v; }
        else if (a >= 0x600 && a <= 0x67F) { s.lights[a - 0x600] = v; touch(kRegLight + ((a - 0x600) >> 4)); }
        else if (a >= 0x1020 && a <= 0x1026) { s.proj[a - 0x1020] = v; touch(kRegProj); }
      }
      if (ntouched) {
        ++seq;
        for (int i = 0; i < ntouched; ++i) push((int)touched[i], seq);
      }
    } else if (!is_draw && c.type == GXCommandType::BPRegister) {
      const uint32_t reg = c.reg & 0xFF;
      const uint32_t val = ((s.bp[reg] & ~bpmask) | (c.val & bpmask)) & 0xFFFFFF; // renderer MaskedBP
      const bool changed = val != s.bp[reg];
      if (changed && BpInMat(reg)) kmat ^= mixk(reg, s.bp[reg]) ^ mixk(reg, val);
      if (changed && BpInTex(reg)) ktex ^= mixk(reg, s.bp[reg]) ^ mixk(reg, val);
      s.bp[reg] = val;
      if (reg == 0xFE) { bpmask = val; continue; }
      bpmask = 0xFFFFFF;
      if (reg >= 0xE0 && reg <= 0xE7) {
        s.tev[((val & 0x800000) ? 8 : 0) + (reg - 0xE0)] = val;
        ++seq;
        push(kRegTev, seq);
      }
    } else if (is_draw) {
      const DrawRaw *r = k < raws.size() ? raws[k].get() : nullptr;
      ++k;
      DrawRec d;
      d.ci = (uint32_t)ci;
      d.seq = seq;
      d.prim = prim;
      uint32_t pt;
      std::memcpy(&pt, &s.proj[6], 4);
      d.ortho = pt != 0;
      d.bpmask_pending = bpmask != 0xFFFFFF;
      d.v_proj = (uint32_t)R.hist[kRegProj].size() - 1;
      d.v_tev = (uint32_t)R.hist[kRegTev].size() - 1;
      d.v_xfcol = (uint32_t)R.hist[kRegXfCol].size() - 1;
      for (int l = 0; l < 8; ++l) d.v_light[l] = (uint32_t)R.hist[kRegLight + l].size() - 1;
      const uint32_t nch = std::min<uint32_t>(s.xf[0x09] & 3, 2);
      for (uint32_t ch = 0; ch < nch; ++ch)
        for (uint32_t ctl : {s.xf[0x0E + ch], s.xf[0x10 + ch]})
          if ((ctl >> 1) & 1) d.lightmask |= (uint8_t)(((ctl >> 2) & 0xF) | (((ctl >> 11) & 0xF) << 4));
      d.kmat = kmat ^ (d.ortho ? 0x5DEECE66Dull : 0); // 2D and 3D never share a material key
      d.ktex = ktex;
      // a live draw reads the position/normal matrix memory at the draw: version those rows now (the matrix
      // loads of the snapshot draws in between are not logged, see kTexRow0)
      const auto version_live_rows = [&](DrawRec &dr) {
        ++seq;
        for (int q = 0; q < kTexRow0; ++q) push(q, seq);
        for (int q = 0; q < 32; ++q) push(kRegNrm + q, seq);
        dr.seq = seq;
      };
      if (!r) {
        d.kind = kLive;
        version_live_rows(d);
        R.draws.push_back(std::move(d));
        continue;
      }
      d.keyed = true;
      d.count = r->count;
      d.direct_pos = r->vat.posMask == VtxAttrMask::Direct;
      for (int q = 0; q < 8; ++q) d.pervtx_tex = d.pervtx_tex || r->vat.texMatIdx[q];
      uint64_t hg[3];
      if (geo) { hg[0] = geo[3 * (k - 1)]; hg[1] = geo[3 * (k - 1) + 1]; hg[2] = geo[3 * (k - 1) + 2]; }
      else GeometryKeys(prim, *r, hg);
      d.bank_hash = hg[2];
      d.ntg = (uint8_t)std::min<uint32_t>(s.xf[0x3F] & 0xF, 8);
      if (!d.pervtx_tex)
        for (uint32_t q = 0; q < d.ntg; ++q) d.tex_slots |= 1ull << (r->defTexMtxIdx[q] & 0x3F);
      d.kgeom = hg[0];
      d.kloose = Combine(hg[1], kmat, ktex);
      d.kstrict = Combine(d.kgeom, kmat, ktex);
      if (!r->snap_pos_bank.empty()) {
        d.kind = kBank;
        d.bank = raws[k - 1];
      } else if (r->have_snap_mtx) {
        d.kind = kSnap;
        std::memcpy(d.mv.m, r->snap_pos_mtx, sizeof d.mv.m);
        std::memcpy(d.nrm, r->snap_nrm_mtx, sizeof d.nrm);
      } else {
        d.kind = kLive;
        version_live_rows(d);
        const uint32_t pb = (uint32_t)r->defPosMtxIdx * 4, nb = 3u * (r->defPosMtxIdx & 31);
        if (pb + 12 <= 256) std::memcpy(d.mv.m, &s.pos[pb], sizeof d.mv.m);
        if (nb + 9 <= 96) std::memcpy(d.nrm, &s.nrm[nb], sizeof d.nrm);
      }
      R.draws.push_back(std::move(d));
    }
  }
  R.us_walk = now_us() - h1;
}

// --------------------------------------------------------------- matching ----
struct MatchStats {
  size_t a_draws = 0, b_draws = 0, a_persp = 0, b_persp = 0, b_ortho = 0;
  size_t tier[4] = {0, 0, 0, 0}; // [0] unmatched, [1] strict, [2] geometry, [3] loose
  size_t persp_matched = 0, ortho_matched = 0, refined = 0;
};

// Open-addressing key -> chain head (A draws in order) used by one matching pass.
struct KeyChains {
  std::vector<uint64_t> keys;
  std::vector<int32_t> head;
  std::vector<int32_t> next;
  size_t mask = 0;
  void Reset(size_t n_items, size_t n_a) {
    size_t cap = 64;
    while (cap < n_items * 2 + 16) cap <<= 1;
    keys.assign(cap, 0);
    head.assign(cap, -2); // -2 = empty slot
    next.assign(n_a, -1);
    mask = cap - 1;
  }
  size_t Slot(uint64_t k) const {
    size_t i = (size_t)(k ^ (k >> 31)) & mask;
    while (head[i] != -2 && keys[i] != k) i = (i + 1) & mask;
    return i;
  }
};

// Three keyed passes; each pairs the k-th unmatched B draw with key K to the k-th unmatched A draw with
// key K (equal kind, ortho and count required). ortho_loose = false: 2D draws match on tiers 1-2 only
// (glyph quads of two different words would pair on the loose tier, INTERP16 G3).
// index_only: the naive baseline (B's i-th draw <-> A's i-th draw), the matcher's negative control.
inline void Match(const FrameRec &A, FrameRec &B, MatchStats &st, bool index_only = false, bool ortho_loose = false) {
  st = MatchStats{};
  st.a_draws = A.draws.size();
  for (const auto &d : A.draws) st.a_persp += !d.ortho;
  st.b_draws = B.draws.size();
  std::vector<char> a_used(A.draws.size(), 0);
  for (auto &d : B.draws) { d.match = -1; d.tier = 0; }
  if (index_only) {
    for (size_t i = 0; i < B.draws.size() && i < A.draws.size(); ++i)
      if (A.draws[i].kind == B.draws[i].kind && A.draws[i].ortho == B.draws[i].ortho &&
          A.draws[i].count == B.draws[i].count && A.draws[i].keyed && B.draws[i].keyed) {
        a_used[i] = 1;
        B.draws[i].match = (int)i;
        B.draws[i].tier = 1;
      }
  } else {
    static thread_local KeyChains kc; // reused: no large allocations per frame
    for (int tier = 1; tier <= 3; ++tier) {
      const auto key = [tier](const DrawRec &d) { return tier == 1 ? d.kstrict : tier == 2 ? d.kgeom : d.kloose; };
      size_t na = 0;
      for (size_t i = 0; i < A.draws.size(); ++i) na += !a_used[i] && A.draws[i].keyed;
      if (!na) break;
      kc.Reset(na, A.draws.size());
      for (size_t ii = A.draws.size(); ii-- > 0;) {
        if (a_used[ii] || !A.draws[ii].keyed) continue;
        const uint64_t k = key(A.draws[ii]);
        const size_t sl = kc.Slot(k);
        if (kc.head[sl] == -2) { kc.keys[sl] = k; kc.head[sl] = -1; }
        kc.next[ii] = kc.head[sl];
        kc.head[sl] = (int32_t)ii;
      }
      for (auto &d : B.draws) {
        if (d.match >= 0 || !d.keyed) continue;
        if (tier == 3 && d.ortho && !ortho_loose) continue;
        const uint64_t k = key(d);
        const size_t sl = kc.Slot(k);
        int32_t &h = kc.head[sl];
        if (h == -2) continue;
        while (h >= 0 && a_used[h]) h = kc.next[h];
        if (h < 0) continue;
        const int32_t ai = h;
        h = kc.next[h]; // the candidate is consumed even if it does not fit (as the prototype)
        const DrawRec &a = A.draws[ai];
        if (a.kind != d.kind || a.ortho != d.ortho || a.count != d.count) continue;
        a_used[ai] = 1;
        d.match = ai;
        d.tier = (uint8_t)tier;
      }
    }
  }
  for (const auto &d : B.draws) {
    st.tier[d.match >= 0 ? d.tier : 0]++;
    if (d.ortho) { st.b_ortho++; st.ortho_matched += d.match >= 0; }
    else { st.b_persp++; st.persp_matched += d.match >= 0; }
  }
}
inline void Recount(const FrameRec &A, const FrameRec &B, MatchStats &st) {
  const size_t refined = st.refined;
  st = MatchStats{};
  st.a_draws = A.draws.size();
  for (const auto &d : A.draws) st.a_persp += !d.ortho;
  st.b_draws = B.draws.size();
  for (const auto &d : B.draws) {
    st.tier[d.match >= 0 ? d.tier : 0]++;
    if (d.ortho) { st.b_ortho++; st.ortho_matched += d.match >= 0; }
    else { st.b_persp++; st.persp_matched += d.match >= 0; }
  }
  st.refined = refined;
}

// ----------------------------------------------------------------- camera ----
// dV = V_B * V_A^-1 voted by matched perspective single-matrix draws (stage geometry shares one view).
struct Camera {
  bool ok = false;
  Aff dV = AffIdentity(), dVinv = AffIdentity();
  Quat dq{1, 0, 0, 0};
  float dR[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  float angle_deg = 0, trans = 0;
  int inliers = 0, candidates = 0;
};
inline bool AffNear(const Aff &a, const Aff &b, float rot_eps, float tr_eps) {
  for (int i = 0; i < 12; ++i) {
    const float e = (i % 4 == 3) ? tr_eps * (1.0f + std::fabs(a.m[i])) : rot_eps;
    if (std::fabs(a.m[i] - b.m[i]) > e) return false;
  }
  return true;
}
inline Camera EstimateCamera(const FrameRec &A, const FrameRec &B) {
  Camera cam;
  std::vector<int> cand;
  cand.reserve(B.draws.size());
  for (int i = 0; i < (int)B.draws.size(); ++i) {
    const DrawRec &d = B.draws[i];
    if (d.match >= 0 && !d.ortho && d.kind != kBank) cand.push_back(i);
  }
  if (cand.empty()) return cam;
  // the vote and the average run on an evenly strided subsample of <= 2048 deltas
  const size_t sstep = std::max<size_t>(1, cand.size() / 2048);
  std::vector<Aff> D;
  D.reserve(cand.size() / sstep + 1);
  for (size_t i = 0; i < cand.size(); i += sstep) {
    const DrawRec &d = B.draws[cand[i]];
    Aff ainv;
    if (!AffInverse(A.draws[d.match].mv, ainv)) continue;
    D.push_back(AffMul(d.mv, ainv));
  }
  cam.candidates = (int)D.size();
  if (D.empty()) return cam;
  // up to 48 evenly spread hypotheses voted on <= 512 of the deltas; a hypothesis that already has more
  // than half of the votes is the static world's camera (the usual case): stop there
  const size_t vstep = std::max<size_t>(1, D.size() / 512);
  const size_t hstep = std::max<size_t>(1, D.size() / 48);
  size_t best = 0;
  int best_n = -1, voters = 0;
  for (size_t i = 0; i < D.size(); i += vstep) ++voters;
  for (size_t h = 0; h < D.size(); h += hstep) {
    int n = 0;
    for (size_t i = 0; i < D.size(); i += vstep) n += AffNear(D[i], D[h], 2e-3f, 2e-3f);
    if (n > best_n) { best_n = n; best = h; }
    if (2 * n > voters) break;
  }
  Aff avg{};
  int n = 0;
  for (const auto &x : D)
    if (AffNear(x, D[best], 2e-3f, 2e-3f)) { for (int i = 0; i < 12; ++i) avg.m[i] += x.m[i]; ++n; }
  for (int i = 0; i < 12; ++i) avg.m[i] /= (float)n;
  cam.inliers = n;
  float r[9], rr[9], s[3];
  M3FromAff(avg, r);
  if (!DecomposeRS(r, rr, s) || std::fabs(s[0] - 1) > 0.02f || std::fabs(s[1] - 1) > 0.02f || std::fabs(s[2] - 1) > 0.02f)
    return cam; // not a rigid camera move
  cam.dq = QuatFromRot(rr);
  RotFromQuat(cam.dq, cam.dR); // re-orthonormalised
  cam.dV = avg;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) cam.dV.m[i * 4 + j] = cam.dR[i * 3 + j];
  AffInverse(cam.dV, cam.dVinv);
  cam.angle_deg = (float)QuatAngleDeg(cam.dq);
  cam.trans = std::sqrt(avg.m[3] * avg.m[3] + avg.m[7] * avg.m[7] + avg.m[11] * avg.m[11]);
  cam.ok = n >= 3;
  return cam;
}
// dV_t = [dR^t | t * dR^(t-1) * T]  (camera rotation slerped, eye lerped)
inline Aff CameraAt(const Camera &c, float t) {
  float rt[9], rtm1[9], dRT[9];
  RotFromQuat(Slerp(Quat{1, 0, 0, 0}, c.dq, t), rt);
  M3T(c.dR, dRT);
  M3Mul(rt, dRT, rtm1);
  Aff o;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) o.m[i * 4 + j] = rt[i * 3 + j];
    o.m[i * 4 + 3] = t * (rtm1[i * 3 + 0] * c.dV.m[3] + rtm1[i * 3 + 1] * c.dV.m[7] + rtm1[i * 3 + 2] * c.dV.m[11]);
  }
  return o;
}

// ----------------------------------------------------------------- config ----
struct Config {
  bool camera_mode = true;     // camera-separated slerp (false = element-wise lerp, negative control)
  bool interp_2d = true;       // 2D layers interpolated (with the 2D jump guard)
  bool vertex_lerp = true;     // same-count draws whose model-space vertices changed
  bool vertex_color_lerp = false;
  bool lights = true, texmtx = true, tex_unwrap = true, projection = true;
  bool values = true;          // TEV colour/konst + XF ambient/material colours (fades)
  bool refine_instances = true, refine_auto = true;
  bool ortho_loose = false;    // 2D strict (tiers 1-2)
  bool index_only = false;     // naive index pairing (negative control)
  bool guards = true;          // cut / jump guards (false = negative control)
  bool kmode = true;           // the camera delta rides on the projection uniform (K): static-in-world draws need
                               // no CPU vertex work; other perspective draws carry K^-1 (see PostConvert)
  int nn_window = 24;
  float max_rel_move = 0.35f;  // reject a match that moved > this * its distance to the camera
  float max_2d_move = 0.25f;   // 2D: reject a layer that moved > this * the ortho view
  float cut_angle_deg = 50.0f;
  float cut_min_matched = 0.4f;
  float cut_spike = 4.0f, cut_trans_floor = 10.0f, cut_angle_floor = 3.0f;
};

// Equal-strict-key instances (particles, foliage, rays): ordinal pairing breaks when instances are
// born/culled; auto mode keeps the ordinal pairs unless re-pairing by the full matrix (B taken back
// into A's camera space; translation distance + linear-part distance) moves the group clearly more
// coherently (3x4-delta variance < half). INTERP16 K3/K4.
inline float PairDispersion(const FrameRec &A, const FrameRec &B, const Camera &cam, const int *bl, size_t nb, int n_min) {
  float sum[12] = {}, sq = 0;
  int n = 0;
  for (size_t q = 0; q < nb; ++q) {
    const DrawRec &db = B.draws[bl[q]];
    if (db.match < 0) continue;
    const DrawRec &da = A.draws[db.match];
    const Aff wb = cam.ok && !db.ortho ? AffMul(cam.dVinv, db.mv) : db.mv;
    for (int k = 0; k < 12; ++k) { const float d = wb.m[k] - da.mv.m[k]; sum[k] += d; sq += d * d; }
    ++n;
  }
  if (n < n_min) return -1.0f;
  float m2 = 0;
  for (int k = 0; k < 12; ++k) m2 += sum[k] * sum[k];
  m2 /= (float)n * n;
  return std::max(0.0f, sq / n - m2);
}
inline void RefineInstances(const FrameRec &A, FrameRec &B, const Camera &cam, const Config &cfg, MatchStats &ms) {
  // group ids per strict key (kSnap draws of both frames)
  static thread_local KeyChains kc;
  kc.Reset(A.draws.size() + B.draws.size(), 0);
  static thread_local std::vector<int32_t> gid_slot; // slot -> group id
  gid_slot.assign(kc.keys.size(), -1);
  int ngroups = 0;
  std::vector<int32_t> ga(A.draws.size(), -1), gb(B.draws.size(), -1);
  for (size_t i = 0; i < A.draws.size(); ++i) {
    if (A.draws[i].kind != kSnap) continue;
    const size_t sl = kc.Slot(A.draws[i].kstrict);
    if (kc.head[sl] == -2) { kc.head[sl] = -1; kc.keys[sl] = A.draws[i].kstrict; gid_slot[sl] = ngroups++; }
    ga[i] = gid_slot[sl];
  }
  for (size_t i = 0; i < B.draws.size(); ++i) {
    if (B.draws[i].kind != kSnap) continue;
    const size_t sl = kc.Slot(B.draws[i].kstrict);
    if (kc.head[sl] == -2) continue; // no A instance with this key
    gb[i] = gid_slot[sl];
  }
  // flat member lists per group (in draw order)
  std::vector<int32_t> acount(ngroups + 1, 0), bcount(ngroups + 1, 0);
  for (int32_t g : ga) if (g >= 0) acount[g + 1]++;
  for (int32_t g : gb) if (g >= 0) bcount[g + 1]++;
  for (int g = 0; g < ngroups; ++g) { acount[g + 1] += acount[g]; bcount[g + 1] += bcount[g]; }
  std::vector<int32_t> amem(acount[ngroups]), bmem(bcount[ngroups]);
  {
    std::vector<int32_t> fa(acount.begin(), acount.end() - 1), fb(bcount.begin(), bcount.end() - 1);
    for (size_t i = 0; i < ga.size(); ++i) if (ga[i] >= 0) amem[fa[ga[i]]++] = (int32_t)i;
    for (size_t i = 0; i < gb.size(); ++i) if (gb[i] >= 0) bmem[fb[gb[i]]++] = (int32_t)i;
  }
  std::vector<char> a_used(A.draws.size(), 0);
  for (const auto &d : B.draws) if (d.match >= 0) a_used[d.match] = 1;
  struct Cand { float d; int a, b; };
  std::vector<Cand> cand;
  std::vector<int32_t> old;
  std::vector<uint8_t> old_tier;
  for (int g = 0; g < ngroups; ++g) {
    const int32_t *al = &amem[acount[g]], *bl = &bmem[bcount[g]];
    const size_t na = (size_t)(acount[g + 1] - acount[g]), nb = (size_t)(bcount[g + 1] - bcount[g]);
    if (!nb || (na < 2 && nb < 2)) continue;
    if (cfg.refine_auto && na == nb) { // complete ordinal pairs that move coherently: nothing to gain (most groups)
      bool complete = true;
      for (size_t q = 0; q < nb && complete; ++q) {
        const int32_t m = B.draws[bl[q]].match;
        complete = m >= 0 && ga[m] == g;
      }
      if (complete) {
        const float disp = PairDispersion(A, B, cam, bl, nb, 3);
        if (disp >= 0.0f && disp <= 1e-4f) continue;
      }
    }
    old.assign(nb, -1);
    for (size_t q = 0; q < nb; ++q) {
      const int32_t m = B.draws[bl[q]].match;
      old[q] = m;
      if (m >= 0 && ga[m] == g) a_used[m] = 0;
      else if (m >= 0) old[q] = -2; // paired outside the group (tier 2/3): kept
    }
    cand.clear();
    for (size_t kb = 0; kb < nb; ++kb) {
      if (old[kb] == -2) continue;
      const DrawRec &db = B.draws[bl[kb]];
      const Aff wb = cam.ok && !db.ortho ? AffMul(cam.dVinv, db.mv) : db.mv;
      const long exp = (long)std::lround((double)kb * (double)na / (double)nb);
      const long lo = std::max(0L, exp - cfg.nn_window), hi = std::min((long)na - 1, exp + cfg.nn_window);
      for (long ka = lo; ka <= hi; ++ka) {
        const int ai = al[ka];
        if (a_used[ai]) continue; // taken outside the group
        const DrawRec &da = A.draws[ai];
        const float dx = wb.m[3] - da.mv.m[3], dy = wb.m[7] - da.mv.m[7], dz = wb.m[11] - da.mv.m[11];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float depth = std::sqrt(da.mv.m[3] * da.mv.m[3] + da.mv.m[7] * da.mv.m[7] + da.mv.m[11] * da.mv.m[11]);
        if (!db.ortho && dist > cfg.max_rel_move * std::max(depth, 1.0f)) continue;
        float rot = 0;
        if (cfg.refine_auto)
          for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) { const float e = wb.m[i * 4 + j] - da.mv.m[i * 4 + j]; rot += e * e; }
        cand.push_back({dist + std::sqrt(rot) + 1e-4f * (float)std::labs(ka - exp), ai, bl[kb]});
      }
    }
    std::sort(cand.begin(), cand.end(), [](const Cand &x, const Cand &y) { return x.d < y.d; });
    float disp_old = -1.0f;
    if (cfg.refine_auto) {
      disp_old = PairDispersion(A, B, cam, bl, nb, 3);
      old_tier.resize(nb);
      for (size_t q = 0; q < nb; ++q) old_tier[q] = B.draws[bl[q]].tier;
    }
    for (size_t q = 0; q < nb; ++q) if (old[q] != -2) B.draws[bl[q]].match = -1;
    for (const auto &c : cand) {
      if (a_used[c.a] || B.draws[c.b].match >= 0) continue;
      a_used[c.a] = 1;
      B.draws[c.b].match = c.a;
      B.draws[c.b].tier = 1;
    }
    if (cfg.refine_auto && disp_old >= 0.0f) {
      const float disp_new = PairDispersion(A, B, cam, bl, nb, 1);
      if (!(disp_new >= 0.0f && disp_new < 0.5f * disp_old)) { // the ordinal pairs are as coherent: keep them
        for (size_t q = 0; q < nb; ++q) {
          if (old[q] == -2) continue;
          DrawRec &db = B.draws[bl[q]];
          if (db.match >= 0) a_used[db.match] = 0;
        }
        for (size_t q = 0; q < nb; ++q) {
          if (old[q] == -2) continue;
          DrawRec &db = B.draws[bl[q]];
          db.match = old[q];
          db.tier = old_tier[q];
          if (old[q] >= 0) a_used[old[q]] = 1;
        }
      }
    }
    for (size_t q = 0; q < nb; ++q)
      if (old[q] != -2 && B.draws[bl[q]].match != old[q]) ++ms.refined;
  }
  Recount(A, B, ms);
}

// ------------------------------------------------------------------- cuts ----
// prev = the camera of the previous frame pair (spike rule).
inline bool CutCheck(const Config &cfg, const Camera &cam, const MatchStats &ms, const char **reason,
                     const Camera *prev = nullptr) {
  *reason = "";
  if (!cfg.guards) return false;
  // matched share of the LARGER frame: a scene that shrinks (fade to a loading screen) or grows is a cut
  const size_t big = std::max(ms.a_persp, ms.b_persp);
  if (big > 20 && ms.persp_matched < cfg.cut_min_matched * big) { *reason = "few matched draws"; return true; }
  if (!cam.ok && cam.candidates >= 20 && cam.inliers < 0.2f * cam.candidates) { *reason = "no camera consensus"; return true; }
  if (cam.ok && cam.angle_deg > cfg.cut_angle_deg) { *reason = "camera angle"; return true; }
  if (prev && prev->ok && cam.ok) {
    if (cam.trans > cfg.cut_spike * std::max(prev->trans, cfg.cut_trans_floor)) { *reason = "camera jump (spike)"; return true; }
    if (cam.angle_deg > cfg.cut_spike * std::max(prev->angle_deg, cfg.cut_angle_floor)) { *reason = "camera turn (spike)"; return true; }
  }
  return false;
}

// ------------------------------------------------------------------- plan ----
// kDpProj/Tev/XfCol: A and B differ in that state at the draw (precomputed on the worker, so the host's
// per-draw work skips equal state); lights: per-light "position/direction differ" mask.
enum : uint8_t { kDpInterp = 1, kDpStatic = 2, kDpProj = 4, kDpTev = 8, kDpXfCol = 16, kDpTex = 32 };
struct DrawPlan {
  uint8_t flags = 0;
  uint8_t lights = 0;
  Aff mv{};          // kSnap: interpolated position matrix
  float nrm[9]{};    // kSnap: interpolated normal matrix
};
struct Plan {
  bool cut = false;
  const char *cut_reason = "";
  float t = 0.5f;
  MatchStats ms;
  Camera cam;
  Aff Ct = AffIdentity(); // camera delta at t
  float Rt[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  float dRT[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::vector<DrawPlan> dp; // per B draw
  // K mode: K = camera(t) * dV^-1 is post-multiplied onto every perspective projection of the in-between
  // frame, so a static draw keeps B's converted vertices (K * pos_B = Ct * pos_A); every other perspective draw
  // is written as Kinv * pos_I (unmatched ones: Kinv * pos_B) and lands where it should.
  bool kmode = false;
  Aff K = AffIdentity(), Kinv = AffIdentity();
  size_t interpolated = 0, rejected_move = 0, static_draws = 0;
  double us_match = 0, us_cam = 0, us_refine = 0, us_draws = 0; // MakePlan phases (diagnostics)
};
inline double PlanNowUs() {
  return (double)std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Ctx {
  const Config &cfg;
  const Camera &cam;
  float t;
  Aff Ct;
  float Rt[9], dRT[9];
  Ctx(const Config &c, const Camera &k, float tt) : cfg(c), cam(k), t(tt) {
    Ct = CameraAt(k, tt);
    RotFromQuat(Slerp(Quat{1, 0, 0, 0}, k.dq, tt), Rt);
    M3T(k.dR, dRT);
  }
  Ctx(const Config &c, const struct Plan &p); // from the plan's precomputed camera at t (below)
  bool cam_on(bool ortho) const { return cfg.camera_mode && cam.ok && !ortho; }
};
// Object motion is interpolated in A's camera space (rotation slerp + scale lerp, element-wise with
// shear), then viewed with the camera at t. A draw whose B matrix is A's seen by the moved camera
// (static in the world) reduces to Ct * A.
inline Aff InterpMV(const Ctx &C, const Aff &a, const Aff &b, bool ortho, bool *is_static = nullptr) {
  if (C.cam_on(ortho)) {
    const Aff wb = AffMul(C.cam.dVinv, b);
    bool st = true;
    for (int i = 0; i < 12 && st; ++i) st = std::fabs(wb.m[i] - a.m[i]) <= 1e-4f * (1.0f + std::fabs(a.m[i]));
    if (is_static) *is_static = st;
    if (st) return AffMul(C.Ct, a);
    float la[9], lb[9], lo[9];
    M3FromAff(a, la);
    M3FromAff(wb, lb);
    InterpM3(la, lb, C.t, lo);
    Aff w;
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) w.m[i * 4 + j] = lo[i * 3 + j];
      w.m[i * 4 + 3] = Lerp(a.m[i * 4 + 3], wb.m[i * 4 + 3], C.t);
    }
    return AffMul(C.Ct, w);
  }
  if (is_static) *is_static = false;
  Aff o;
  for (int i = 0; i < 12; ++i) o.m[i] = Lerp(a.m[i], b.m[i], C.t);
  return o;
}
inline void InterpNrm(const Ctx &C, const float a[9], const float b[9], bool ortho, bool is_static, float out[9]) {
  if (C.cam_on(ortho)) {
    if (is_static) { M3Mul(C.Rt, a, out); return; }
    float wb[9], w[9];
    M3Mul(C.dRT, b, wb);
    InterpM3(a, wb, C.t, w);
    M3Mul(C.Rt, w, out);
    return;
  }
  for (int i = 0; i < 9; ++i) out[i] = Lerp(a[i], b[i], C.t);
}

inline Ctx::Ctx(const Config &c, const Plan &p) : cfg(c), cam(p.cam), t(p.t), Ct(p.Ct) {
  std::memcpy(Rt, p.Rt, sizeof Rt);
  std::memcpy(dRT, p.dRT, sizeof dRT);
}

// Match -> camera -> instance refinement -> camera again -> cut guards -> per-draw guards and the
// interpolated matrices of single-matrix draws. B is modified (match / tier).
inline void MakePlan(const FrameRec &A, FrameRec &B, float t, const Config &cfg, const Camera *prev, Plan &P,
                     const ParallelRun *par = nullptr) {
  P.t = t;
  P.interpolated = P.rejected_move = P.static_draws = 0;
  const double t0 = PlanNowUs();
  Match(A, B, P.ms, cfg.index_only, cfg.ortho_loose);
  const double t1 = PlanNowUs();
  P.cam = EstimateCamera(A, B);
  const double t2 = PlanNowUs();
  if (cfg.refine_instances && !cfg.index_only) {
    RefineInstances(A, B, P.cam, cfg, P.ms);
    if (P.ms.refined * 100 > B.draws.size()) { // more than 1% of the pairs changed: vote again
      const Camera c2 = EstimateCamera(A, B);
      if (c2.ok) P.cam = c2;
    }
  }
  const double t3 = PlanNowUs();
  P.us_match = t1 - t0;
  P.us_cam = t2 - t1;
  P.us_refine = t3 - t2;
  P.cut = CutCheck(cfg, P.cam, P.ms, &P.cut_reason, prev);
  const Ctx C(cfg, P.cam, t);
  P.Ct = C.Ct;
  std::memcpy(P.Rt, C.Rt, sizeof P.Rt);
  std::memcpy(P.dRT, C.dRT, sizeof P.dRT);
  P.kmode = cfg.kmode && C.cam_on(false);
  P.K = P.Kinv = AffIdentity();
  if (P.kmode) {
    P.K = AffMul(C.Ct, P.cam.dVinv);
    if (!AffInverse(P.K, P.Kinv)) { P.kmode = false; P.K = AffIdentity(); }
  }
  P.dp.assign(B.draws.size(), DrawPlan{});
  if (P.cut) { P.us_draws = PlanNowUs() - t3; return; }
  std::atomic<size_t> n_interp{0}, n_rej{0}, n_static{0};
  const auto body = [&](size_t b, size_t e) {
    size_t ni = 0, nr = 0, ns = 0;
    for (size_t i = b; i < e; ++i) {
      const DrawRec &d = B.draws[i];
      if (d.match < 0 || (d.ortho && !cfg.interp_2d)) continue;
      const DrawRec &a = A.draws[d.match];
      if (cfg.guards && d.kind != kBank && !d.ortho) {
        // object jump (teleport, respawn, state swap): moved > max_rel_move of its distance (A camera space)
        const Aff wb = C.cam_on(false) ? AffMul(P.cam.dVinv, d.mv) : d.mv;
        const float dx = wb.m[3] - a.mv.m[3], dy = wb.m[7] - a.mv.m[7], dz = wb.m[11] - a.mv.m[11];
        const float dist = std::sqrt(a.mv.m[3] * a.mv.m[3] + a.mv.m[7] * a.mv.m[7] + a.mv.m[11] * a.mv.m[11]);
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > cfg.max_rel_move * std::max(dist, 1.0f)) { ++nr; continue; }
      } else if (cfg.guards && d.ortho && d.kind != kBank) {
        // 2D: a layer that jumped more than a quarter of the ortho view (menu cursor, page switch) -> B
        const float *pb = B.Get(kRegProj, d.v_proj);
        const float w = std::fabs(pb[0]) > 1e-12f ? 2.0f / std::fabs(pb[0]) : 1e30f;
        const float h = std::fabs(pb[2]) > 1e-12f ? 2.0f / std::fabs(pb[2]) : 1e30f;
        const float dx = std::fabs(d.mv.m[3] - a.mv.m[3]), dy = std::fabs(d.mv.m[7] - a.mv.m[7]);
        if (dx > cfg.max_2d_move * w || dy > cfg.max_2d_move * h) { ++nr; continue; }
      }
      DrawPlan &p = P.dp[i];
      p.flags = kDpInterp;
      ++ni;
      if (std::memcmp(A.Get(kRegProj, a.v_proj), B.Get(kRegProj, d.v_proj), 28) != 0) p.flags |= kDpProj;
      if (std::memcmp(A.Get(kRegTev, a.v_tev), B.Get(kRegTev, d.v_tev), 64) != 0) p.flags |= kDpTev;
      if (std::memcmp(A.Get(kRegXfCol, a.v_xfcol), B.Get(kRegXfCol, d.v_xfcol), 16) != 0) p.flags |= kDpXfCol;
      for (uint64_t ts = d.tex_slots; ts; ts &= ts - 1) {
        int sl = 0;
        while (!(ts >> sl & 1)) ++sl;
        for (int j = 0; j < 3 && sl + j < 64 && !(p.flags & kDpTex); ++j)
          if (std::memcmp(A.AtFixed(kRegPos + sl + j, a.seq), B.AtFixed(kRegPos + sl + j, d.seq), 16) != 0)
            p.flags |= kDpTex;
      }
      for (int l = 0; l < 8; ++l)
        if ((d.lightmask >> l & 1) &&
            std::memcmp(A.Get(kRegLight + l, a.v_light[l]) + 10, B.Get(kRegLight + l, d.v_light[l]) + 10, 24) != 0)
          p.lights |= (uint8_t)(1u << l);
      if (d.kind == kSnap) {
        bool st = false;
        p.mv = InterpMV(C, a.mv, d.mv, d.ortho, &st);
        InterpNrm(C, a.nrm, d.nrm, d.ortho, st, p.nrm);
        if (st) { p.flags |= kDpStatic; ++ns; }
      }
    }
    n_interp += ni;
    n_rej += nr;
    n_static += ns;
  };
  if (par && B.draws.size() > 1024) (*par)(B.draws.size(), 1024, body);
  else body(0, B.draws.size());
  P.interpolated = n_interp.load();
  P.rejected_move = n_rej.load();
  P.static_draws = n_static.load();
  P.us_draws = PlanNowUs() - t3;
}

// ------------------------------------------------- per-draw interpolation ----
// Region values an in-between draw uses (applied by the caller to the renderer state, deduplicated).
struct RegionWrite { int16_t r; float v[16]; };
struct DrawOverride {
  int nw = 0;
  RegionWrite w[48];
  uint8_t vmode = 0; // 0 none, 1 single matrix, 2 skinning bank, 3 model-space vertex lerp only (live slot)
  bool vlerp = false, vcolor = false;
  bool kfix = false; // K mode: the converted positions/normals get Kinv (perspective draws that are not static)
  Aff mv{};
  float nrm[9]{};
  const DrawRec *a = nullptr, *b = nullptr;
  RegionWrite &Add(int r) { RegionWrite &x = w[nw++]; x.r = (int16_t)r; return x; }
  bool Full() const { return nw >= 44; }
};
// Skinning bank slots interpolated on demand (cached across consecutive draws with the same banks).
struct BankCache {
  uint64_t ha = 0, hb = 0; // bank hashes of the A and B draw the slots were computed for
  uint64_t done = 0;
  Aff mv[64];
  float nrm[64][9];
  void Reset() { ha = hb = 0; done = 0; }
};

// texture-matrix slots and live position slots read by the draw's decoded vertices
inline void UsedSlots(const DrawRec &d, const GXCommand &bcmd, uint32_t ntg, uint64_t &texslots, uint64_t &posslots) {
  texslots = posslots = 0;
  const auto &vs = bcmd.vertices;
  if (vs.empty()) return;
  // single-matrix and bank draws are transformed at decode (pre_xf); only live draws read live slots
  const bool live = d.kind == kLive;
  const size_t n = (d.pervtx_tex || live) ? vs.size() : 1;
  for (size_t k = 0; k < n; ++k) {
    const VertexData &v = vs[k];
    for (uint32_t i = 0; i < ntg; ++i) texslots |= 1ull << (v.texMtxIdx[i] & 0x3F);
    if (live && !v.pre_xf && v.has_pos) posslots |= 1ull << (v.posMtxIdx & 0x3F);
  }
}
// Hash of a decoded draw's model-space positions/normals (computed by the decode pool): equal hashes in A
// and B = no vertex lerp needed, so the per-vertex comparison runs only for changed vertex data.
inline uint64_t VertexHash(const GXCommand &c) {
  H64 h;
  for (const auto &v : c.vertices) {
    uint64_t w[3];
    std::memcpy(w, v.raw_pos, 12);
    std::memcpy(reinterpret_cast<char *>(w) + 12, v.raw_norm, 12);
    h.w64(w[0]); h.w64(w[1]); h.w64(w[2]);
  }
  h.w64(c.vertices.size());
  return h.get();
}

// Fills ov for B draw di (in B's stream order). bcmd/acmd: the decoded commands (vertices) of the draw in
// B and of its match in A (acmd may be null: no vertex lerp). ntg = XF numTexGens at the draw (B).
// Thread-safe (no cursor lookups): PrepareAll runs it for every draw in parallel before the render walk.
// vh_a / vh_b: VertexHash of acmd / bcmd (0 = unknown).
inline void PrepareDraw(const FrameRec &A, const FrameRec &B, const Plan &P, const Config &cfg, size_t di,
                        const GXCommand &bcmd, const GXCommand *acmd, uint32_t ntg, DrawOverride &ov,
                        uint64_t vh_a = 0, uint64_t vh_b = 0) {
  ov.nw = 0;
  ov.vmode = 0;
  ov.vlerp = ov.vcolor = false;
  ov.a = ov.b = nullptr;
  ov.kfix = false;
  if (P.cut || di >= P.dp.size()) return;
  ov.kfix = P.kmode && !B.draws[di].ortho; // unmatched / rejected: B's own position (Kinv * pos_B)
  if (!(P.dp[di].flags & kDpInterp)) return;
  const DrawRec &d = B.draws[di];
  const DrawRec &a = A.draws[d.match];
  ov.a = &a;
  ov.b = &d;
  // projection: lerp the 6 parameters when the type is equal (FOV zooms)
  const DrawPlan &dp = P.dp[di];
  if (cfg.projection && (dp.flags & kDpProj)) {
    const float *pa = A.Get(kRegProj, a.v_proj), *pb = B.Get(kRegProj, d.v_proj);
    uint32_t ta, tb;
    std::memcpy(&ta, &pa[6], 4);
    std::memcpy(&tb, &pb[6], 4);
    if (ta == tb && std::memcmp(pa, pb, 24) != 0) {
      RegionWrite &x = ov.Add(kRegProj);
      for (int i = 0; i < 6; ++i) x.v[i] = Lerp(pa[i], pb[i], P.t);
      std::memcpy(&x.v[6], &pb[6], 4);
    }
  }
  uint64_t texslots = 0, posslots = 0;
  // texture matrices: only per-vertex indices or rows the worker found different need a look here
  const bool tex_look = cfg.texmtx && (d.pervtx_tex || (dp.flags & kDpTex));
  if (tex_look || d.kind == kLive)
    UsedSlots(d, bcmd, tex_look ? std::min<uint32_t>(ntg, 8) : 0, texslots, posslots);
  for (int s = 0; s < 64 && texslots; ++s) {
    if (!(texslots >> s & 1)) continue;
    const int rows = std::min(3, 64 - s);
    float ma[12], mb[12];
    for (int j = 0; j < rows; ++j) {
      std::memcpy(&ma[j * 4], A.AtFixed(kRegPos + s + j, a.seq), 16);
      std::memcpy(&mb[j * 4], B.AtFixed(kRegPos + s + j, d.seq), 16);
    }
    if (std::memcmp(ma, mb, (size_t)rows * 16) == 0) continue;
    float m[12];
    InterpTex(ma, mb, rows * 4, P.t, m, cfg.tex_unwrap);
    for (int j = 0; j < rows && !ov.Full(); ++j) std::memcpy(ov.Add(kRegPos + s + j).v, &m[j * 4], 16);
  }
  for (int s = 0; s < 64 && posslots; ++s) { // non-snapshot vertices: the renderer uses the live slot
    if (!(posslots >> s & 1) || s * 4 + 12 > 256 || (texslots >> s & 1)) continue;
    const Ctx C(cfg, P);
    Aff ma, mb;
    for (int j = 0; j < 3; ++j) {
      std::memcpy(&ma.m[j * 4], A.AtFixed(kRegPos + s + j, a.seq), 16);
      std::memcpy(&mb.m[j * 4], B.AtFixed(kRegPos + s + j, d.seq), 16);
    }
    if (std::memcmp(ma.m, mb.m, 48) != 0) {
      const Aff m = InterpMV(C, ma, mb, d.ortho);
      for (int j = 0; j < 3 && !ov.Full(); ++j) std::memcpy(ov.Add(kRegPos + s + j).v, &m.m[j * 4], 16);
    }
    const int nr = s & 31;
    if (nr + 3 <= 32) {
      float na[9], nb[9], n[9];
      for (int j = 0; j < 3; ++j) {
        std::memcpy(&na[j * 3], A.AtFixed(kRegNrm + nr + j, a.seq), 12);
        std::memcpy(&nb[j * 3], B.AtFixed(kRegNrm + nr + j, d.seq), 12);
      }
      if (std::memcmp(na, nb, 36) != 0) {
        InterpNrm(C, na, nb, d.ortho, false, n);
        for (int j = 0; j < 3 && !ov.Full(); ++j) std::memcpy(ov.Add(kRegNrm + nr + j).v, &n[j * 3], 12);
      }
    }
  }
  if (cfg.lights && !d.ortho && dp.lights && !P.kmode) { // K mode: lighting stays in B's view space
    for (int l = 0; l < 8; ++l) {
      if (!(dp.lights >> l & 1) || ov.Full()) continue;
      const float *la = A.Get(kRegLight + l, a.v_light[l]), *lb = B.Get(kRegLight + l, d.v_light[l]);
      if (std::memcmp(&la[10], &lb[10], 24) == 0) continue;
      RegionWrite &x = ov.Add(kRegLight + l);
      std::memcpy(x.v, lb, 64);
      float *p = &x.v[10];
      const Ctx C(cfg, P);
      if (C.cam_on(false)) {
        const float *pa = &la[10], *pb = &lb[10];
        float wb[3], w[3];
        for (int i = 0; i < 3; ++i)
          wb[i] = P.cam.dVinv.m[i * 4] * pb[0] + P.cam.dVinv.m[i * 4 + 1] * pb[1] + P.cam.dVinv.m[i * 4 + 2] * pb[2] + P.cam.dVinv.m[i * 4 + 3];
        for (int i = 0; i < 3; ++i) w[i] = Lerp(pa[i], wb[i], P.t);
        for (int i = 0; i < 3; ++i) p[i] = C.Ct.m[i * 4] * w[0] + C.Ct.m[i * 4 + 1] * w[1] + C.Ct.m[i * 4 + 2] * w[2] + C.Ct.m[i * 4 + 3];
        const float *da = &la[13], *db = &lb[13];
        float wdb[3], wd[3];
        for (int i = 0; i < 3; ++i) wdb[i] = P.cam.dR[0 * 3 + i] * db[0] + P.cam.dR[1 * 3 + i] * db[1] + P.cam.dR[2 * 3 + i] * db[2];
        for (int i = 0; i < 3; ++i) wd[i] = Lerp(da[i], wdb[i], P.t);
        for (int i = 0; i < 3; ++i) p[3 + i] = C.Ct.m[i * 4] * wd[0] + C.Ct.m[i * 4 + 1] * wd[1] + C.Ct.m[i * 4 + 2] * wd[2];
        const float lbn = std::sqrt(db[0] * db[0] + db[1] * db[1] + db[2] * db[2]);
        const float ltn = std::sqrt(p[3] * p[3] + p[4] * p[4] + p[5] * p[5]);
        if (ltn > 1e-12f && lbn > 0) for (int i = 3; i < 6; ++i) p[i] *= lbn / ltn;
      } else {
        for (int i = 0; i < 6; ++i) p[i] = Lerp(la[10 + i], lb[10 + i], P.t);
      }
    }
  }
  if (cfg.values && !d.bpmask_pending && !a.bpmask_pending && (dp.flags & (kDpTev | kDpXfCol))) {
    const float *fa = A.Get(kRegTev, a.v_tev), *fb = B.Get(kRegTev, d.v_tev);
    if ((dp.flags & kDpTev) && !ov.Full()) {
      RegionWrite &x = ov.Add(kRegTev);
      for (int r = 0; r < 16; ++r) {
        uint32_t va, vb;
        std::memcpy(&va, &fa[r], 4);
        std::memcpy(&vb, &fb[r], 4);
        uint32_t v = vb;
        if (va != vb) {
          const auto f11 = [](uint32_t u, int sh) { int q = (int)((u >> sh) & 0x7FF); if (q & 0x400) q -= 0x800; return q; };
          v = vb & 0x800000;
          for (int sh : {0, 12}) {
            const int q = (int)std::lround(Lerp((float)f11(va, sh), (float)f11(vb, sh), P.t));
            v |= ((uint32_t)q & 0x7FF) << sh;
          }
        }
        std::memcpy(&x.v[r], &v, 4);
      }
    }
    const float *xa = A.Get(kRegXfCol, a.v_xfcol), *xb = B.Get(kRegXfCol, d.v_xfcol);
    if ((dp.flags & kDpXfCol) && !ov.Full()) {
      RegionWrite &x = ov.Add(kRegXfCol);
      for (int r = 0; r < 4; ++r) {
        uint32_t va, vb, v = 0;
        std::memcpy(&va, &xa[r], 4);
        std::memcpy(&vb, &xb[r], 4);
        for (int sh = 0; sh < 32; sh += 8)
          v |= (uint32_t)std::lround(Lerp((float)((va >> sh) & 0xFF), (float)((vb >> sh) & 0xFF), P.t)) << sh;
        std::memcpy(&x.v[r], &v, 4);
      }
    }
  }
  // vertices
  if (d.kind == kSnap) { ov.vmode = 1; ov.mv = P.dp[di].mv; std::memcpy(ov.nrm, P.dp[di].nrm, sizeof ov.nrm); }
  else if (d.kind == kBank && d.bank && a.bank) ov.vmode = 2;
  const auto &vb = bcmd.vertices;
  const bool same_data = vh_a && vh_b && vh_a == vh_b;
  if (cfg.vertex_lerp && !same_data && acmd && acmd->vertices.size() == vb.size() && !vb.empty()) {
    const auto &va = acmd->vertices;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, maxd = 0;
    for (size_t i = 0; i < vb.size(); ++i) {
      float dd = 0;
      for (int q = 0; q < 3; ++q) {
        const float x = vb[i].raw_pos[q];
        lo[q] = std::min(lo[q], x); hi[q] = std::max(hi[q], x);
        const float e = x - va[i].raw_pos[q];
        dd += e * e;
      }
      maxd = std::max(maxd, dd);
    }
    if (maxd > 0) {
      const float diag2 = (hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]);
      ov.vlerp = maxd <= 0.25f * diag2 + 1e-6f;
    }
  }
  if (cfg.vertex_color_lerp && acmd && acmd->vertices.size() == vb.size())
    for (size_t i = 0; i < vb.size() && !ov.vcolor; ++i)
      ov.vcolor = vb[i].has_color && acmd->vertices[i].has_color &&
                  std::memcmp(vb[i].color, acmd->vertices[i].color, sizeof vb[i].color) != 0;
  if (!ov.vmode && ov.vlerp) ov.vmode = 3;
  // K mode fast path: a static single-matrix draw keeps B's converted vertices (K in the projection moves it)
  if (P.kmode && !d.ortho && d.kind == kSnap && (dp.flags & kDpStatic) && !ov.vlerp && !ov.vcolor) {
    ov.vmode = 0;
    ov.kfix = false;
  }
}

// K mode: positions (x,y,z at 0) and normals (7..9) of the converted vertices times Kinv (rigid: its 3x3 part
// rotates the normals).
inline void ApplyKinv(const Plan &P, float *conv, size_t F, size_t n) {
  const float *m = P.Kinv.m;
  for (size_t i = 0; i < n; ++i) {
    float *o = conv + i * F;
    const float x = o[0], y = o[1], z = o[2];
    o[0] = m[0] * x + m[1] * y + m[2] * z + m[3];
    o[1] = m[4] * x + m[5] * y + m[6] * z + m[7];
    o[2] = m[8] * x + m[9] * y + m[10] * z + m[11];
    const float a = o[7], b = o[8], c = o[9];
    o[7] = m[0] * a + m[1] * b + m[2] * c;
    o[8] = m[4] * a + m[5] * b + m[6] * c;
    o[9] = m[8] * a + m[9] * b + m[10] * c;
  }
}
// K mode: p16 (column-major projection) = p16 * K (K as a 4x4 affine).
inline void ProjTimesK(const Plan &P, float *p16) {
  float r[16];
  const float *k = P.K.m;
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col) {
      float v = 0;
      for (int q = 0; q < 3; ++q) v += p16[q * 4 + row] * k[q * 4 + col];
      if (col == 3) v += p16[3 * 4 + row];
      r[col * 4 + row] = v;
    }
  std::memcpy(p16, r, sizeof r);
}

// ---- every draw prepared in parallel before the in-between render walk (the walk only applies them) ----
struct PreparedDraw {
  uint32_t woff = 0;  // first region write in PreparedFrame::writes[chunk]
  uint16_t nw = 0, chunk = 0;
  uint8_t vmode = 0;
  bool vlerp = false, vcolor = false, kfix = false, has_b = false;
};
struct PreparedFrame {
  static constexpr size_t kGrain = 256;
  std::vector<PreparedDraw> d;
  std::vector<std::vector<RegionWrite>> writes; // per chunk of kGrain draws
};
// bcmds / acmds: the decoded command lists of B and A; vha / vhb: their per-draw vertex hashes (may be empty).
inline void PrepareAll(const FrameRec &A, const FrameRec &B, const Plan &P, const Config &cfg,
                       const std::vector<GXCommand> &bcmds, const std::vector<GXCommand> &acmds,
                       const std::vector<uint64_t> &vha, const std::vector<uint64_t> &vhb, PreparedFrame &out,
                       const ParallelRun *par = nullptr) {
  const size_t n = B.draws.size();
  out.d.assign(n, PreparedDraw{});
  const size_t chunks = (n + PreparedFrame::kGrain - 1) / PreparedFrame::kGrain;
  if (out.writes.size() < chunks) out.writes.resize(chunks);
  for (size_t c = 0; c < chunks; ++c) out.writes[c].clear();
  const auto body = [&](size_t b, size_t e) {
    DrawOverride ov;
    for (size_t di = b; di < e; ++di) {
      const DrawRec &d = B.draws[di];
      if (d.ci >= bcmds.size()) continue;
      const GXCommand *acmd = nullptr;
      uint64_t ha = 0;
      if (d.match >= 0 && (size_t)d.match < A.draws.size()) {
        const uint32_t aci = A.draws[d.match].ci;
        if (aci < acmds.size() && acmds[aci].type == GXCommandType::DrawPrimitive) acmd = &acmds[aci];
        if ((size_t)d.match < vha.size()) ha = vha[d.match];
      }
      PrepareDraw(A, B, P, cfg, di, bcmds[d.ci], acmd, d.ntg, ov, ha, di < vhb.size() ? vhb[di] : 0);
      PreparedDraw &pd = out.d[di];
      const size_t c = di / PreparedFrame::kGrain;
      pd.chunk = (uint16_t)c;
      pd.woff = (uint32_t)out.writes[c].size();
      pd.nw = (uint16_t)ov.nw;
      out.writes[c].insert(out.writes[c].end(), ov.w, ov.w + ov.nw);
      pd.vmode = ov.vmode;
      pd.vlerp = ov.vlerp;
      pd.vcolor = ov.vcolor;
      pd.kfix = ov.kfix;
      pd.has_b = ov.b != nullptr;
    }
  };
  if (par && n > PreparedFrame::kGrain) (*par)(n, PreparedFrame::kGrain, body);
  else
    for (size_t b = 0; b < n; b += PreparedFrame::kGrain) body(b, std::min(n, b + PreparedFrame::kGrain));
}
// The render walk's view of draw di: ov (vertex path) + its region writes.
inline void LoadPrepared(const FrameRec &A, const FrameRec &B, const Plan &P, const PreparedFrame &pf, size_t di,
                         DrawOverride &ov, const RegionWrite **w, int *nw) {
  ov.nw = 0;
  *w = nullptr;
  *nw = 0;
  ov.a = ov.b = nullptr;
  if (di >= pf.d.size()) { ov.vmode = 0; ov.vlerp = ov.vcolor = ov.kfix = false; return; }
  const PreparedDraw &pd = pf.d[di];
  ov.vmode = pd.vmode;
  ov.vlerp = pd.vlerp;
  ov.vcolor = pd.vcolor;
  ov.kfix = pd.kfix;
  if (pd.has_b) {
    ov.b = &B.draws[di];
    ov.a = &A.draws[ov.b->match];
    if (pd.vmode == 1) {
      ov.mv = P.dp[di].mv;
      std::memcpy(ov.nrm, P.dp[di].nrm, sizeof ov.nrm);
    }
  }
  if (pd.nw) {
    *w = &pf.writes[pd.chunk][pd.woff];
    *nw = pd.nw;
  }
}

inline void BankSlot(const Ctx &C, const DrawRec &a, const DrawRec &d, uint32_t s, BankCache &bc) {
  if (bc.done >> s & 1) return;
  const std::vector<float> &pa = a.bank->snap_pos_bank, &pb = d.bank->snap_pos_bank;
  Aff ma, mb;
  std::memcpy(ma.m, &pa[s * 4], 48);
  std::memcpy(mb.m, &pb[s * 4], 48);
  bool st = false;
  bc.mv[s] = InterpMV(C, ma, mb, d.ortho, &st);
  const uint32_t nb = 3u * (s & 31);
  const std::vector<float> &na = a.bank->snap_nrm_bank, &nbk = d.bank->snap_nrm_bank;
  if (na.size() == 96 && nbk.size() == 96) InterpNrm(C, &na[nb], &nbk[nb], d.ortho, st, bc.nrm[s]);
  else std::memset(bc.nrm[s], 0, sizeof bc.nrm[s]);
  bc.done |= 1ull << s;
}

// Rewrites the converted batch vertices of the draw (layout: x,y,z at 0, r,g,b,a at 3, nx,ny,nz at 7;
// F floats per vertex) from the decoded model-space data. live_pos / live_nrm: the renderer's current
// XF matrix memory (overrides applied) for vertices the renderer transforms itself.
inline void PostConvert(const Plan &P, const Config &cfg, const DrawOverride &ov, const GXCommand &bcmd,
                        const GXCommand *acmd, float *conv, size_t F, const float *live_pos, const float *live_nrm,
                        BankCache &bc) {
  if (ov.kfix && (!ov.b || (!ov.vmode && !ov.vcolor))) { ApplyKinv(P, conv, F, bcmd.vertices.size()); return; }
  if (!ov.b || (!ov.vmode && !ov.vcolor)) return;
  const auto &vb = bcmd.vertices;
  const bool lerp_ok = ov.vlerp && acmd && acmd->vertices.size() == vb.size();
  const bool col_ok = ov.vcolor && acmd && acmd->vertices.size() == vb.size();
  if (ov.vmode == 2 && (bc.ha != ov.a->bank_hash || bc.hb != ov.b->bank_hash || !bc.ha)) {
    // a different bank than the previous skinned draw (equal banks keep the slots computed so far)
    bc.done = 0;
    bc.ha = ov.a->bank_hash;
    bc.hb = ov.b->bank_hash;
  }
  for (size_t i = 0; i < vb.size(); ++i) {
    const VertexData &v = vb[i];
    float *o = conv + i * F;
    if (col_ok && v.has_color && acmd->vertices[i].has_color)
      for (int q = 0; q < 4; ++q) o[3 + q] = Lerp(acmd->vertices[i].color[q], v.color[q], P.t);
    if (!v.has_pos || ov.vmode == 0) continue;
    float x = v.raw_pos[0], y = v.raw_pos[1], z = v.raw_pos[2];
    float na = v.raw_norm[0], nb = v.raw_norm[1], nc = v.raw_norm[2];
    if (lerp_ok) {
      const VertexData &w = acmd->vertices[i];
      x = Lerp(w.raw_pos[0], x, P.t); y = Lerp(w.raw_pos[1], y, P.t); z = Lerp(w.raw_pos[2], z, P.t);
      if (v.has_norm) { na = Lerp(w.raw_norm[0], na, P.t); nb = Lerp(w.raw_norm[1], nb, P.t); nc = Lerp(w.raw_norm[2], nc, P.t); }
    }
    const float *m = nullptr, *n = nullptr;
    if (v.pre_xf) {
      if (ov.vmode == 1) { m = ov.mv.m; n = ov.nrm; }
      else if (ov.vmode == 2) {
        const uint32_t s = v.posMtxIdx & 0x3F;
        if (s * 4 + 12 > 256) continue;
        if (!(bc.done >> s & 1)) BankSlot(Ctx(cfg, P), *ov.a, *ov.b, s, bc);
        m = bc.mv[s].m;
        n = bc.nrm[s];
      } else continue;
    } else {
      if (!lerp_ok) continue; // the renderer already used the (overridden) live slot
      const uint32_t base = (uint32_t)v.posMtxIdx * 4, nbase = 3u * (v.posMtxIdx & 31);
      if (base + 12 > 256) continue;
      m = live_pos + base;
      n = nbase + 9 <= 96 ? live_nrm + nbase : nullptr;
    }
    o[0] = m[0] * x + m[1] * y + m[2] * z + m[3];
    o[1] = m[4] * x + m[5] * y + m[6] * z + m[7];
    o[2] = m[8] * x + m[9] * y + m[10] * z + m[11];
    if (v.has_norm && n) {
      o[7] = n[0] * na + n[1] * nb + n[2] * nc;
      o[8] = n[3] * na + n[4] * nb + n[5] * nc;
      o[9] = n[6] * na + n[7] * nb + n[8] * nc;
    }
  }
  if (ov.kfix) ApplyKinv(P, conv, F, vb.size());
}

} // namespace nwii::runtime::gx::interp
