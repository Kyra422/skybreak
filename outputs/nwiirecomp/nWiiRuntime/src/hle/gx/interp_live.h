#pragma once
// INTERP17: the live frame interpolation pipeline (60 fps through in-between frames). Included by
// src/hle/gx.cpp only (gxreplay links gx.cpp, so the same code runs offline). Off unless
// [graphics] frame_interpolation = true or NWII_FRAME_INTERP=1 (runtime/interp_config.h).
//
// Per cut guest frame N (gx.cpp ProcessGXFifoImpl):
//   OnCut        host, before decode: take the draws' DrawRaw references and the renderer's start state,
//                hand the frame to the worker thread: RecordFrame(N) + MakePlan(N-1, N) run in parallel
//                with the decode pool (frame_interp.h).
//   decode pool  + a model-space vertex hash per draw (VhashSink).
//   BeforeRender host: wait for the plan; render the in-between frame I = interp(N-1, N, 0.5) from N's
//                commands through RenderInterpFrame (interp_render.h: isolated scope); hand I to the host's
//                present hook (main.cpp submits it to the present thread, paced). Then gx.cpp renders N.
//   Keep         host, after N rendered: N (decoded) becomes the next A, N-1 is freed.
// No I (N is shown for two 60 Hz periods instead) on: a cut (camera/scene change, see frame_interp.h
// CutCheck), no history, a newer guest frame already waiting (host behind), the adaptive budget skip,
// a game running faster than 45 fps, or a display composing below 50 Hz.
//
// Diagnostics (env, all off by default):
//   NWII_INTERP_STATS=1        [INTERP17] line every 5 s (main.cpp prints it with the present stats)
//   NWII_INTERP_T=<t>          interpolation time (default 0.5)
//   NWII_INTERP_REFINE=0|1|2   instance re-pairing off / nearest / auto (default 2)
//   NWII_INTERP_VCOLOR=1       vertex colour lerp
//   NWII_INTERP_GUARDS=0       cut/jump guards off (diagnostic only)
//   NWII_INTERP_LOG=1          one [INTERP17] line per guest frame (decision, match, camera, timings)
//   NWII_INTERP_PROFILE=1      time of the per-draw hooks inside the in-between frame (in the stats line)
//   NWII_INTERP_FORCE=1        no budget / behind / pacing skips (leak test, profiling)
//   NWII_INTERP_KMODE=0        camera delta per vertex on the CPU instead of in the projection (A/B, slower)
//   NWII_INTERP_SHOT=<prefix>  every NWII_INTERP_SHOT_EVERY (60) frames: <prefix>_<N>_a.bmp (N-1, the image
//                              on screen before I), _i.bmp (I), _b.bmp (N), 640x480 like NWII_SCREENSHOT
//
// MVGEN21 ([graphics] frame_interpolation_mode, runtime/interp_config.h): "rerender" = the INTERP17 path above;
// "warp" = I made AFTER N rendered, on the GPU: N records per-pixel motion + a boundary snapshot while it renders
// (MotionHook::Prev below gives each vertex its N-1 position from the plan), then the renderer warps the boundary
// snapshots of N-1 and N and replays N's 2D tail (frame_motion.h, renderer_gl.cpp MVGEN21 block); "auto"
// (default) = rerender when the host fits both renders in the guest frame (frame_motion.h AutoDecider), else warp.
//   NWII_INTERP_TEST_STRIDE=2  (offline quality test) only every 2nd guest frame takes part (phase
//                              NWII_INTERP_TEST_PHASE): I = interp(f-2, f), the real f-1 is the ground truth
//   NWII_MVGEN_FLAGS=<n>       warp flags (frame_motion.h kWarp*; default sky + blend)
//   NWII_MVGEN_GPUTIME=1       GPU timestamps of the warp (in the stats line)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/gx/frame_interp.h"
#include "runtime/gx/frame_motion.h" // MVGEN21
#include "runtime/gx/frame_pipeline.h"
#include "runtime/gx/gx_capture.h"
#include "runtime/gx/interp_render.h"
#include "runtime/gx/renderer.h"
#include "runtime/gx_state.h"
#include "runtime/interp_config.h"

int GX_PendingFrames(); // gx.cpp

namespace nwii::runtime::gx {
void (*g_interpPresentHook)(uint64_t frame, int64_t t_cut_us, int64_t period_us) = nullptr;
int64_t InterpNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace nwii::runtime::gx

namespace interp17 {
using nwii::runtime::gx::DrawRaw;
using nwii::runtime::gx::GXCommand;
using nwii::runtime::gx::GXCommandType;
namespace ip = nwii::runtime::gx::interp;
namespace gx = nwii::runtime::gx;
namespace motion = nwii::runtime::gx::motion; // MVGEN21

inline int EnvInt(const char *n, int def) {
  const char *e = std::getenv(n);
  return e && *e ? std::atoi(e) : def;
}

class Pipeline final : public gx::InterpDrawHook, public gx::MotionHook { // MVGEN21: + MotionHook
public:
  static Pipeline &Get() {
    static Pipeline *p = new Pipeline(); // never destroyed: its worker blocks until the process exits
    return *p;
  }

