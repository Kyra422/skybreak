#pragma once
// GXF12: present thread (used by main.cpp; smoke-tested by
// outputs/pc-tests/gxf12/present-smoke.cpp).
//
// Measured (work/claude-notes/GXF12.md): SDL_GL_SwapWindow with vsync blocks
// ~30 ms per call on this desktop, and the host loop ran it in series with GX
// (~36 ms per Skyloft frame), so the window showed 14-16 of the game's 30
// frames/s. With the presenter, the host thread renders every guest frame,
// copies the present image into a ring slot (a texture shared with a second
// GL context) and queues it; the present thread blits queued slots to the
// window in frame order and swaps (vsync as configured), so a blocking swap
// no longer holds up GX.
//
// Threading rules (the first version deadlocked on the AMD driver with both
// contexts current on the window's DC and GPU-side glWaitSync between them):
//  - Only the present thread makes a context current on the visible window.
//    The host context must be current on another drawable (the caller makes it
//    current on a hidden window of the same pixel format: SetHostDrawable).
//  - Cross-context syncs are waited on the CPU with glClientWaitSync and a
//    timeout; the creating context flushes right after glFenceSync. A wait
//    that times out drops that frame (counted), it never blocks forever.
//
// Pacing (NWII_PRESENT_PACING=dwm|vsync|none). A vsync swap (interval 1) on
// the present context also stalls the GPU queue the host renders into: host
// GL time per Skyloft/title frame rose from ~15 to ~23 ms and the GPU time per
// frame from ~9 to ~13-16 ms (GXF12.md). With vsync requested the default is
// therefore "dwm": swap interval 0, then DwmFlush() on the present thread, which
// waits (on the CPU) for the next desktop composition - one present per
// composition, no tearing in a composed window. In fullscreen (where the
// window may bypass composition) and without dwmapi it falls back to interval 1.
#include <SDL.h>
#include <glad/glad.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <algorithm>
#include <vector>

#include "runtime/gx/frame_pipeline.h"

namespace gxf12 {

// INTERP17: when a queued frame may be shown (frame interpolation, 60 fps from the game's 30).
//   kind 0: at once (as before; t_cut_us only feeds the latency statistics when set)
//   kind 1: an in-between frame I of guest frame N: at t_cut + D
//   kind 2: the real frame N on the interpolation schedule: at t_cut + D + P/2 (with or without an I
//           before it: without one, N-1 simply stays on screen for two 60 Hz periods)
// D = p90 of when the frames were ready after their cut (last 32), so the presents are spaced P/2 apart
// even when the host's time per frame jitters. Times: steady_clock microseconds.
struct PresentTiming {
  int kind = 0;
  int64_t t_cut_us = 0;
  int64_t period_us = 0;
};
inline int64_t SteadyUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
// Present statistics since the last TakePaceStats (present thread fills, host reads).
struct PaceStats {
  uint64_t presents = 0, interp = 0, real_paced = 0, late = 0;
  std::vector<int> lat_i_us, lat_n_us, lat_plain_us; // cut -> present (swap + DwmFlush returned)
  std::vector<int> gap_us;                           // between consecutive presents
  int64_t d_us = 0;
};

// Window blit rect for the present image: dst = {x, y, w, h} in a dw x dh
// drawable (GL origin bottom-left). Called on the present thread.
using FitFn = void (*)(int dw, int dh, bool wide, int aspect, int dst[4]);

class Presenter {
public:
  ~Presenter() { Stop(); }
  bool on() const { return thread_.joinable(); }

