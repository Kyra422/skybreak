#pragma once
// INTERP17: renderer side of the in-between frame (frame interpolation, 60 fps). Implemented in
// src/hle/gx/renderer_gl.cpp (INTERP17 block); driven by src/hle/gx/interp_live.h.
//
// RenderInterpFrame renders an in-between frame from frame B's decoded command list through the normal
// Render() walk, inside a scope that keeps the real frames untouched (INTERP16-plan.md 5.2):
//  - g_state, the dual-texture post matrices and the renderer counters are saved and restored;
//  - the EFB (colour + depth) is backed up before and restored after, so frame B's real render starts
//    from exactly what frame A left;
//  - EFB->texture copies go to shadow textures (never registered in the texture cache, no RAM hash,
//    no overlap drops); EFB copy reads use the shadow made earlier in this in-between frame, else the
//    real copy read-only (a stale RAM hash only skips it, it never drops it);
//  - the XFB copy goes to the present framebuffer as usual (the caller hands it to the present thread
//    before B renders), without the PEEK10 EFB snapshot; pokes, the frame log, the texture-cache frame
//    start and the per-frame shader compile budget reset belong to real frames only.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/gx_command.h"

namespace nwii::runtime::gx {

class IRenderer;

// Per-draw callbacks while the renderer walks B's commands for the in-between frame.
struct InterpDrawHook {
  virtual ~InterpDrawHook() = default;
  // Before draw command `ci` (index in B's list): apply state overrides with InterpWriteXF /
  // InterpWriteBP. Return false to skip the draw.
  virtual bool Pre(size_t ci, const GXCommand &cmd) = 0;
  // After the renderer converted the draw's vertices: `conv` holds cmd.vertices.size() vertices of
  // F floats each (x,y,z at 0, r,g,b,a at 3, nx,ny,nz at 7, texcoords from 10).
  virtual void PostConvert(const GXCommand &cmd, float *conv, size_t F) = 0;
  virtual void Post(size_t ci, const GXCommand &cmd) = 0;
  // The projection uniform (column-major 4x4) about to be used; perspective = a GX perspective projection.
  // The in-between frame may post-multiply it by the camera delta (frame_interp.h K mode).
  virtual void AdjustProj(float *p16, bool perspective) { (void)p16; (void)perspective; }
};

// False (nothing rendered, state untouched) when there is no EFB target yet.
bool RenderInterpFrame(IRenderer &r, const std::vector<GXCommand> &commands, InterpDrawHook &hook);
// Inside the hook: an XF write with the renderer's stream mapping (matrix rows, normal rows, lights,
// projection, XF registers) and its dirty bits / a BP write (TEV colour/konst registers).
void InterpWriteXF(int addr, const float *v, int n);
void InterpWriteBP(uint8_t reg, uint32_t val);
// The renderer's current XF matrix memory (overrides applied), for PostConvert of live-slot vertices.
const float *InterpLivePosMatrices();
const float *InterpLiveNrmMatrices();

struct InterpRenderStats {
  uint64_t frames = 0;        // in-between frames rendered
  uint64_t shadow_copies = 0; // EFB->texture copies redirected to shadow textures
  uint64_t stale_reads = 0;   // real EFB copies whose RAM changed: read as RAM textures (not dropped)
  uint64_t skipped_draws = 0; // draws skipped for the shader compile budget inside in-between frames
  uint64_t xfb_copies = 0;
  uint64_t flushes = 0, state_syncs = 0, uniform_uploads = 0, tex_lookups = 0, vertices = 0; // renderer counters
};
InterpRenderStats GetInterpRenderStats();

// ---- MVGEN21: motion vectors of a real frame + the GPU warp in-between frame (runtime/gx/frame_motion.h) ----
// Real frame, a motion-writing draw (perspective, opaque, depth-writing) right after its vertices were
// converted: write each of the cmd.vertices.size() vertices' previous-frame view-space position at
// conv[i * F + off .. + 2] (conv: F floats per vertex, view-space x, y, z at 0).
struct MotionHook {
  virtual ~MotionHook() = default;
  virtual void Prev(const GXCommand &cmd, float *conv, size_t F, size_t off) = 0;
};
// Arms the NEXT real Render(): it records motion and, at command `boundary` (the first command after the last
// perspective draw), snapshots the EFB (colour + depth) and the render-time state for a later warp.
struct MotionArm {
  MotionHook *hook = nullptr;
  size_t boundary = 0;
  uint64_t frame = 0; // tag of the recording (the pipeline's frame number)
};
// The warp in-between frame between two recorded real frames (prev = N-1, cur = N, both recorded, cur the
// last real Render()): the boundary snapshots warped on the GPU, then cur's commands from its boundary on
// (copies, bloom, HUD, XFB copy) replayed in the isolated in-between scope into the present framebuffer.
struct WarpRequest {
  uint64_t prev_frame = 0, cur_frame = 0;
  float t = 0.5f;
  float sky_rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // rotation of dVinv (N view -> N-1 view), row-major
  bool sky_ok = false;                             // a camera estimate exists (else no background motion)
  int flags = -1;                                  // frame_motion.h kWarp* (-1 = default: sky + blend)
};
struct WarpStats {
  uint64_t warps = 0, failed = 0, recorded = 0;     // warp frames made / refused, real frames recorded
  uint64_t mv_batches = 0, mv_fallbacks = 0, db_switches = 0; // motion batches, plain-program fallbacks, draw-buffer changes
  uint64_t gpu_n = 0;
  double gpu_ms = 0;  // sum of measured GPU times of warp pass + tail (NWII_MVGEN_GPUTIME=1)
  double pass_ms = 0; // ... of the warp pass alone
  bool unsupported = false;
};

// The renderer entry points the pipeline (gx.cpp) uses. renderer_gl.cpp fills this table during static
// initialisation; gx.cpp never names the functions above, so it still links with a renderer that has no
// in-between frames (old renderer copies in negative controls): the table stays empty and interpolation off.
struct InterpRendererApi {
  bool (*render)(IRenderer &, const std::vector<GXCommand> &, InterpDrawHook &) = nullptr;
  void (*write_xf)(int, const float *, int) = nullptr;
  void (*write_bp)(uint8_t, uint32_t) = nullptr;
  const float *(*live_pos)() = nullptr;
  const float *(*live_nrm)() = nullptr;
  InterpRenderStats (*stats)() = nullptr;
  // MVGEN21 (null = a renderer without motion vectors: no warp)
  void (*mv_arm)(const MotionArm &) = nullptr;
  bool (*mv_recorded)(uint64_t frame) = nullptr; // a slot holds that frame's motion + boundary snapshot
  bool (*warp)(IRenderer &, const std::vector<GXCommand> &, const WarpRequest &) = nullptr; // I -> present FB
  void (*warp_done)() = nullptr; // after the I hand-off: the real frame's image back into the present FB
  WarpStats (*warp_stats)() = nullptr;
};
inline InterpRendererApi g_interp_renderer{};

// ---- host side (gx.cpp / src/hle/gx/interp_live.h <-> main.cpp) ----
// Steady-clock microseconds (the time base of every INTERP17 timestamp).
int64_t InterpNowUs();
// Set by the host: called on the host thread (host GL context current) right after an in-between frame
// was rendered into the present framebuffer (PresentFramebuffer()), before the real frame `frame`
// renders. t_cut_us = when `frame` was cut; period_us = the guest frame interval estimate.
extern void (*g_interpPresentHook)(uint64_t frame, int64_t t_cut_us, int64_t period_us);
// The newest real frame's timing, for the host's submit of it (main.cpp, after ProcessGXFifo).
struct InterpFrameInfo {
  uint64_t frame = 0;
  int64_t t_cut_us = 0;
  int64_t period_us = 0;
  bool paced = false;      // frame interpolation is on: present on the 2x schedule
  bool had_interp = false; // an in-between frame (or the dup A/B frame) was handed over before it
};
InterpFrameInfo InterpLastFrameInfo();
// The host's display check (main.cpp): false = the desktop composes below 50 Hz, 60 fps is impossible
// and no in-between frames are made.
void InterpSetDisplayOk(bool ok);
// [INTERP17] statistics line (NWII_INTERP_STATS=1), host thread; empty when there is nothing new.
std::string InterpStatsLine();

} // namespace nwii::runtime::gx