  // ---- host thread ----
  void OnCut(std::vector<GXCommand> &frame, uint64_t frame_no, bool have_renderer, void (*free_fn)(std::vector<GXCommand> &&)) {
    const int64_t now = gx::InterpNowUs();
    if (last_cut_us_) {
      const int64_t dt = now - last_cut_us_;
      raw_ema_us_ = raw_ema_us_ ? (raw_ema_us_ * 7 + dt) / 8 : dt;
      const int64_t c = std::clamp<int64_t>(dt, 25000, 50000);
      period_us_ = period_us_ ? (period_us_ * 7 + c) / 8 : c;
    }
    last_cut_us_ = now;
    info_.frame = frame_no;
    info_.t_cut_us = now;
    info_.period_us = period_us_ ? period_us_ : 33367;
    info_.had_interp = false;
    // (a renderer without in-between frames, e.g. an old copy in a negative control, registers nothing)
    const bool on = have_renderer && gx::g_interp_renderer.render && nwii::runtime::FrameInterpOn();
    info_.paced = on && display_ok_.load(std::memory_order_relaxed) && !(raw_ema_us_ && raw_ema_us_ < 22000);
    job_active_ = false;
    if (!on) {
      DropHistory(free_fn);
      return;
    }
    if (!announced_) {
      announced_ = true;
      // started on first use: no threads while the option is off. A small pool hashes the draws and plans
      // the per-draw matrices in parallel (the decode pool runs at the same time).
      const int hw = (int)std::thread::hardware_concurrency();
      pool_ = std::make_unique<gx::ParallelFor>(std::clamp(hw / 8, 1, 4));
      par_ = [this](size_t n, size_t g, const std::function<void(size_t, size_t)> &fn) { pool_->Run(n, g, fn); };
      worker_ = std::thread([this] { Worker(); });
      worker_.detach();
      std::printf("[INTERP17] frame interpolation ON (mode %s, 2D %s, t=%.2f)\n", // MVGEN21: mode name
                  nwii::runtime::InterpModeName(nwii::runtime::FrameInterpMode(), nwii::runtime::FrameInterpDup()),
                  nwii::runtime::FrameInterp2D() ? "on" : "off", t_);
      std::fflush(stdout);
    }
    // MVGEN21 (offline quality test): only every test_stride_-th frame takes part; the others render as usual
    // and are the ground truth of the in-between frame made across them (history kept).
    if (test_stride_ > 1 && (int)(frame_no % (uint64_t)test_stride_) != test_phase_)
      return;
    // the worker's inputs; it starts now and runs while gx.cpp decodes the draws
    raws_.clear();
    scan_.Clear();
    size_t nd = 0;
    for (size_t ci = 0; ci < frame.size(); ++ci) {
      const GXCommand &c = frame[ci];
      if (c.type == GXCommandType::DrawPrimitive) {
        raws_.push_back(c.raw);
        scan_.prim.push_back(c.prim_type);
        scan_.idx.push_back((uint32_t)ci | 0x80000000u);
        ++nd;
      } else if (c.type == GXCommandType::XFRegister || c.type == GXCommandType::BPRegister) {
        scan_.idx.push_back((uint32_t)ci);
      }
    }
    ip::CaptureBase(gx::g_state, base_);
    cur_vh_.assign(nd, 0);
    cfg_.interp_2d = nwii::runtime::FrameInterp2D();
    cfg_.refine_instances = refine_ != 0;
    cfg_.refine_auto = refine_ == 2;
    cfg_.vertex_color_lerp = vcolor_;
    cfg_.guards = guards_;
    cfg_.kmode = kmode_;
    {
      std::lock_guard<std::mutex> lk(m_);
      job_frame_ = &frame;
      job_no_ = frame_no;
      job_ = true;
      done_ = false;
    }
    cv_.notify_all();
    job_active_ = true;
  }
  uint64_t *VhashSink() { return job_active_ && !cur_vh_.empty() ? cur_vh_.data() : nullptr; }