  // Caller thread, host context current on `host_drawable` (NOT `win`).
  bool Start(SDL_Window *win, SDL_Window *host_drawable, SDL_GLContext host_ctx, int swap_interval,
             FitFn fit) {
    win_ = win;
    host_win_ = host_drawable;
    host_ctx_ = host_ctx;
    si_ = swap_interval;
    fit_ = fit;
    {
      const char *e = std::getenv("NWII_PRESENT_PACING");
      const std::string m = e ? e : "";
      pacing_ = m == "vsync" ? kVsync : m == "none" ? kNone : m == "dwm" ? kDwm
              : (si_ == 1 ? kDwm : kVsync); // kVsync = the configured interval as is
      if (pacing_ == kDwm) {
        if (void *lib = SDL_LoadObject("dwmapi.dll"))
          dwm_flush_ = (long(__stdcall *)())SDL_LoadFunction(lib, "DwmFlush");
        if (!dwm_flush_)
          pacing_ = kVsync;
      }
    }
    MeasureDisplay(); // INTERP17: composition rate (frame interpolation needs >= 50 Hz)
    if (const char *e = std::getenv("NWII_GXF12_TRACE"); e && *e && *e != '0')
      trace_ = 8;
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    ctx_ = SDL_GL_CreateContext(host_drawable); // becomes current on this thread
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    const bool back = SDL_GL_MakeCurrent(host_drawable, host_ctx) == 0;
    if (!ctx_ || !back) {
      std::printf("[GXF12] present thread off: shared context failed: %s\n", SDL_GetError());
      if (ctx_)
        SDL_GL_DeleteContext(ctx_);
      ctx_ = nullptr;
      SDL_GL_MakeCurrent(host_drawable, host_ctx);
      return false;
    }
    thread_ = std::thread([this] { Loop(); });
    std::unique_lock<std::mutex> lk(m_);
    if (!cv_.wait_for(lk, std::chrono::seconds(5), [&] { return init_state_ != 0; }) ||
        init_state_ < 0) {
      const bool hung = init_state_ == 0;
      stop_ = true;
      lk.unlock();
      cv_.notify_all();
      if (hung) {
        thread_.detach(); // stuck in the driver; leak it rather than hang here
        std::printf("[GXF12] present thread off: init timed out\n");
      } else {
        thread_.join();
        SDL_GL_DeleteContext(ctx_);
        std::printf("[GXF12] present thread off: %s\n", init_error_.c_str());
      }
      ctx_ = nullptr;
      return false;
    }
    std::printf("[GXF12] present thread on (swap interval %d, pacing %s, %d slots)\n", si_,
                pacing_ == kDwm ? "dwm" : pacing_ == kNone ? "none" : "vsync", ring_.slots());
    std::fflush(stdout);
    return true;
  }