  void BeforeRender(gx::IRenderer &r, std::vector<GXCommand> &frame) {
    if (!job_active_)
      return;
    const int64_t w0 = gx::InterpNowUs();
    WaitDone();
    const int64_t w1 = gx::InterpNowUs();
    win_.wait_us += (uint64_t)(w1 - w0);
    win_.rec_us += rec_us_;
    win_.plan_us += plan_us_;
    ++win_.frames;
    // MVGEN21: in "auto"/"warp" every paced real frame records motion + its boundary snapshot (the warp of this
    // frame, after it rendered, or of the next one needs it).
    const int mode = nwii::runtime::FrameInterpMode();
    const bool dup = nwii::runtime::FrameInterpDup();
    mv_on_ = !dup && mode != nwii::runtime::kInterpRerender && gx::g_interp_renderer.mv_arm &&
             gx::g_interp_renderer.warp && (info_.paced || force_);
    warp_this_ = false;
    r_ = &r;
    mframe_cand_ = &frame;
    boundary_ = 0; // the first command after the last perspective draw (0: no 3D in this frame)
    for (size_t i = cur_rec_.draws.size(); i-- > 0;)
      if (!cur_rec_.draws[i].ortho) { boundary_ = (size_t)cur_rec_.draws[i].ci + 1; break; }
    // "auto" records only when a warp is possible soon (frame_motion.h AutoDecider::Close) or warped < 2 s ago
    record_ = mv_on_ && (mode == nwii::runtime::kInterpWarp || force_ || w1 - last_warp_us_ < 2000000 ||
                         auto_.Close(w1 - info_.t_cut_us, MaxOf(rn_hist_), info_.period_us));
    struct ArmAtExit { // every return below: arm the real render of this frame (MVGEN21)
      Pipeline *p;
      ~ArmAtExit() { p->ArmMotion(); }
    } arm_at_exit{this};
    const char *why = nullptr;
    if (!prev_valid_ || !have_plan_) why = "nohist";
    else if (plan_.cut) why = "cut";
    else if (!info_.paced && !force_) why = "unpaced";
    else if (!force_ && GX_PendingFrames() > 0) why = "behind";
    else if (mv_on_ && mode == nwii::runtime::kInterpWarp) { // MVGEN21: always warp
      if (WarpReady()) warp_this_ = true;
      else why = "nowarp";
    } else if (mv_on_) { // MVGEN21 "auto": re-render when both renders fit, else warp
      if (!force_ && !auto_.Rerender(w1, w1 - info_.t_cut_us, MaxOf(rn_hist_), info_.period_us)) {
        if (WarpReady()) warp_this_ = true;
        else why = "budget";
      }
    } else if (!force_ && w1 < budget_skip_until_us_) why = "budget";
    else if (!force_ && !budget_trial_ && ema_ri_us_ &&
             (w1 - info_.t_cut_us) + MaxOf(ri_hist_) + MaxOf(rn_hist_) + MaxOf(prep_hist_) >
                 (info_.period_us * 90) / 100) {
      // (the second-slowest of the last 8 measured renders: a frame over the budget would merge (lose) the next)
      // both renders would not fit in the guest frame: the next frame would be merged (lost). Show this one
      // for two periods instead; after 0.5 s one trial in-between frame measures the cost again.
      why = "budget";
      budget_skip_until_us_ = w1 + 500000;
      budget_trial_ = true;
      ++win_.budget_trips;
    }
    if (!why && !warp_this_ && budget_trial_) { budget_trial_ = false; trial_next_ = true; } // this frame is the trial
    if (have_plan_) {
      win_.b_persp += plan_.ms.b_persp;
      win_.persp_matched += plan_.ms.persp_matched;
      win_.b_ortho += plan_.ms.b_ortho;
      win_.ortho_matched += plan_.ms.ortho_matched;
      win_.static_draws += plan_.static_draws;
      win_.interp_draws += plan_.interpolated;
      if (plan_.cut) last_cut_reason_ = plan_.cut_reason;
    }
    if (log_)
      std::printf("[INTERP17] frame %llu: %s | draws %zu matched 3d %zu/%zu 2d %zu/%zu cam %s %.2f deg %.1f u | "
                  "interp %zu static %zu rejected %zu | record %.2f (hash %.2f walk %.2f) plan %.2f (match %.2f cam %.2f refine %.2f "
                  "draws %.2f) wait %.2f ms%s%s\n",
                  (unsigned long long)job_no_, why ? why : warp_this_ ? "warp" : "interpolate", cur_rec_.draws.size(),
                  have_plan_ ? plan_.ms.persp_matched : 0, have_plan_ ? plan_.ms.b_persp : 0,
                  have_plan_ ? plan_.ms.ortho_matched : 0, have_plan_ ? plan_.ms.b_ortho : 0,
                  have_plan_ && plan_.cam.ok ? "ok" : "-", have_plan_ ? plan_.cam.angle_deg : 0.0f,
                  have_plan_ ? plan_.cam.trans : 0.0f, have_plan_ ? plan_.interpolated : 0,
                  have_plan_ ? plan_.static_draws : 0, have_plan_ ? plan_.rejected_move : 0, rec_us_ / 1000.0, cur_rec_.us_hash / 1000.0, cur_rec_.us_walk / 1000.0,
                  plan_us_ / 1000.0, plan_.us_match / 1000.0, plan_.us_cam / 1000.0, plan_.us_refine / 1000.0,
                  plan_.us_draws / 1000.0, (w1 - w0) / 1000.0, have_plan_ && plan_.cut ? " cut: " : "",
                  have_plan_ && plan_.cut ? plan_.cut_reason : "");
    t_before_n_us_ = w1;
    if (why) {
      if (!std::strcmp(why, "cut")) ++win_.cut;
      else if (!std::strcmp(why, "behind")) ++win_.behind;
      else if (!std::strcmp(why, "budget")) ++win_.budget;
      else if (!std::strcmp(why, "nohist")) ++win_.nohist;
      else if (!std::strcmp(why, "nowarp")) ++win_.nowarp; // MVGEN21
      else ++win_.unpaced;
      if (have_plan_) { prev_cam_ = plan_.cam; prev_cam_ok_ = true; }
      return;
    }
    prev_cam_ = plan_.cam;
    prev_cam_ok_ = true;
    if (warp_this_) { // MVGEN21: I is made in Keep, after N rendered
      if (ShotFrame(job_no_)) Shot("a");
      return;
    }
    trial_render_ = trial_next_;
    trial_next_ = false;
    const bool shot = ShotFrame(job_no_);
    if (shot) Shot("a");
    const int64_t r0 = gx::InterpNowUs();
    bool rendered = false;
    if (nwii::runtime::FrameInterpDup()) {
      rendered = true; // A/B control: the present framebuffer still holds N-1
      ++win_.dup;
    } else {
      const int64_t q0 = gx::InterpNowUs();
      ip::PrepareAll(prev_rec_, cur_rec_, plan_, cfg_, frame, prev_cmds_, prev_vh_, cur_vh_, prep_, &par_);
      win_.prep_us += (uint64_t)(gx::InterpNowUs() - q0);
      prep_hist_[prep_i_++ & 7] = gx::InterpNowUs() - q0;
      b_cmds_ = &frame;
      di_ = 0;
      ov_.nw = 0;
      ov_.b = nullptr;
      bank_.Reset();
      for (int rr : active_) ap_[rr].on = false;
      active_.clear();
      rendered = gx::g_interp_renderer.render && gx::g_interp_renderer.render(r, frame, *this);
      b_cmds_ = nullptr;
      if (rendered) ++win_.interp;
    }
    const int64_t r1 = gx::InterpNowUs();
    win_.ri_us += (uint64_t)(r1 - r0);
    if (rendered && !nwii::runtime::FrameInterpDup()) {
      ema_ri_us_ = ema_ri_us_ && !trial_render_ ? (ema_ri_us_ * 3 + (r1 - r0)) / 4 : (r1 - r0);
      if (trial_render_) std::fill(std::begin(ri_hist_), std::end(ri_hist_), int64_t(0)); // re-measure after a pause
      ri_hist_[ri_i_++ & 7] = r1 - r0;
      if (ema_rn_us_) auto_.Measured((r1 - r0) + prep_hist_[(prep_i_ - 1) & 7], ema_rn_us_); // MVGEN21
    }
    if (!rendered)
      return;
    if (shot) Shot("i");
    info_.had_interp = true;
    if (gx::g_interpPresentHook)
      gx::g_interpPresentHook(job_no_, info_.t_cut_us, info_.period_us);
    t_before_n_us_ = gx::InterpNowUs();
  }

  // After the real render of `frame`: it becomes the next A (decoded vertices kept one frame longer).
  void Keep(std::vector<GXCommand> &&frame, void (*free_fn)(std::vector<GXCommand> &&)) {
    if (!job_active_) {
      free_fn(std::move(frame));
      return;
    }
    WaitDone(); // normally already done in BeforeRender (no renderer: the worker may still read the frame)
    // the host's time for this guest frame (cut -> both renders done) and the real render's own time
    const int64_t now = gx::InterpNowUs();
    if (t_before_n_us_) {
      const int64_t rn = now - t_before_n_us_;
      ema_rn_us_ = ema_rn_us_ ? (ema_rn_us_ * 7 + rn) / 8 : rn;
      rn_hist_[rn_i_++ & 7] = rn;
      win_.rn_us += (uint64_t)rn;
    }
    if (warp_this_) { // MVGEN21: the warp in-between frame, now that N rendered
      warp_this_ = false;
      Warp(frame);
    }
    win_.host_us += (uint64_t)(gx::InterpNowUs() - info_.t_cut_us);
    if (ShotFrame(job_no_) && info_.had_interp) Shot("b");
    if (prev_valid_) free_fn(std::move(prev_cmds_));
    prev_cmds_ = std::move(frame);
    std::swap(prev_rec_, cur_rec_);
    std::swap(prev_vh_, cur_vh_);
    prev_valid_ = true;
    job_active_ = false;
  }

  gx::InterpFrameInfo LastInfo() const { return info_; }
  void SetDisplayOk(bool ok) { display_ok_.store(ok, std::memory_order_relaxed); }