  // Host thread, host context current: queue the present image `src` (an FBO
  // of pw x ph) as guest frame `frame`. INTERP17: `timing` = when it may be shown (null = at once).
  void Submit(unsigned src, int pw, int ph, uint64_t frame, bool wide, int aspect,
              const PresentTiming *timing = nullptr) {
    if (!src || pw <= 0 || ph <= 0)
      return;
    int s = -1;
    bool stolen = false;
    GLsync old_ready = nullptr, done = nullptr;
    {
      std::lock_guard<std::mutex> lk(m_);
      s = ring_.Acquire(&stolen, nullptr);
      if (s >= 0) {
        old_ready = slot_[s].ready;
        slot_[s].ready = nullptr;
        done = slot_[s].done;
        slot_[s].done = nullptr;
      }
    }
    if (s < 0) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (stolen)
      dropped_.fetch_add(1, std::memory_order_relaxed);
    if (old_ready)
      glDeleteSync(old_ready);
    if (done) { // the window blit that last read this slot (flushed by the present thread)
      if (!ClientWait(done, "host: slot reuse"))
        timeouts_.fetch_add(1, std::memory_order_relaxed);
      glDeleteSync(done);
    }
    Slot &sl = slot_[s];
    if (!sl.tex || sl.w != pw || sl.h != ph) {
      if (!sl.tex)
        glGenTextures(1, &sl.tex);
      glBindTexture(GL_TEXTURE_2D, sl.tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindTexture(GL_TEXTURE_2D, 0);
      if (!sl.fbo_host)
        glGenFramebuffers(1, &sl.fbo_host);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sl.fbo_host);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sl.tex, 0);
      sl.w = pw;
      sl.h = ph;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sl.fbo_host);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, pw, ph, 0, 0, pw, ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLsync ready = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush(); // the fence must reach the GPU before the other context waits on it
    GLsync freed_ready = nullptr;
    {
      std::lock_guard<std::mutex> lk(m_);
      sl.ready = ready;
      sl.wide = wide;
      sl.aspect = aspect;
      sl.tm = timing ? *timing : PresentTiming{}; // INTERP17
      sl.t_submit_us = SteadyUs();
      ring_.set_max_queued(sl.tm.kind ? 4 : 2); // INTERP17: I and N may wait for their present time
      const int o = ring_.Queue(s, frame, nullptr);
      if (o >= 0) { // the window is more than 2 frames behind: drop the oldest queued
        freed_ready = slot_[o].ready;
        slot_[o].ready = nullptr;
        dropped_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (freed_ready)
      glDeleteSync(freed_ready);
    if (trace_ > 0) {
      std::printf("[GXF12 trace] host submit frame %llu slot %d\n", (unsigned long long)frame, s);
      std::fflush(stdout);
    }
    submitted_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
  }

  // Caller thread, host context current.
  void Stop() {
    if (!thread_.joinable())
      return;
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
    for (auto &sl : slot_) {
      if (sl.ready) glDeleteSync(sl.ready);
      if (sl.done) glDeleteSync(sl.done);
      if (sl.fbo_host) glDeleteFramebuffers(1, &sl.fbo_host);
      if (sl.tex) glDeleteTextures(1, &sl.tex);
      sl = Slot{};
    }
    if (ctx_)
      SDL_GL_DeleteContext(ctx_);
    ctx_ = nullptr;
  }

  uint64_t submitted() const { return submitted_.load(std::memory_order_relaxed); }
  uint64_t presented() const { return presented_.load(std::memory_order_relaxed); }
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  uint64_t timeouts() const { return timeouts_.load(std::memory_order_relaxed); }
  uint64_t swap_us() const { return swap_us_.load(std::memory_order_relaxed); }
  uint64_t last_presented_frame() const { return last_frame_.load(std::memory_order_relaxed); }
  // INTERP17: desktop composition rate (DwmGetCompositionTimingInfo) or the display mode's refresh rate.
  double composition_hz() const { return comp_hz_; }
  const char *composition_source() const { return comp_src_; }
  PaceStats TakePaceStats() {
    std::lock_guard<std::mutex> lk(pm_);
    PaceStats r = std::move(ps_);
    ps_ = PaceStats{};
    r.d_us = d_us_;
    return r;
  }

private:
  struct Slot {
    GLuint tex = 0;      // shared by both contexts
    GLuint fbo_host = 0; // host context: copy target
    int w = 0, h = 0;
    GLsync ready = nullptr; // host copy done
    GLsync done = nullptr;  // window blit done reading
    bool wide = true;
    int aspect = 0;
    PresentTiming tm;         // INTERP17
    int64_t t_submit_us = 0;  // INTERP17
  };

  // INTERP17: composition period from DWM (windowed) or the display mode (fallback).
  void MeasureDisplay() {
    comp_hz_ = 0;
    comp_src_ = "none";
    if (void *lib = SDL_LoadObject("dwmapi.dll")) {
      using Fn = long(__stdcall *)(void *, void *);
      if (auto fn = (Fn)SDL_LoadFunction(lib, "DwmGetCompositionTimingInfo")) {
        alignas(8) unsigned char ti[512] = {};
        const uint32_t cb = 292; // sizeof(DWM_TIMING_INFO), #pragma pack(1)
        std::memcpy(ti, &cb, 4);
        if (fn(nullptr, ti) == 0) {
          uint64_t qpc = 0;
          std::memcpy(&qpc, ti + 12, 8); // qpcRefreshPeriod
          const double f = (double)SDL_GetPerformanceFrequency();
          if (qpc && f > 0) {
            comp_hz_ = f / (double)qpc;
            comp_src_ = "DWM";
          }
        }
      }
    }
    SDL_DisplayMode dm{};
    if (win_ && SDL_GetWindowDisplayMode(win_, &dm) == 0 && dm.refresh_rate > 0) {
      mode_hz_ = dm.refresh_rate;
      const bool fullscreen = (SDL_GetWindowFlags(win_) & SDL_WINDOW_FULLSCREEN) != 0;
      if (comp_hz_ <= 0 || fullscreen) {
        comp_hz_ = dm.refresh_rate;
        comp_src_ = "display mode";
      }
    }
    if (void *k = SDL_LoadObject("kernel32.dll")) {
      create_timer_ = (void *(__stdcall *)(void *, const wchar_t *, unsigned long, unsigned long))SDL_LoadFunction(
          k, "CreateWaitableTimerExW");
      set_timer_ = (int(__stdcall *)(void *, const long long *, long, void *, void *, int))SDL_LoadFunction(
          k, "SetWaitableTimer");
      wait_obj_ = (unsigned long(__stdcall *)(void *, unsigned long))SDL_LoadFunction(k, "WaitForSingleObject");
    }
  }
  // INTERP17: high-resolution wait (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION; the default timer tick is 15.6 ms).
  void WaitUntilUs(int64_t t) {
    if (!hr_timer_ && create_timer_ && !hr_failed_) {
      hr_timer_ = create_timer_(nullptr, nullptr, 0x2 /*HIGH_RESOLUTION*/, 0x1F0003 /*TIMER_ALL_ACCESS*/);
      hr_failed_ = hr_timer_ == nullptr;
    }
    const int64_t stop = SteadyUs() + 250000; // never hold a frame longer than 250 ms
    for (;;) {
      const int64_t now = SteadyUs();
      const int64_t left = t - now;
      if (left <= 0 || now >= stop)
        return;
      if (left > 1500 && hr_timer_ && set_timer_ && wait_obj_) {
        const long long due = -(long long)(left - 1000) * 10; // relative, 100 ns units
        if (set_timer_(hr_timer_, &due, 0, nullptr, nullptr, 0))
          wait_obj_(hr_timer_, 200);
      } else if (left > 1500) {
        std::this_thread::sleep_for(std::chrono::microseconds(left - 1000));
      } else {
        std::this_thread::yield();
      }
    }
  }

  // CPU wait (<= 1 s in 50 ms steps) on a fence another context flushed.
  bool ClientWait(GLsync s, const char *who) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
      const GLenum r = glClientWaitSync(s, 0, 50000000ull);
      if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        if (ms > 100.0)
          std::printf("[GXF12] slow sync wait %.0f ms (%s)\n", ms, who);
        return true;
      }
      if (r == GL_WAIT_FAILED)
        break;
    }
    std::printf("[GXF12] sync wait failed/timed out (%s)\n", who);
    std::fflush(stdout);
    return false;
  }

  void Loop() {
    if (SDL_GL_MakeCurrent(win_, ctx_) != 0) {
      std::lock_guard<std::mutex> lk(m_);
      init_error_ = std::string("MakeCurrent: ") + SDL_GetError();
      init_state_ = -1;
      cv_.notify_all();
      return;
    }
    int interval = -2; // set per frame below (pacing)
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    {
      std::lock_guard<std::mutex> lk(m_);
      init_state_ = 1;
    }
    cv_.notify_all();
    GLuint pres_fbo[nwii::runtime::gx::PresentRing::kMaxSlots] = {};
    for (;;) {
      int s = -1;
      GLsync ready = nullptr;
      Slot sl;
      uint64_t frame = 0;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return stop_ || ring_.queued() > 0; });
        if (stop_)
          break;
        s = ring_.Pop();
        ready = slot_[s].ready;
        slot_[s].ready = nullptr;
        sl = slot_[s];
        frame = ring_.frame(s);
      }
      if (trace_ > 0)
        std::printf("[GXF12 trace] present pop frame %llu slot %d\n", (unsigned long long)frame, s);
      bool ok = true;
      bool was_ready = true; // INTERP17: the host copy had already finished when popped
      if (ready) {
        const GLenum q = glClientWaitSync(ready, 0, 0);
        was_ready = q == GL_ALREADY_SIGNALED || q == GL_CONDITION_SATISFIED;
        ok = was_ready || ClientWait(ready, "present: host copy");
        glDeleteSync(ready);
      }
      if (!ok) {
        timeouts_.fetch_add(1, std::memory_order_relaxed);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(m_);
        ring_.Release(s);
        continue;
      }
      if (!pres_fbo[s])
        glGenFramebuffers(1, &pres_fbo[s]);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, pres_fbo[s]);
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sl.tex, 0);
      int dw = 0, dh = 0;
      SDL_GL_GetDrawableSize(win_, &dw, &dh);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      glViewport(0, 0, dw, dh);
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      int dst[4] = {0, 0, dw, dh};
      if (fit_)
        fit_(dw, dh, sl.wide, sl.aspect, dst);
      if (dst[2] > 0 && dst[3] > 0 && dw > 0 && dh > 0)
        glBlitFramebuffer(0, 0, sl.w, sl.h, dst[0], dst[1], dst[0] + dst[2], dst[1] + dst[3],
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
      GLsync done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      glFlush();
      {
        std::lock_guard<std::mutex> lk(m_);
        if (slot_[s].done)
          glDeleteSync(slot_[s].done);
        slot_[s].done = done;
        ring_.Release(s);
      }
      if (trace_ > 0)
        std::printf("[GXF12 trace] present swap frame %llu\n", (unsigned long long)frame);
      const bool fullscreen = (SDL_GetWindowFlags(win_) & SDL_WINDOW_FULLSCREEN) != 0;
      const bool dwm = pacing_ == kDwm && !fullscreen;
      const int want = dwm || pacing_ == kNone ? 0 : si_;
      if (want != interval) {
        if (SDL_GL_SetSwapInterval(want) != 0 && want == -1)
          SDL_GL_SetSwapInterval(0); // adaptive vsync unsupported
        interval = SDL_GL_GetSwapInterval();
      }
      if (sl.tm.kind) { // INTERP17: wait for this frame's present time
        const int64_t now = SteadyUs();
        if (!was_ready)
          lag_us_ = (lag_us_ * 7 + (now - sl.t_submit_us)) / 8;
        const int64_t ready_us = was_ready ? sl.t_submit_us + lag_us_ : now;
        const int64_t target = pacer_.Plan(sl.tm.kind, sl.tm.t_cut_us, sl.tm.period_us, ready_us);
        d_us_ = pacer_.D();
        const int64_t comp = comp_hz_ > 1 ? (int64_t)(1e6 / comp_hz_) : 16667;
        int queued = 0;
        { std::lock_guard<std::mutex> lk(m_); queued = ring_.queued(); }
        if (nwii::runtime::gx::DropLatePresent(now, target, comp, queued)) {
          dropped_.fetch_add(1, std::memory_order_relaxed);
          ++late_;
          continue; // slot and its GPU read fence were released above
        }
        WaitUntilUs(target - nwii::runtime::gx::PresentLeadUs(comp, interval, dwm));
        if (SteadyUs() > target + comp)
          ++late_;
      }
      const auto t0 = std::chrono::steady_clock::now();
      SDL_GL_SwapWindow(win_);
      if (dwm)
        dwm_flush_(); // CPU wait for the next desktop composition
      const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t0).count();
      swap_us_.fetch_add(us, std::memory_order_relaxed);
      if (trace_ > 0) {
        std::printf("[GXF12 trace] present swapped frame %llu (%.1f ms)\n",
                    (unsigned long long)frame, us / 1000.0);
        std::fflush(stdout);
        --trace_;
      }
      presented_.fetch_add(1, std::memory_order_relaxed);
      last_frame_.store(frame, std::memory_order_relaxed);
      { // INTERP17: latency and cadence statistics
        const int64_t t = SteadyUs();
        std::lock_guard<std::mutex> lk(pm_);
        ++ps_.presents;
        if (last_present_us_ && ps_.gap_us.size() < 4096)
          ps_.gap_us.push_back((int)(t - last_present_us_));
        last_present_us_ = t;
        if (sl.tm.t_cut_us) {
          auto &v = sl.tm.kind == 1 ? ps_.lat_i_us : sl.tm.kind == 2 ? ps_.lat_n_us : ps_.lat_plain_us;
          if (v.size() < 4096)
            v.push_back((int)(t - sl.tm.t_cut_us));
        }
        ps_.interp += sl.tm.kind == 1;
        ps_.real_paced += sl.tm.kind == 2;
        ps_.late += late_;
        late_ = 0;
      }
    }
    for (GLuint &f : pres_fbo)
      if (f) {
        glDeleteFramebuffers(1, &f);
        f = 0;
      }
    glFinish();
    SDL_GL_MakeCurrent(win_, nullptr);
  }

  SDL_Window *win_ = nullptr, *host_win_ = nullptr;
  SDL_GLContext ctx_ = nullptr, host_ctx_ = nullptr;
  int si_ = 1;
  FitFn fit_ = nullptr;
  enum Pacing { kVsync, kDwm, kNone } pacing_ = kVsync;
  long(__stdcall *dwm_flush_)() = nullptr;
  int trace_ = 0;
  std::thread thread_;
  std::mutex m_;
  std::condition_variable cv_;
  bool stop_ = false;
  int init_state_ = 0; // 1 ok, -1 failed
  std::string init_error_;
  nwii::runtime::gx::PresentRing ring_{6, 2}; // INTERP17: 6 slots (<= 4 queued while interpolating)
  Slot slot_[6];
  // INTERP17 pacing (present thread) and statistics
  double comp_hz_ = 0;
  int mode_hz_ = 0;
  const char *comp_src_ = "none";
  void *(__stdcall *create_timer_)(void *, const wchar_t *, unsigned long, unsigned long) = nullptr;
  int(__stdcall *set_timer_)(void *, const long long *, long, void *, void *, int) = nullptr;
  unsigned long(__stdcall *wait_obj_)(void *, unsigned long) = nullptr;
  void *hr_timer_ = nullptr;
  bool hr_failed_ = false;
  nwii::runtime::gx::PacePlanner pacer_;
  int64_t lag_us_ = 2000, d_us_ = 20000, last_present_us_ = 0;
  uint64_t late_ = 0;
  std::mutex pm_;
  PaceStats ps_;
  std::atomic<uint64_t> submitted_{0}, presented_{0}, dropped_{0}, timeouts_{0}, swap_us_{0},
      last_frame_{0};
};

} // namespace gxf12