  std::string StatsLine() {
    const auto st = gx::g_interp_renderer.stats ? gx::g_interp_renderer.stats() : gx::InterpRenderStats{};
    const auto rs = gx::GetRendererStats();
    if (!win_.frames) return std::string();
    // renderer work per in-between frame vs per real frame since the last line
    const double ni = std::max<double>(1.0, (double)(st.frames - last_ist_.frames));
    const double nr = std::max<double>(1.0, (double)win_.frames);
    char work[768]; // MVGEN21: room for its part
    std::snprintf(work, sizeof work,
                  " | per frame I/N: flushes %.0f/%.0f syncs %.0f/%.0f uniforms %.0f/%.0f texlookups %.0f/%.0f verts %.0fk/%.0fk",
                  (st.flushes - last_ist_.flushes) / ni, (rs.flushes - last_rs_.flushes) / nr,
                  (st.state_syncs - last_ist_.state_syncs) / ni, (rs.state_syncs - last_rs_.state_syncs) / nr,
                  (st.uniform_uploads - last_ist_.uniform_uploads) / ni, (rs.uniform_uploads - last_rs_.uniform_uploads) / nr,
                  (st.tex_lookups - last_ist_.tex_lookups) / ni, (rs.tex_lookups - last_rs_.tex_lookups) / nr,
                  (st.vertices - last_ist_.vertices) / ni / 1000.0, (rs.vertices - last_rs_.vertices) / nr / 1000.0);
    if (prof_) {
      const size_t l = std::strlen(work);
      std::snprintf(work + l, sizeof work - l, " | I hooks ms: pre %.2f postconvert %.2f", win_.pre_us / ni / 1000.0,
                    win_.post_us / ni / 1000.0);
    }
    {
      const size_t l = std::strlen(work);
      std::snprintf(work + l, sizeof work - l, " | prepare %.2f ms", win_.prep_us / ni / 1000.0);
    }
    { // MVGEN21: warp frames (CPU ms each incl. the 2D tail; GPU ms with NWII_MVGEN_GPUTIME=1)
      const gx::WarpStats ws = gx::g_interp_renderer.warp_stats ? gx::g_interp_renderer.warp_stats() : gx::WarpStats{};
      const double nw = std::max<double>(1.0, (double)win_.warp), ngpu = (double)(ws.gpu_n - last_ws_.gpu_n);
      const size_t l = std::strlen(work);
      std::snprintf(work + l, sizeof work - l,
                    " | MVGEN21 mode=%s warp=%llu nowarp=%llu warp_late=%llu warp_fail=%llu warp_cpu=%.2fms "
                    "warp_gpu=%.2fms pass_gpu=%.2fms recorded=%llu mv_batches/f=%.0f fallbacks=%llu dbsw/f=%.0f%s",
                    nwii::runtime::InterpModeName(nwii::runtime::FrameInterpMode(), nwii::runtime::FrameInterpDup()),
                    (unsigned long long)win_.warp, (unsigned long long)win_.nowarp, (unsigned long long)win_.warp_late,
                    (unsigned long long)win_.warp_fail, win_.warp_us / nw / 1000.0,
                    ngpu > 0 ? (ws.gpu_ms - last_ws_.gpu_ms) / ngpu : 0.0, ngpu > 0 ? (ws.pass_ms - last_ws_.pass_ms) / ngpu : 0.0,
                    (unsigned long long)(ws.recorded - last_ws_.recorded),
                    (double)(ws.mv_batches - last_ws_.mv_batches) / std::max<double>(1.0, (double)(ws.recorded - last_ws_.recorded)),
                    (unsigned long long)(ws.mv_fallbacks - last_ws_.mv_fallbacks),
                    (double)(ws.db_switches - last_ws_.db_switches) / std::max<double>(1.0, (double)(ws.recorded - last_ws_.recorded)),
                    ws.unsupported ? " UNSUPPORTED" : "");
      last_ws_ = ws;
    }
    last_ist_ = st;
    last_rs_ = rs;
    char line[1536]; // MVGEN21
    const double n = (double)win_.frames;
    std::snprintf(line, sizeof line,
                  "[INTERP17] frames=%llu interp=%llu dup=%llu skipped: cut=%llu behind=%llu budget=%llu nohist=%llu "
                  "unpaced=%llu | match3d=%.1f%% match2d=%.1f%% static=%.0f%% | ms/frame record=%.2f plan=%.2f "
                  "wait=%.2f render_I=%.2f render_N=%.2f host=%.2f | P=%.1fms budget_trips=%llu shadow_copies=%llu "
                  "stale_reads=%llu I_skipped_draws=%llu last_cut=\"%s\"%s",
                  (unsigned long long)win_.frames, (unsigned long long)win_.interp, (unsigned long long)win_.dup,
                  (unsigned long long)win_.cut, (unsigned long long)win_.behind, (unsigned long long)win_.budget,
                  (unsigned long long)win_.nohist, (unsigned long long)win_.unpaced,
                  win_.b_persp ? 100.0 * win_.persp_matched / win_.b_persp : 0.0,
                  win_.b_ortho ? 100.0 * win_.ortho_matched / win_.b_ortho : 0.0,
                  win_.interp_draws ? 100.0 * win_.static_draws / win_.interp_draws : 0.0, win_.rec_us / n / 1000.0,
                  win_.plan_us / n / 1000.0, win_.wait_us / n / 1000.0,
                  (win_.interp + win_.dup) ? win_.ri_us / double(win_.interp + win_.dup) / 1000.0 : 0.0,
                  win_.rn_us / n / 1000.0, win_.host_us / n / 1000.0, info_.period_us / 1000.0, (unsigned long long)win_.budget_trips,
                  (unsigned long long)st.shadow_copies, (unsigned long long)st.stale_reads,
                  (unsigned long long)st.skipped_draws, last_cut_reason_, work);
    win_ = Window{};
    return line;
  }

  // ---- InterpDrawHook (host thread, inside RenderInterpFrame) ----
  bool Pre(size_t ci, const GXCommand &cmd) override {
    const auto &Bd = cur_rec_.draws;
    while (di_ < Bd.size() && Bd[di_].ci < ci) ++di_;
    if (di_ >= Bd.size() || Bd[di_].ci != ci) { // not recorded (cannot happen): draw as B
      ov_.nw = 0;
      ov_.b = nullptr;
      ov_.kfix = false;
      RestoreAll(nullptr);
      return true;
    }
    const ip::DrawRec &d = Bd[di_];
    const int64_t p0 = prof_ ? gx::InterpNowUs() : 0;
    const ip::RegionWrite *w = nullptr;
    int nw = 0;
    ip::LoadPrepared(prev_rec_, cur_rec_, plan_, prep_, di_, ov_, &w, &nw);
    acmd_ = nullptr;
    if (ov_.a && ov_.a->ci < prev_cmds_.size()) acmd_ = &prev_cmds_[ov_.a->ci];
    Apply(d, w, nw);
    if (prof_) win_.pre_us += (uint64_t)(gx::InterpNowUs() - p0);
    return true;
  }
  void PostConvert(const GXCommand &cmd, float *conv, size_t F) override {
    if (!ov_.b && !ov_.kfix) return;
    const int64_t p0 = prof_ ? gx::InterpNowUs() : 0;
    ip::PostConvert(plan_, cfg_, ov_, cmd, acmd_, conv, F, gx::g_interp_renderer.live_pos(), gx::g_interp_renderer.live_nrm(), bank_);
    if (prof_) win_.post_us += (uint64_t)(gx::InterpNowUs() - p0);
  }
  void Post(size_t, const GXCommand &) override { ++di_; }
  void AdjustProj(float *p16, bool perspective) override {
    if (perspective && plan_.kmode) ip::ProjTimesK(plan_, p16); // K mode (frame_interp.h)
  }

  // ---- MVGEN21: MotionHook (host thread, inside the real Render() of an armed frame) ----
  void Prev(const GXCommand &cmd, float *conv, size_t F, size_t off) override {
    const auto &Bd = cur_rec_.draws;
    const size_t ci = mframe_ ? (size_t)(&cmd - mframe_->data()) : (size_t)-1;
    while (mdi_ < Bd.size() && Bd[mdi_].ci < ci) ++mdi_;
    const bool rec = mdi_ < Bd.size() && Bd[mdi_].ci == ci;
    const bool plan = rec && prev_valid_ && have_plan_;
    const GXCommand *acmd = nullptr;
    uint64_t vha = 0, vhb = 0;
    if (plan && Bd[mdi_].match >= 0 && (size_t)Bd[mdi_].match < prev_rec_.draws.size()) {
      const size_t m = (size_t)Bd[mdi_].match;
      const uint32_t aci = prev_rec_.draws[m].ci;
      if (aci < prev_cmds_.size() && prev_cmds_[aci].type == GXCommandType::DrawPrimitive) acmd = &prev_cmds_[aci];
      vha = m < prev_vh_.size() ? prev_vh_[m] : 0;
      vhb = mdi_ < cur_vh_.size() ? cur_vh_[mdi_] : 0;
    }
    motion::PrevForDraw(plan ? &prev_rec_ : nullptr, cur_rec_, plan ? &plan_ : nullptr, rec ? mdi_ : 0, cmd, acmd,
                        vha, vhb, conv, F, off, pc_);
  }

private:
  // MVGEN21: arm the real render of this frame (motion + boundary snapshot) when "auto"/"warp" is active.
  void ArmMotion() {
    if (!mv_on_ || !record_ || !gx::g_interp_renderer.mv_arm || !boundary_ || !mframe_cand_) return;
    mframe_ = mframe_cand_;
    mdi_ = 0;
    pc_.draw = -1;
    gx::MotionArm a;
    a.hook = this;
    a.boundary = boundary_;
    a.frame = job_no_;
    gx::g_interp_renderer.mv_arm(a);
  }
  // N-1 recorded (motion + boundary snapshot) and N has 3D: a warp after N's render can work.
  bool WarpReady() const {
    return boundary_ && gx::g_interp_renderer.mv_recorded && gx::g_interp_renderer.mv_recorded(prev_rec_.frame) &&
           prev_rec_.frame + (uint64_t)test_stride_ == job_no_;
  }
  // After N rendered: I = the GPU warp of N-1 and N + N's 2D tail, handed to the present thread before N.
  void Warp(std::vector<GXCommand> &frame) {
    const int64_t w0 = gx::InterpNowUs();
    if (!force_ && warp_n_ >= 4 && (w0 - info_.t_cut_us) + MaxOf(warp_hist_) > (info_.period_us * 95) / 100) {
      ++win_.warp_late; // I would reach the present thread after its slot: N alone, for two periods
      return;
    }
    gx::WarpRequest q;
    q.prev_frame = prev_rec_.frame;
    q.cur_frame = job_no_;
    q.t = t_;
    q.sky_ok = plan_.cam.ok;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) q.sky_rot[i * 3 + j] = plan_.cam.dVinv.m[i * 4 + j];
    q.flags = warp_flags_;
    if (!r_ || !gx::g_interp_renderer.warp(*r_, frame, q)) {
      ++win_.warp_fail;
      return;
    }
    if (ShotFrame(job_no_)) Shot("i");
    info_.had_interp = true;
    if (gx::g_interpPresentHook)
      gx::g_interpPresentHook(job_no_, info_.t_cut_us, info_.period_us);
    if (gx::g_interp_renderer.warp_done) gx::g_interp_renderer.warp_done();
    const int64_t w1 = gx::InterpNowUs();
    warp_hist_[warp_i_++ & 7] = w1 - w0;
    last_warp_us_ = w1;
    ++warp_n_;
    win_.warp_us += (uint64_t)(w1 - w0);
    ++win_.warp;
  }

  Pipeline() {
    {
      const char *e = std::getenv("NWII_INTERP_T");
      if (e && *e) t_ = std::clamp((float)std::atof(e), 0.0f, 1.0f);
    }
    refine_ = std::clamp(EnvInt("NWII_INTERP_REFINE", 2), 0, 2);
    vcolor_ = EnvInt("NWII_INTERP_VCOLOR", 0) != 0;
    guards_ = EnvInt("NWII_INTERP_GUARDS", 1) != 0;
    log_ = EnvInt("NWII_INTERP_LOG", 0) != 0;
    force_ = EnvInt("NWII_INTERP_FORCE", 0) != 0;
    prof_ = EnvInt("NWII_INTERP_PROFILE", 0) != 0;
    kmode_ = EnvInt("NWII_INTERP_KMODE", 1) != 0;
    if (const char *e = std::getenv("NWII_INTERP_SHOT"); e && *e) shot_prefix_ = e;
    test_stride_ = std::clamp(EnvInt("NWII_INTERP_TEST_STRIDE", 1), 1, 8); // MVGEN21
    test_phase_ = std::clamp(EnvInt("NWII_INTERP_TEST_PHASE", 0), 0, test_stride_ - 1);
    warp_flags_ = EnvInt("NWII_MVGEN_FLAGS", -1);
    shot_every_ = std::max(1, EnvInt("NWII_INTERP_SHOT_EVERY", 60));
  }

  void Worker() {
    for (;;) {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return job_; });
      job_ = false;
      const std::vector<GXCommand> *f = job_frame_;
      const uint64_t no = job_no_;
      lk.unlock();
      const int64_t t0 = gx::InterpNowUs();
      ip::RecordFrame(*f, raws_, base_, no, cur_rec_, &par_, &scan_);
      std::vector<std::shared_ptr<DrawRaw>> dying; // released after the hand-off (60k frees)
      dying.swap(raws_); // the bank draws keep their DrawRaw (DrawRec::bank)
      const int64_t t1 = gx::InterpNowUs();
      bool plan = false;
      if (prev_valid_ && !prev_rec_.draws.empty() && !cur_rec_.draws.empty()) {
        ip::MakePlan(prev_rec_, cur_rec_, t_, cfg_, prev_cam_ok_ ? &prev_cam_ : nullptr, plan_, &par_);
        plan = true;
      }
      const int64_t t2 = gx::InterpNowUs();
      lk.lock();
      have_plan_ = plan;
      rec_us_ = (uint64_t)(t1 - t0);
      plan_us_ = (uint64_t)(t2 - t1);
      done_ = true;
      lk.unlock();
      cv_.notify_all();
      dying.clear(); // the DrawRaws decode already dropped are freed here, off the host's critical path
    }
  }

  void WaitDone() {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [&] { return done_; });
  }

  void DropHistory(void (*free_fn)(std::vector<GXCommand> &&)) {
    if (prev_valid_) {
      free_fn(std::move(prev_cmds_));
      prev_cmds_ = std::vector<GXCommand>();
      prev_rec_.Clear();
      prev_vh_.clear();
      prev_valid_ = false;
      prev_cam_ok_ = false;
    }
  }

  // --- region overrides (deduplicated: a region keeps its in-between value while consecutive draws want it)
  static void ReadState(int r, float *v) {
    const auto &st = gx::g_state;
    if (r < ip::kRegNrm) std::memcpy(v, &st.posMatrices[r * 4], 16);
    else if (r < ip::kRegLight) std::memcpy(v, &st.normalMatrices[(r - ip::kRegNrm) * 3], 12);
    else if (r < ip::kRegProj) std::memcpy(v, st.lights[r - ip::kRegLight], 64);
    else if (r == ip::kRegProj) std::memcpy(v, st.projection, 28);
    else if (r == ip::kRegXfCol) std::memcpy(v, &st.xf[0x0A], 16);
    else { std::memcpy(v, st.tevColorReg, 32); std::memcpy(v + 8, st.tevKonstReg, 32); }
  }
  static bool StateEquals(int r, const float *v) {
    float cur[16];
    ReadState(r, cur);
    if (r == ip::kRegTev) {
      for (int i = 0; i < 16; ++i) {
        uint32_t a, b;
        std::memcpy(&a, &cur[i], 4);
        std::memcpy(&b, &v[i], 4);
        if ((a ^ b) & 0x7FFFFF) return false;
      }
      return true;
    }
    return std::memcmp(cur, v, (size_t)ip::RegFloats(r) * 4) == 0;
  }
  static void WriteRegion(int r, const float *v) {
    if (r < ip::kRegNrm) gx::g_interp_renderer.write_xf(r * 4, v, 4);
    else if (r < ip::kRegLight) gx::g_interp_renderer.write_xf(0x400 + (r - ip::kRegNrm) * 3, v, 3);
    else if (r < ip::kRegProj) gx::g_interp_renderer.write_xf(0x600 + (r - ip::kRegLight) * 16, v, 16);
    else if (r == ip::kRegProj) gx::g_interp_renderer.write_xf(0x1020, v, 7);
    else if (r == ip::kRegXfCol) gx::g_interp_renderer.write_xf(0x100A, v, 4);
    else {
      for (int i = 0; i < 16; ++i) {
        uint32_t want, have = i < 8 ? gx::g_state.tevColorReg[i] : gx::g_state.tevKonstReg[i - 8];
        std::memcpy(&want, &v[i], 4);
        if (!((want ^ have) & 0x7FFFFF)) continue;
        want = (want & 0x7FFFFF) | (i < 8 ? 0u : 0x800000u); // the bank bit selects the register file
        gx::g_interp_renderer.write_bp((uint8_t)(0xE0 + (i & 7)), want);
      }
    }
  }
  // B's own value of region r at draw d (what the stream state is without overrides)
  const float *BValue(int r, const ip::DrawRec *d) const {
    const ip::FrameRec &B = cur_rec_;
    if (!d) return nullptr;
    if (r == ip::kRegProj) return B.Get(r, d->v_proj);
    if (r == ip::kRegTev) return B.Get(r, d->v_tev);
    if (r == ip::kRegXfCol) return B.Get(r, d->v_xfcol);
    if (r >= ip::kRegLight && r < ip::kRegProj) return B.Get(r, d->v_light[r - ip::kRegLight]);
    return B.At(r, d->seq);
  }
  void RestoreAll(const ip::DrawRec *d) {
    for (int r : active_) {
      if (StateEquals(r, ap_[r].v)) { // still our in-between value: put B's back
        if (const float *bv = BValue(r, d)) WriteRegion(r, bv);
      }
      ap_[r].on = false;
    }
    active_.clear();
  }
  void Apply(const ip::DrawRec &d, const ip::RegionWrite *ws, int nws) {
    ++stamp_;
    for (int i = 0; i < nws; ++i) want_[ws[i].r] = stamp_;
    size_t k = 0;
    for (size_t i = 0; i < active_.size(); ++i) {
      const int r = active_[i];
      if (want_[r] == stamp_ && StateEquals(r, ap_[r].v)) { active_[k++] = r; continue; }
      if (want_[r] != stamp_ && StateEquals(r, ap_[r].v)) WriteRegion(r, BValue(r, &d));
      ap_[r].on = false;
    }
    active_.resize(k);
    for (int i = 0; i < nws; ++i) {
      const ip::RegionWrite &w = ws[i];
      const int r = w.r;
      const size_t bytes = (size_t)ip::RegFloats(r) * 4;
      if (ap_[r].on && std::memcmp(ap_[r].v, w.v, bytes) == 0) continue; // already in force
      WriteRegion(r, w.v);
      std::memcpy(ap_[r].v, w.v, bytes);
      if (!ap_[r].on) active_.push_back(r);
      ap_[r].on = true;
    }
  }

  bool ShotFrame(uint64_t f) const { return !shot_prefix_.empty() && f % (uint64_t)shot_every_ == 0; }
  void Shot(const char *tag) {
    std::vector<uint8_t> px;
    if (!gx::ReadPresented(px) || px.empty()) return;
    char num[64];
    std::snprintf(num, sizeof num, "_%06llu_%s.bmp", (unsigned long long)job_no_, tag);
    gx::SaveBMP32(shot_prefix_ + num, px.data(), gx::kPresentWidth, gx::kPresentHeight, 0);
  }

  struct Window {
    uint64_t frames = 0, interp = 0, dup = 0, cut = 0, behind = 0, budget = 0, nohist = 0, unpaced = 0;
    uint64_t rec_us = 0, plan_us = 0, wait_us = 0, ri_us = 0, rn_us = 0, host_us = 0, budget_trips = 0;
    uint64_t pre_us = 0, post_us = 0; // NWII_INTERP_PROFILE
    uint64_t prep_us = 0;             // parallel per-draw preparation before the walk
    uint64_t b_persp = 0, persp_matched = 0, b_ortho = 0, ortho_matched = 0, static_draws = 0, interp_draws = 0;
    uint64_t warp = 0, nowarp = 0, warp_late = 0, warp_fail = 0, warp_us = 0; // MVGEN21
  };

  // worker handoff
  std::thread worker_;
  std::unique_ptr<gx::ParallelFor> pool_;
  ip::ParallelRun par_;
  std::mutex m_;
  std::condition_variable cv_;
  bool job_ = false, done_ = true, have_plan_ = false;
  const std::vector<GXCommand> *job_frame_ = nullptr;
  uint64_t job_no_ = 0, rec_us_ = 0, plan_us_ = 0;
  std::vector<std::shared_ptr<DrawRaw>> raws_;
  ip::FrameScan scan_;
  ip::BaseState base_{};
  ip::Config cfg_;
  ip::Plan plan_;
  ip::Camera prev_cam_;
  bool prev_cam_ok_ = false;
  // frames: B = the frame being rendered (cur_*), A = the previous one (prev_*)
  ip::FrameRec cur_rec_, prev_rec_;
  std::vector<uint64_t> cur_vh_, prev_vh_;
  std::vector<GXCommand> prev_cmds_;
  bool prev_valid_ = false, job_active_ = false, announced_ = false;
  // pacing / stats
  int64_t last_cut_us_ = 0, period_us_ = 0, raw_ema_us_ = 0, budget_skip_until_us_ = 0;
  int64_t ema_ri_us_ = 0, ema_rn_us_ = 0, t_before_n_us_ = 0;
  int64_t ri_hist_[8] = {}, rn_hist_[8] = {}, prep_hist_[8] = {}; // last 8 measured render / prepare times
  unsigned ri_i_ = 0, rn_i_ = 0, prep_i_ = 0;
  // the second-slowest of the last 8 (one outlier, e.g. a shader compile, does not pause interpolation)
  static int64_t MaxOf(const int64_t (&h)[8]) {
    int64_t t[8];
    std::copy(std::begin(h), std::end(h), t);
    std::nth_element(t, t + 6, t + 8);
    return t[6];
  }
  bool budget_trial_ = false, trial_render_ = false, trial_next_ = false; // after a budget pause: one trial frame
  gx::InterpFrameInfo info_;
  std::atomic<bool> display_ok_{true};
  Window win_;
  gx::InterpRenderStats last_ist_;
  gx::RendererStats last_rs_;
  const char *last_cut_reason_ = "";
  // per-draw state inside RenderInterpFrame
  const std::vector<GXCommand> *b_cmds_ = nullptr;
  const GXCommand *acmd_ = nullptr;
  size_t di_ = 0;
  ip::DrawOverride ov_;
  ip::PreparedFrame prep_;
  ip::BankCache bank_;
  struct Applied { bool on = false; float v[16]; };
  Applied ap_[ip::kRegCount];
  uint32_t want_[ip::kRegCount] = {};
  uint32_t stamp_ = 0;
  std::vector<int> active_;
  // config from env
  float t_ = 0.5f;
  int refine_ = 2;
  bool vcolor_ = false, guards_ = true, log_ = false, force_ = false, prof_ = false, kmode_ = true;
  std::string shot_prefix_;
  int shot_every_ = 60;
  // MVGEN21
  bool mv_on_ = false, warp_this_ = false, record_ = false;
  int64_t last_warp_us_ = 0;
  size_t boundary_ = 0;
  gx::IRenderer *r_ = nullptr;
  const std::vector<GXCommand> *mframe_ = nullptr, *mframe_cand_ = nullptr; // the armed real frame's commands
  size_t mdi_ = 0;
  motion::PrevCache pc_;
  motion::AutoDecider auto_;
  int64_t warp_hist_[8] = {};
  unsigned warp_i_ = 0, warp_n_ = 0;
  int test_stride_ = 1, test_phase_ = 0, warp_flags_ = -1;
  gx::WarpStats last_ws_;
};

} // namespace interp17

namespace nwii::runtime::gx {
InterpFrameInfo InterpLastFrameInfo() { return interp17::Pipeline::Get().LastInfo(); }
void InterpSetDisplayOk(bool ok) { interp17::Pipeline::Get().SetDisplayOk(ok); }
std::string InterpStatsLine() { return interp17::Pipeline::Get().StatsLine(); }
} // namespace nwii::runtime::gx
