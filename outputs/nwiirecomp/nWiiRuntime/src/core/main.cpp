#include "input/input_manager.h"
#include "input/sources/gamepad_source.h"
#include "input/sources/mouse_keyboard_source.h"
#include "input/sources/phone_source.h"
#include "loader/loader.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/event_scheduler.h" // CLOCK15
#include "runtime/guest_clock.h"     // CLOCK15
#include "runtime/virtual_disc.h"
#include "runtime/nand_fs.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <SDL.h>
#include <glad/glad.h>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/gx_state.h"
#include "runtime/gx/gx_capture.h"
#include "runtime/gx/renderer.h"
#include "runtime/gx/frame_pipeline.h"
#include "runtime/gx/hd_textures.h" // HDTEX16
#include "core/gxf12_presenter.h"
#include "runtime/gx/interp_render.h" // INTERP17: frame interpolation (in-between frames)
#include "runtime/interp_config.h"    // INTERP17
#include "ui/ui_hooks.h" // UI13: launcher, settings, in-game menu
#include "runtime/hw/hw.h"
#include "runtime/profiler.h"

namespace nwii::runtime {
    namespace hw { extern uint32_t g_vi_top_field_base; }
}
namespace prof = nwii::runtime::prof;

// PERF2 (roadmap M2.6): presentation and VI are decoupled from the host loop.
//  - VI interrupts come only from the virtual-time scheduler (ios.cpp, one VI
//    per tb_freq/60 timebase ticks), as in Dolphin. The host loop used to raise
//    an extra VI per iteration (vblank_pending), which tied the guest's VI rate
//    to the host's swap rate (33 Hz here, 165 Hz elsewhere; roadmap M2.6).
//    NWII_HOST_VI=1 restores that extra VI.
//  - SDL_GL_SwapWindow blocked ~30 ms per call with swap interval 1 on this
//    machine, and the loop presented every iteration, so GX processing and the
//    vsync wait were serialised (PERF2 profile: host loop 13-18 it/s; the
//    swap time depends on which display the window lands on: 30 ms in some
//    runs, 5-7 ms in others). NWII_SWAP_INTERVAL=0 (or -1, adaptive) presents
//    only frames the cutter produced, without vsync. The default stays 1
//    (vsync, every iteration presents, as before): with interval 0 the busier
//    host loop coincided with a 30-45% lower CPU-thread instruction rate in
//    the PERF2 A/B (runs 181727 vs 182253), so it is opt-in.
namespace {
struct HostPacing {
  bool host_vi = false;
  int swap_interval = 1;
  bool swap_from_env = false; // else [graphics] vsync decides (RES9)
};
HostPacing load_host_pacing() {
  HostPacing h;
  if (const char *e = std::getenv("NWII_HOST_VI")) h.host_vi = e[0] == '1';
  if (const char *e = std::getenv("NWII_SWAP_INTERVAL")) {
    h.swap_interval = std::atoi(e);
    h.swap_from_env = true;
  }
  if (h.swap_interval < -1 || h.swap_interval > 1) h.swap_interval = 1;
  return h;
}
HostPacing g_host_pacing = load_host_pacing();
} // namespace

// RES9 (roadmap M7.3): internal resolution, window/fullscreen, aspect-correct
// present and screenshots.
//  - [graphics] internal_scale s (1..6, 0 = auto: 2 unless the GL renderer is a
//    software one or the limits forbid it; env NWII_INTERNAL_SCALE) sizes the
//    EFB FBO at 640s x 528s. The renderer derives s from that FBO (renderer.h).
//  - The window is resizable; F11 or Alt+Enter toggles borderless fullscreen
//    ([graphics] fullscreen / env NWII_FULLSCREEN start state). Automated runs
//    (NWII_RUN_SECONDS set) start windowed unless NWII_FULLSCREEN=1.
//  - Present: the 640s x 480s XFB image is fitted into the window at 16:9
//    when the game runs in widescreen (SYSCONF IPL.AR) or 4:3 otherwise, with
//    black bars ([graphics] aspect overrides: "4:3", "16:9", "stretch").
//  - NWII_SCREENSHOT shots stay 640x480 (box-filtered at s > 1;
//    NWII_SCREENSHOT_NATIVE=0 writes the full-size image instead). F12 saves a
//    full-resolution BMP to [graphics] screenshot_dir.
//  - NWII_GPU_MS=1 (diagnostic): GPU time per presented frame (GL timer
//    queries), printed as [RES9] lines every 5 s.
namespace res9 {

int ChooseInternalScale() {
  const auto &cfg = nwii::runtime::Config::get();
  int s = cfg.internal_scale;
  const char *src = "config";
  if (const char *e = std::getenv("NWII_INTERNAL_SCALE"); e && *e) {
    s = std::atoi(e);
    src = "env NWII_INTERNAL_SCALE";
  }
  GLint max_tex = 0, max_rb = 0, max_vp[2] = {0, 0};
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
  glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
  glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_vp);
  int limit_w = std::min<int>(max_tex, max_vp[0]) / nwii::runtime::gx::kEfbWidth;
  int limit_h = std::min<int>(max_tex, max_vp[1]) / nwii::runtime::gx::kEfbHeight;
  int limit = std::clamp(std::min(limit_w, limit_h), 1, nwii::runtime::gx::kMaxInternalScale);
  if (s <= 0) {
    const char *r = (const char *)glGetString(GL_RENDERER);
    const std::string rs = r ? r : "";
    const bool software = rs.find("llvmpipe") != std::string::npos ||
                          rs.find("softpipe") != std::string::npos ||
                          rs.find("Software") != std::string::npos ||
                          rs.find("GDI Generic") != std::string::npos ||
                          rs.find("Basic Render") != std::string::npos;
    s = software ? 1 : 2;
    src = software ? "auto (software GL renderer)" : "auto";
  }
  const int chosen = std::clamp(s, 1, limit);
  std::cout << "[RES9] internal scale " << chosen << " (" << src << ", requested " << s
            << ", GL limit " << limit << ")" << std::endl;
  return chosen;
}

// EFB FBO: RGBA8 colour texture + DEPTH24 texture (sampled by EFB copies) of
// (640 x 528) * s. False (and nothing left allocated) if incomplete.
bool CreateEfb(int s, GLuint &fbo, GLuint &color, GLuint &depth) {
  const int w = nwii::runtime::gx::kEfbWidth * s, h = nwii::runtime::gx::kEfbHeight * s;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glGenTextures(1, &depth);
  glBindTexture(GL_TEXTURE_2D, depth);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT,
               GL_UNSIGNED_INT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
  const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  std::cout << "[GL] EFB FBO " << w << "x" << h << " status: 0x" << std::hex << st << std::dec
            << (st == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : " (INCOMPLETE!)") << std::endl;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (st == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR)
    return true;
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &color);
  glDeleteTextures(1, &depth);
  fbo = color = depth = 0;
  return false;
}

// The game's widescreen mode for aspect "auto": SYSCONF IPL.AR (what
// SCGetAspectRatio returns), read once from the host NAND after the first
// frame (the game has booted ES/NAND by then). Before that, and if the read
// fails: [nand] widescreen, whose default (-1) means the default SYSCONF,
// which is 16:9.
bool GameWidescreen(uint64_t frames) {
  static int known = -1;
  if (known >= 0)
    return known != 0;
  const int cfg = nwii::runtime::Config::get().nand_widescreen;
  const bool fallback = cfg != 0;
  if (frames == 0)
    return fallback;
  std::vector<uint8_t> sc;
  size_t off = 0, size = 0;
  if (nwii::runtime::nand::instance().read_whole_file("/shared2/sys/SYSCONF", sc) &&
      nwii::runtime::nand::sysconf_find_item(sc, "IPL.AR", &off, &size) && size == 1 &&
      off < sc.size()) {
    known = sc[off] ? 1 : 0;
    std::cout << "[RES9] game aspect from SYSCONF IPL.AR=" << known << ": "
              << (known ? "16:9" : "4:3") << std::endl;
  } else {
    known = fallback ? 1 : 0;
    std::cout << "[RES9] SYSCONF IPL.AR unreadable, aspect " << (known ? "16:9" : "4:3")
              << " from [nand] widescreen" << std::endl;
  }
  return known != 0;
}

// Destination rect of the present image in a draw_w x draw_h drawable.
void FitRect(int draw_w, int draw_h, bool widescreen, int mode, int r[4]) {
  r[0] = 0; r[1] = 0; r[2] = draw_w; r[3] = draw_h;
  if (mode == 3 || draw_w <= 0 || draw_h <= 0)
    return; // stretch
  const bool wide = mode == 2 || (mode == 0 && widescreen);
  const double ar = wide ? 16.0 / 9.0 : 4.0 / 3.0;
  int w = draw_w, h = (int)std::lround(draw_w / ar);
  if (h > draw_h) {
    h = draw_h;
    w = (int)std::lround(draw_h * ar);
  }
  r[0] = (draw_w - w) / 2;
  r[1] = (draw_h - h) / 2;
  r[2] = w;
  r[3] = h;
}

std::filesystem::path ScreenshotDir() {
  const std::string &cfg = nwii::runtime::Config::get().screenshot_dir;
  if (!cfg.empty())
    return std::filesystem::path(cfg);
  std::filesystem::path base;
  if (char *bp = SDL_GetBasePath()) {
    base = std::filesystem::path(bp);
    SDL_free(bp);
  } else {
    base = std::filesystem::current_path();
  }
  std::error_code ec;
  const auto results = base / ".." / ".." / "pc-testresultaten";
  if (std::filesystem::is_directory(results, ec))
    return std::filesystem::weakly_canonical(results / "screenshots", ec);
  return base / "screenshots";
}

// F12: the presented image at full internal resolution, as an opaque BMP.
void SaveFullScreenshot(uint64_t frame) {
  std::vector<unsigned char> px;
  int w = 0, h = 0;
  if (!nwii::runtime::gx::ReadPresentedFull(px, w, h)) {
    std::cout << "[RES9] F12: nothing presented yet" << std::endl;
    return;
  }
  std::error_code ec;
  const auto dir = ScreenshotDir();
  std::filesystem::create_directories(dir, ec);
  char name[96];
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  std::snprintf(name, sizeof name, "skyward-%04d%02d%02d-%02d%02d%02d-%06llu.bmp",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                tm.tm_sec, (unsigned long long)frame);
  const auto path = dir / name;
  const bool ok = nwii::runtime::gx::SaveBMP32(path.string(), px.data(), w, h, 0);
  std::cout << "[RES9] F12 screenshot " << w << "x" << h << " -> " << path.string()
            << (ok ? "" : " (WRITE FAILED)") << std::endl;
}

bool IsFullscreen(SDL_Window *w) { return (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0; }

void ToggleFullscreen(SDL_Window *w) {
  const bool to_fs = !IsFullscreen(w);
  if (SDL_SetWindowFullscreen(w, to_fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0)
    std::cout << "[RES9] fullscreen toggle failed: " << SDL_GetError() << std::endl;
  else
    std::cout << "[RES9] " << (to_fs ? "borderless fullscreen" : "windowed") << std::endl;
  int ww = 0, wh = 0;
  SDL_GetWindowSize(w, &ww, &wh);
  if (ww > 0 && wh > 0) {
    nwii::runtime::Config::get().window_width = ww;
    nwii::runtime::Config::get().window_height = wh;
  }
}

// NWII_GPU_MS=1: GL_TIME_ELAPSED around GX processing + present, read back
// a few frames later (no stall); mean/max per 5 s.
struct GpuTimer {
  bool on = false;
  GLuint q[4] = {};
  bool pending[4] = {};
  bool counts[4] = {}; // the bracket rendered a new frame
  int cur = 0;
  bool open = false;
  double sum_ms = 0, max_ms = 0;
  uint64_t n = 0;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  void init() {
    const char *e = std::getenv("NWII_GPU_MS");
    on = e && *e && *e != '0';
    if (on) glGenQueries(4, q);
  }
  void begin() {
    if (!on || open)
      return;
    if (pending[cur])
      collect(cur, true);
    glBeginQuery(GL_TIME_ELAPSED, q[cur]);
    open = true;
  }
  void end(bool new_frame) {
    if (!on || !open)
      return;
    glEndQuery(GL_TIME_ELAPSED);
    pending[cur] = true;
    counts[cur] = new_frame;
    open = false;
    cur = (cur + 1) & 3;
    for (int i = 0; i < 4; ++i)
      if (pending[i])
        collect(i, false);
    const auto now = std::chrono::steady_clock::now();
    if (now - t0 >= std::chrono::seconds(5)) {
      std::ostringstream os;
      os << std::dec << "[RES9] gpu_ms mean=" << (n ? sum_ms / n : 0.0) << " max=" << max_ms
         << " frames=" << n << " scale=" << nwii::runtime::gx::InternalScale() << "\n";
      std::cout << os.str() << std::flush;
      sum_ms = max_ms = 0;
      n = 0;
      t0 = now;
    }
  }
  void collect(int i, bool wait) {
    GLint avail = 0;
    if (!wait) {
      glGetQueryObjectiv(q[i], GL_QUERY_RESULT_AVAILABLE, &avail);
      if (!avail)
        return;
    }
    GLuint64 ns = 0;
    glGetQueryObjectui64v(q[i], GL_QUERY_RESULT, &ns);
    pending[i] = false;
    if (!counts[i])
      return;
    const double ms = ns / 1e6;
    sum_ms += ms;
    max_ms = std::max(max_ms, ms);
    ++n;
  }
};


// NWII_RES9_SELFTEST=<dir> (diagnostic): drives the window code through SDL
// itself once the title is up: resize wide and tall (pillarbox/letterbox),
// F11 (borderless fullscreen), Alt+Enter (back to a window), F12 (full-size
// screenshot), restoring the window size in between. After each step it saves
// the window back buffer (what the player sees) as <dir>/win_<step>.bmp and
// logs the drawable size, the image rect and a bar/centre pixel. Starts at
// game frame NWII_RES9_SELFTEST_FRAME (default 780, the title screen).
struct SelfTest {
  std::string dir;
  int step = -1;
  uint64_t next = 0;
  void init() {
    const char *e = std::getenv("NWII_RES9_SELFTEST");
    if (!e || !*e)
      return;
    dir = e;
    step = 0;
    next = 780; // the title screen (boot-title / new-game replays)
    if (const char *f = std::getenv("NWII_RES9_SELFTEST_FRAME"); f && *f)
      next = std::strtoull(f, nullptr, 10);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
  }
  static void push_key(SDL_Keycode k, uint16_t mod) {
    SDL_Event ev{};
    ev.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = k;
    ev.key.keysym.mod = mod;
    SDL_PushEvent(&ev);
  }
  void capture(SDL_Window *w, const char *tag, int draw_w, int draw_h, const int dst[4]) {
    std::vector<unsigned char> px((size_t)draw_w * draw_h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, draw_w, draw_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    auto at = [&](int x, int y) {
      x = std::clamp(x, 0, draw_w - 1);
      y = std::clamp(y, 0, draw_h - 1);
      const unsigned char *p = &px[((size_t)y * draw_w + x) * 4];
      char b[32];
      std::snprintf(b, sizeof b, "%d,%d,%d", p[0], p[1], p[2]);
      return std::string(b);
    };
    // A bar pixel: the middle of the left bar, else of the bottom bar.
    const std::string bar = dst[0] > 0 ? at(dst[0] / 2, draw_h / 2)
                            : dst[1] > 0 ? at(draw_w / 2, dst[1] / 2) : std::string("none");
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    const auto path = std::filesystem::path(dir) / (std::string("win_") + tag + ".bmp");
    nwii::runtime::gx::SaveBMP32(path.string(), px.data(), draw_w, draw_h, 0);
    std::ostringstream os;
    os << std::dec << "[RES9-SELFTEST] " << tag << " window=" << ww << "x" << wh
       << " drawable=" << draw_w << "x" << draw_h << " image=" << dst[0] << "," << dst[1] << ","
       << dst[2] << "x" << dst[3] << " fullscreen=" << (IsFullscreen(w) ? 1 : 0)
       << " cfg_window=" << nwii::runtime::Config::get().window_width << "x"
       << nwii::runtime::Config::get().window_height << " bar_rgb=" << bar
       << " centre_rgb=" << at(draw_w / 2, draw_h / 2) << " -> " << path.string() << "\n";
    std::cout << os.str() << std::flush;
  }
  // Called after the present blit, before the swap.
  void after_present(SDL_Window *w, uint64_t frame, int draw_w, int draw_h, const int dst[4]) {
    if (step < 0 || frame < next || draw_w <= 0 || draw_h <= 0)
      return;
    switch (step++) {
    case 0:
      capture(w, "a_start", draw_w, draw_h, dst);
      SDL_SetWindowSize(w, 1100, 480); // wider than 16:9: pillarbox
      next = frame + 30;
      break;
    case 1:
      capture(w, "b_wide", draw_w, draw_h, dst);
      SDL_SetWindowSize(w, 640, 720); // taller: letterbox
      next = frame + 30;
      break;
    case 2:
      capture(w, "c_tall", draw_w, draw_h, dst);
      SDL_SetWindowSize(w, 1280, 720);
      push_key(SDLK_F11, KMOD_NONE);
      next = frame + 45;
      break;
    case 3:
      capture(w, "d_fullscreen_f11", draw_w, draw_h, dst);
      push_key(SDLK_RETURN, KMOD_LALT);
      next = frame + 45;
      break;
    case 4:
      capture(w, "e_windowed_altenter", draw_w, draw_h, dst);
      push_key(SDLK_F12, KMOD_NONE);
      next = frame + 15;
      break;
    default:
      std::cout << "[RES9-SELFTEST] done" << std::endl;
      step = -1;
      break;
    }
  }
};

} // namespace res9

// GXF12 counters (gx.cpp), declared at namespace scope for gxf12::MaybeReport.
extern uint64_t g_stat_frames, g_stat_xfb_copies, g_stat_frames_merged, g_stat_gxf12_cut_us,
    g_stat_gxf12_decode_us, g_stat_gxf12_render_us, g_stat_gxf12_free_us, g_stat_gxf12_lock_us,
    g_stat_gxf12_backlog_max, g_stat_gxf12_wakeups, g_stat_gxf12_lockwait_us,
    g_stat_gxf12_hostparse_us;
extern std::atomic<uint64_t> g_stat_gxf12_idleparse_us;
int GX_PendingFrames();
uint64_t GX_WorkSeq();
bool GX_WaitWork(uint64_t seen, int timeout_ms);
void GX_StopParseWorker(); // UIT14: before ctx (guest RAM) is freed at exit

// GXF12: present thread (core/gxf12_presenter.h; NWII_PRESENT_THREAD=0 = off).
namespace gxf12 {
// Present-thread blit rect: RES9 aspect fit, published for the mouse source.
void FitAndPublish(int dw, int dh, bool wide, int aspect, int dst[4]) {
  res9::FitRect(dw, dh, wide, aspect, dst);
  if (dst[2] > 0 && dst[3] > 0 && dw > 0 && dh > 0) {
    // Picture area for the mouse source (window-normalised, y from the top).
    nwii::runtime::input::g_present_rect[0].store(float(dst[0]) / dw);
    nwii::runtime::input::g_present_rect[1].store(float(dh - dst[1] - dst[3]) / dh);
    nwii::runtime::input::g_present_rect[2].store(float(dst[2]) / dw);
    nwii::runtime::input::g_present_rect[3].store(float(dst[3]) / dh);
  }
}

// NWII_GXF12_STATS=1 (or NWII_PROFILE): one [GXF12] line per 5 s with the
// frame pipeline counters (gx.cpp) and the present thread's.
void MaybeReport(const Presenter *p, uint64_t host_iters) {
  static const bool on = [] {
    const char *e = std::getenv("NWII_GXF12_STATS");
    const char *q = std::getenv("NWII_PROFILE");
    return (e && *e && *e != '0') || (q && *q && *q != '0');
  }();
  if (!on)
    return;
  static auto t0 = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const double dt = std::chrono::duration<double>(now - t0).count();
  if (dt < 5.0)
    return;
  t0 = now;
  struct Snap {
    uint64_t frames, xfb, merged, cut, dec, ren, fre, lck, wake, sub, pres, drop, swap, iters,
        lwait, hparse, iparse;
  };
  static Snap a{};
  Snap b{g_stat_frames, g_stat_xfb_copies, g_stat_frames_merged, g_stat_gxf12_cut_us,
         g_stat_gxf12_decode_us, g_stat_gxf12_render_us, g_stat_gxf12_free_us,
         g_stat_gxf12_lock_us, g_stat_gxf12_wakeups, p ? p->submitted() : 0,
         p ? p->presented() : 0, p ? p->dropped() : 0, p ? p->swap_us() : 0, host_iters,
         g_stat_gxf12_lockwait_us, g_stat_gxf12_hostparse_us,
         g_stat_gxf12_idleparse_us.load(std::memory_order_relaxed)};
  const double nf = b.frames > a.frames ? double(b.frames - a.frames) : 1.0;
  const double np = b.pres > a.pres ? double(b.pres - a.pres) : 1.0;
  char line[512];
  std::snprintf(line, sizeof line,
                "[GXF12] xfb/s=%.1f rendered/s=%.1f presented/s=%.1f merged=%llu pdropped=%llu "
                "backlog_max=%llu wakeups/s=%.0f host_iters/s=%.0f | ms/frame lock+parse=%.2f "
                "(wait=%.2f host_parse=%.2f idle_parse=%.2f) cut=%.2f decode=%.2f render=%.2f free=%.2f | "
                "swap_ms/present=%.1f sync_timeouts=%llu\n",
                (b.xfb - a.xfb) / dt, (b.frames - a.frames) / dt, (b.pres - a.pres) / dt,
                (unsigned long long)(b.merged - a.merged), (unsigned long long)(b.drop - a.drop),
                (unsigned long long)g_stat_gxf12_backlog_max, (b.wake - a.wake) / dt,
                (b.iters - a.iters) / dt, (b.lck - a.lck) / 1000.0 / nf,
                (b.lwait - a.lwait) / 1000.0 / nf, (b.hparse - a.hparse) / 1000.0 / nf,
                (b.iparse - a.iparse) / 1000.0 / nf,
                (b.cut - a.cut) / 1000.0 / nf, (b.dec - a.dec) / 1000.0 / nf,
                (b.ren - a.ren) / 1000.0 / nf, (b.fre - a.fre) / 1000.0 / nf,
                (b.swap - a.swap) / 1000.0 / np, (unsigned long long)(p ? p->timeouts() : 0));
  g_stat_gxf12_backlog_max = 0;
  a = b;
  std::cout << line << std::flush;
}

// INTERP17: the present thread's frame interpolation hook and statistics.
Presenter *g_interp_presenter = nullptr;
// gx.cpp (host thread): an in-between frame is in the present framebuffer; queue it before the real
// frame renders. The renderer's EFB binding is put back afterwards (Submit binds the ring slot).
void InterpPresentHook(uint64_t frame, int64_t t_cut_us, int64_t period_us) {
  Presenter *p = g_interp_presenter;
  if (!p || !p->on())
    return;
  GLint draw_fb = 0, read_fb = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
  int pw = 0, ph = 0;
  nwii::runtime::gx::PresentSize(pw, ph);
  const PresentTiming tm{1, t_cut_us, period_us};
  p->Submit(nwii::ui::ComposeFrame(nwii::runtime::gx::PresentFramebuffer(), pw, ph), pw, ph, frame,
            res9::GameWidescreen(frame), nwii::runtime::Config::get().aspect_mode, &tm);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
}
// NWII_INTERP_STATS=1: [INTERP17] (gx side) + [INTERP17-present] lines every 5 s.
void InterpMaybeReport(Presenter *p) {
  static const bool on = [] {
    const char *e = std::getenv("NWII_INTERP_STATS");
    return e && *e && *e != '0';
  }();
  if (!on)
    return;
  static auto t0 = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const double dt = std::chrono::duration<double>(now - t0).count();
  if (dt < 5.0)
    return;
  t0 = now;
  const std::string g = nwii::runtime::gx::InterpStatsLine();
  if (!g.empty())
    std::cout << g << "\n";
  if (!p)
    return;
  PaceStats ps = p->TakePaceStats();
  auto pct = [](std::vector<int> v, double q) -> double {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(q * (double)(v.size() - 1) + 0.5))] / 1000.0;
  };
  size_t in_band = 0, over_25 = 0;
  for (int gp : ps.gap_us) {
    in_band += gp >= 10000 && gp <= 23500;
    over_25 += gp > 25000;
  }
  char line[640];
  std::snprintf(line, sizeof line,
                "[INTERP17-present] presented/s=%.1f interp/s=%.1f real_paced/s=%.1f late=%llu D=%.1fms | gap ms "
                "p50=%.1f p95=%.1f max=%.1f in[10,23.5]=%.1f%% >25=%zu | latency cut->shown ms: I p50=%.1f p95=%.1f "
                "N p50=%.1f p95=%.1f plain p50=%.1f p95=%.1f",
                ps.presents / dt, ps.interp / dt, ps.real_paced / dt, (unsigned long long)ps.late, ps.d_us / 1000.0,
                pct(ps.gap_us, 0.5), pct(ps.gap_us, 0.95), pct(ps.gap_us, 1.0),
                ps.gap_us.empty() ? 0.0 : 100.0 * (double)in_band / (double)ps.gap_us.size(), over_25,
                pct(ps.lat_i_us, 0.5), pct(ps.lat_i_us, 0.95), pct(ps.lat_n_us, 0.5), pct(ps.lat_n_us, 0.95),
                pct(ps.lat_plain_us, 0.5), pct(ps.lat_plain_us, 0.95));
  std::cout << line << std::endl;
}
} // namespace gxf12

namespace nwii::runtime {
uint32_t g_debug_pc = 0;
MMU *g_mmu = nullptr;
CPUContext *g_ctx_ptr = nullptr;

bool init(const char *config_path = "config.toml") {
  Config::get().load(config_path);
  hw::register_all_hw(MMIODispatcher::get());
  return true;
}
void shutdown() {}
} 

extern "C" void run_game(nwii::runtime::CPUContext &ctx);
namespace nwii::runtime { void set_dispatcher_function(const void *fn); } // ios.cpp (PERF2)
namespace nwii::runtime { void idle_loop_scan(CPUContext &ctx); }          // ios.cpp (PERF2)

namespace {
std::atomic<bool> cpu_finished{false};
std::atomic<bool> cpu_failed{false};
}

void cpu_thread_func(nwii::runtime::CPUContext *ctx) {
  nwii::runtime::g_ctx_ptr = ctx;
  nwii::runtime::prof::register_current_thread(0); // PERF2 (NWII_PROFILE only)
  std::cout << "[Thread] CPU Core started. pc=" << std::hex << ctx->pc << " is_running=" << ctx->is_running << std::endl;
  std::flush(std::cout);
  run_game(*ctx);

  
  
  if (!ctx->is_running) {
    std::cout << "[Thread] CPU Core exited: shutdown requested (window closed "
                 "or quit). NOT a crash." << std::endl;
  } else {
    cpu_failed = true;
    std::cout << "[Thread] CPU Core CRASHED (jumped to 0). Final PC: 0x"
              << std::hex << ctx->pc << ", LR: 0x" << ctx->lr << std::dec
              << std::endl;
  }
  cpu_finished = true;
}

int main(int argc, char **argv) {
  SDL_SetMainReady();
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <path_to_unpacked_game_dir> [config_path]\n";
    return 1;
  }

  const char *config_path = (argc >= 3) ? argv[2] : "config.toml";
  if (!nwii::runtime::init(config_path))
    return 1;
  nwii::runtime::Config::get().game_dir = argv[1];
  // UI13 hook: the player's saved settings over the profile ([ui] settings_file / instellingen.toml),
  // [ui] env defaults, --no-launcher; NWII_UI_SELFTEST renders the UI off-screen and exits here.
  nwii::ui::OnConfigLoaded(config_path, argc, argv);
  if (const int ui_rc = nwii::ui::MaybeRunSelfTest(argv[1]); ui_rc >= 0)
    return ui_rc;

  {
    using namespace nwii::runtime::input;
    auto& im = InputManager::get();
    int mode = nwii::runtime::Config::get().input_mode;
    switch (mode) {
    case 1: 
      std::cout << "[Input] Mode 1 (Bluetooth Wiimote) not implemented, "
                   "falling back to keyboard+mouse" << std::endl;
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 2: 
    case 3: 
    case 4: 
      im.add_source(std::make_unique<GamepadSource>());
      
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 5: 
      im.add_source(std::make_unique<PhoneSource>());
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 7: 
      std::cout << "[Input] Mode 7 (touch): using pointer-as-touch mapping"
                << std::endl;
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 6:
    default:
      im.add_source(std::make_unique<MouseKeyboardSource>());
      im.add_source(std::make_unique<GamepadSource>());
      break;
    }
  }

  const char *headless_env = std::getenv("NWII_HEADLESS");
  bool headless = headless_env && headless_env[0] == '1';

  SDL_Window* window = nullptr;
  SDL_GLContext gl_ctx = nullptr;
  SDL_AudioDeviceID audio_dev = 0;
  GLuint efb_fbo = 0, efb_tex = 0;
  GLuint xfb_tex = 0;
  int efb_w = nwii::runtime::gx::kEfbWidth, efb_h = nwii::runtime::gx::kEfbHeight; // RES9
  res9::GpuTimer gpu_timer; // RES9: NWII_GPU_MS diagnostic
  res9::SelfTest res9_selftest; // RES9: NWII_RES9_SELFTEST diagnostic

  if (!headless) {
    // Controllers (user play logs 2026-10-03): SDL's DirectInput device detection hung the main
    // thread for good inside dinput8 -> HidD_GetProductString on a Bluetooth DualSense (with Steam,
    // Parsec and Virtual Desktop virtual HID devices present). That froze the window, GX and ALL
    // input. DirectInput is off by default: PlayStation/Switch pads go through SDL's HIDAPI driver
    // (which also exposes the DualSense gyro/accelerometer), Xbox pads through XInput.
    // NWII_DIRECTINPUT=1 re-enables it for exotic DirectInput-only controllers.
    {
      const char *dinput = std::getenv("NWII_DIRECTINPUT");
      SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, (dinput && dinput[0] == '1') ? "1" : "0");
      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
      // Enhanced Bluetooth reports carry the motion sensors (DualSense/DualShock 4 over BT).
      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
      // INPUT13: SDL_PollEvent on this thread never updates joysticks; the controller thread below
      // owns SDL's joystick subsystem (init, SDL_JoystickUpdate, every controller read), so a HID
      // driver that hangs can only stall the controllers, never the window, GX, keyboard or mouse.
      SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
      std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
      return 1;
    }
    {
      // INPUT13: controller thread (input/sources/gamepad_source.h). Not for replays (they disable the
      // live sources; a probe must not open the player's controller) nor for keyboard-only modes.
      const int im = nwii::runtime::Config::get().input_mode;
      if (im != 1 && im != 5 && im != 7 && nwii::runtime::input::InputManager::get().live_sources_enabled())
        nwii::runtime::input::start_controller_thread(nwii::runtime::input::sdl_controller_api());
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    // Generated shaders use GLSL 330 and explicit attribute locations.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    {
      SDL_DisplayMode dm;
      if (SDL_GetCurrentDisplayMode(0, &dm) == 0)
        std::cout << "[GL] display 0: " << dm.w << "x" << dm.h << " fmt="
                  << SDL_GetPixelFormatName(dm.format) << std::endl;
      else
        std::cerr << "[GL] no display mode: " << SDL_GetError()
                  << " (screen locked or no display attached?)" << std::endl;
    }
#ifdef __APPLE__

    
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    bool use_gl = (nwii::runtime::Config::get().backend == nwii::runtime::Backend::OpenGL);
    uint32_t window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    {
      // RES9: start state of borderless fullscreen (F11 / Alt+Enter toggle).
      bool fs = nwii::runtime::Config::get().fullscreen && !std::getenv("NWII_RUN_SECONDS");
      if (const char *e = std::getenv("NWII_FULLSCREEN"); e && *e) fs = *e != '0';
      if (fs) window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    if (!g_host_pacing.swap_from_env) // RES9: [graphics] vsync (default true = 1)
      g_host_pacing.swap_interval = nwii::runtime::Config::get().enable_vsync ? 1 : 0;
    if (use_gl) {
        window_flags |= SDL_WINDOW_OPENGL;
    } else {
#ifdef SDL_WINDOW_METAL
        window_flags |= SDL_WINDOW_METAL;
#endif
    }
    window = SDL_CreateWindow("NWiiRecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              nwii::runtime::Config::get().window_width,
                              nwii::runtime::Config::get().window_height,
                              window_flags);
    if (!window) {
      std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
      return 1;
    }
    SDL_SetWindowMinimumSize(window, 320, 240);
    {
      // Config window size = actual window size (fullscreen, DPI clamps), for
      // the mouse pointer mapping (mouse_keyboard_source.cpp).
      int ww = 0, wh = 0;
      SDL_GetWindowSize(window, &ww, &wh);
      if (ww > 0 && wh > 0) {
        nwii::runtime::Config::get().window_width = ww;
        nwii::runtime::Config::get().window_height = wh;
      }
    }

    
    extern void GX_SetWindow(void *);
    GX_SetWindow(window);
    if (use_gl) {
      gl_ctx = SDL_GL_CreateContext(window);
      if (!gl_ctx) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << "\n";
        return 1;
      }
      if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        std::cerr << "glad: failed to load GL functions\n";
        return 1;
      }
      {
        // A lost/disabled GPU (driver reset, device error) yields a context that cannot
        // answer glGetString: report it clearly instead of crashing on a null string.
        const char *renderer = (const char *)glGetString(GL_RENDERER);
        const char *version = (const char *)glGetString(GL_VERSION);
        const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
        if (!renderer || !version || !glsl) {
          const char *msg =
              "OpenGL is niet beschikbaar (de grafische driver geeft geen antwoord).\n"
              "Herstart de pc of controleer de grafische kaart in Apparaatbeheer.\n\n"
              "OpenGL is not available (the graphics driver does not respond).";
          std::cerr << "[GL] fatal: glGetString returned null (renderer=" << (renderer ? renderer : "null")
                    << ") - GPU driver unavailable\n";
          // Interactive sessions only (same rule as freeze_watchdog.cpp): never block a probe run.
          if (!std::getenv("NWII_RUN_SECONDS") && !std::getenv("NWII_HEADLESS") && !std::getenv("NWII_SS_INPUT_REPLAY"))
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Skyward Sword PC", msg, window);
          return 1;
        }
        std::cout << "[GL] " << renderer << " | " << version << " | GLSL " << glsl << std::endl;
      }
      {
        int si = g_host_pacing.swap_interval;
        if (SDL_GL_SetSwapInterval(si) != 0 && si == -1) {
          si = 0; // adaptive vsync unsupported
          SDL_GL_SetSwapInterval(si);
        }
        std::cout << "[PERF2] swap interval " << si << ", host VI "
                  << (g_host_pacing.host_vi ? "on" : "off") << std::endl;
      }
    }
    // UI13 hook: launcher (title screen + settings) before the guest boots, then the in-game menu.
    // Skipped for probe/replay/headless runs, [ui] launcher = false and --no-launcher.
    if (use_gl && !nwii::ui::Start(window, gl_ctx, argv[1])) {
      if (nwii::runtime::input::stop_controller_thread()) // UI13: AFSLUITEN, same teardown as below
        SDL_Quit();
      return 0;
    }
    // HDTEX16: custom (HD) texture pack index + texture dump, after the launcher (its settings apply).
    nwii::runtime::hle::HdTexInit();
    
    // RENDER1 (M1.3): the EFB is 640x528 like the hardware's; the renderer
    // copies the XFB source rect into its own 640x480 present framebuffer,
    // which is what the window and the screenshots show. Depth is a texture
    // (not a renderbuffer) so EFB depth copies can sample it.
    // RES9 (M7.3): both are scaled by the internal resolution s; a scale whose
    // FBO is incomplete falls back to the next smaller one.
    {
      GLuint efb_depth_tex = 0;
      for (int s = res9::ChooseInternalScale(); s >= 1; --s) {
        if (res9::CreateEfb(s, efb_fbo, efb_tex, efb_depth_tex)) {
          efb_w = nwii::runtime::gx::kEfbWidth * s;
          efb_h = nwii::runtime::gx::kEfbHeight * s;
          break;
        }
        std::cout << "[RES9] internal scale " << s << " unavailable, trying " << s - 1
                  << std::endl;
      }
    }
    gpu_timer.init();
    res9_selftest.init();

    glGenTextures(1, &xfb_tex);
    glBindTexture(GL_TEXTURE_2D, xfb_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // AUDIO track (M5.3): host audio is now an SDL callback device owned by
    // src/platform/audio_out.cpp, fed by the AI DMA engine in hw_dsp.cpp. The
    // legacy 32 kHz queue device stays closed (audio_dev = 0 skips its pump).
  }

  auto ctx = std::make_unique<nwii::runtime::CPUContext>();
  nwii::runtime::g_mmu = &ctx->mmu;

  nwii::loader::Executable exe;
  if (!exe.load_unpacked_game(argv[1]))
    return 1;

  uint32_t arena_lo = 0x80000000;
  
  for (const auto &sec : exe.sections) {
    uint32_t end_addr = sec.address + sec.size;
    if (end_addr > arena_lo)
      arena_lo = end_addr;

    if (sec.is_bss) {
      for (size_t i = 0; i < sec.size; ++i)
        ctx->mmu.write8(sec.address + i, 0);
    }
  }

  for (const auto &sec : exe.sections) {
    if (!sec.is_bss) {
      std::cout << "[Loader] Loading section at 0x" << std::hex << sec.address << " size 0x" << sec.size << std::dec << std::endl;
      for (size_t i = 0; i < sec.size; ++i) {
        if ((sec.address + i) == 0x80003448) {
            std::cout << "[Loader] Found 0x80003448 inside section! val=" << std::hex << (uint32_t)sec.data[i] << std::dec << std::endl;
        }
        ctx->mmu.write8(sec.address + i, sec.data[i]);
      }
    }

    

    
    
    if (sec.is_text) {
      uint32_t start = sec.address;
      if (std::getenv("NWII_INTERP_LOW"))
        start = std::max<uint32_t>(sec.address, 0x80004000);
      uint32_t end = sec.address + sec.size;
      if (end > start)
        nwii::runtime::add_recompiled_range(start, end);
    }
  }

  arena_lo = (arena_lo + 31) & ~31;

  

  {
    uint32_t stack_top = 0;
    uint32_t hi = 0;
    bool have_hi = false;
    // SOUE01's CRT initializes r1 at entry+0x234. The former 0x200-byte
    // window missed it and let OS ClearArena erase its own active stack.
    for (uint32_t off = 0; off < 0x400; off += 4) {
      if (!ctx->mmu.get_ptr(exe.entry_point + off, 4)) break;
      uint32_t insn = ctx->mmu.read32(exe.entry_point + off);
      if ((insn & 0xFFFF0000) == 0x3C200000) { 
        hi = (insn & 0xFFFF) << 16;
        have_hi = true;
      } else if (have_hi && (insn & 0xFFFF0000) == 0x60210000) { 
        stack_top = hi | (insn & 0xFFFF);
        break;
      } else if (have_hi && (insn & 0xFFFF0000) == 0x38210000) { 
        stack_top = hi + (int16_t)(insn & 0xFFFF);
        break;
      }
    }
    if (stack_top >= arena_lo && stack_top < 0x81800000) {
      std::cout << "[Loader] crt stack top 0x" << std::hex << stack_top
                << " above section end 0x" << arena_lo
                << ", raising ArenaLo" << std::dec << std::endl;

      arena_lo = (stack_top + 0x100 + 31) & ~31u;
    }
  }

  bool is_gc = nwii::runtime::Config::get().platform ==
               nwii::runtime::Platform::GameCube;

  
  uint32_t arena_hi = 0x81700000;
  auto &vdisc = nwii::runtime::VirtualDisc::get();
  if (vdisc.init(argv[1])) {
    const auto &boot = vdisc.boot_data();
    
    for (size_t i = 0; i < 0x20 && i < boot.size(); ++i)
      ctx->mmu.write8(0x80000000 + (uint32_t)i, boot[i]);

    const auto &fst = vdisc.fst_data();
    uint32_t fst_size = (uint32_t)fst.size();
    uint32_t fst_addr = (0x81800000 - fst_size) & ~63u;
    for (uint32_t i = 0; i < fst_size; ++i)
      ctx->mmu.write8(fst_addr + i, fst[i]);

    ctx->mmu.write32(0x80000038, fst_addr); 
    ctx->mmu.write32(0x8000003C, fst_size); 
    // The apploader gives the game MEM1 up to the FST. A fixed 0x81700000
    // ceiling withholds almost 1 MiB and breaks Skyward Sword's fixed heaps.
    arena_hi = fst_addr & ~31u;
    std::cout << "[Loader] FST loaded at 0x" << std::hex << fst_addr
              << " size 0x" << fst_size << std::dec << std::endl;
  }

  // Console type (Dolphin BootManager): Wii retail Hollywood = 0x10000006,
  
  uint32_t console_type = is_gc ? 0x00000003 : 0x10000006;
  uint32_t mem1_size = 24 * 1024 * 1024; 
  uint32_t mem2_size = 64 * 1024 * 1024; 

  ctx->mmu.write32(0x80000020, is_gc ? 0x0D15EA5E : console_type); 
  
  ctx->mmu.write32(0x80000024, mem1_size);
  
  ctx->mmu.write32(0x80000028, mem1_size);
  ctx->mmu.write32(0x8000002C, console_type);
  // Arena bounds, apploader-style (matches Dolphin BS2 emulation):

  

  if (!is_gc) {
    ctx->mmu.write32(0x80000030, arena_lo);
    ctx->mmu.write32(0x80000034, arena_hi);
  }
  if (!vdisc.valid()) {
    
    ctx->mmu.write32(0x80000038, arena_hi);
  }
  if (is_gc) {

    

    uint32_t gc_arena_hi = arena_hi;
    uint32_t fst_base = ctx->mmu.read32(0x80000038);
    if (fst_base >= 0x80000000 && fst_base < 0x81800000)
      gc_arena_hi = fst_base & ~31u;
    std::filesystem::path bi2_path =
        std::filesystem::path(argv[1]) / "sys" / "bi2.bin";
    if (std::filesystem::exists(bi2_path)) {
      std::ifstream bi2(bi2_path, std::ios::binary);
      std::vector<char> bi2_data((std::istreambuf_iterator<char>(bi2)),
                                 std::istreambuf_iterator<char>());
      uint32_t bi2_addr = 0x81200000; 
      for (size_t i = 0; i < bi2_data.size(); ++i)
        ctx->mmu.write8(bi2_addr + (uint32_t)i, (uint8_t)bi2_data[i]);
      ctx->mmu.write32(0x800000F4, bi2_addr);
      std::cout << "[Loader] BI2 loaded at 0x" << std::hex << bi2_addr
                << std::dec << std::endl;
    }

    
    
    ctx->mmu.write32(0x80000030, arena_lo);
    ctx->mmu.write32(0x80000034, gc_arena_hi);
    std::cout << "[Loader] GC Arena = 0x" << std::hex << arena_lo << " - 0x"
              << gc_arena_hi << " (" << std::dec
              << (gc_arena_hi - arena_lo) / 1024 << " KiB)" << std::endl;
  }
  
  ctx->mmu.write32(0x800000F0, mem1_size);

  

  
  
  auto poke_global = [&](uint32_t addr, uint32_t val) {
    for (const auto &sec : exe.sections)
      if (!sec.is_bss && addr >= sec.address && addr < sec.address + sec.size)
        return; 
    ctx->mmu.write32(addr, val);
  };

  if (!is_gc) {

    poke_global(0x80003118, mem2_size);
    poke_global(0x8000311C, mem2_size);
    
    poke_global(0x80003124, 0x90000800);
    poke_global(0x80003128, 0x93e00000);
    poke_global(0x80003130, 0x93e00000);
    poke_global(0x80003134, 0x94000000);
    
    poke_global(0x80003158, 0x00000023);
  }

  

  

  {
    const std::string &gid = nwii::runtime::Config::get().game_id;
    char region = gid.size() >= 4 ? gid[3] : 'E';
    
    bool pal = (region == 'P' || region == 'D' || region == 'F' ||
                region == 'I' || region == 'S' || region == 'H' ||
                region == 'U' || region == 'X' || region == 'Y' ||
                region == 'Z');
    poke_global(0x800000CC, pal ? 1u : 0u);
  }

  ctx->gpr[1] = 0x816FFFF0;

  // Low memory 0xF8/0xFC hold the BUS and CPU clocks (Dolphin writes

  
  
  if (is_gc) {
    ctx->mmu.write32(0x800000F8, 162000000); 
    ctx->mmu.write32(0x800000FC, 486000000); 
    ctx->tb_freq = 40500000;                 
  } else {
    ctx->mmu.write32(0x800000F8, 243000000); 
    ctx->mmu.write32(0x800000FC, 729000000); 
    ctx->tb_freq = 60750000;                 
  }

  ctx->tb_start = std::chrono::steady_clock::now();

  // CLOCK15: the time base starts at the host's local date and time (Dolphin
  // SystemTimers::Init: FakeTBStartValue = timer clock * seconds since
  // 2000-01-01); NWII_RTC pins it (guest_clock.h). The poll scheduler starts
  // there too, so every pending event keeps its distance to the clock.
  {
    namespace gclk = nwii::runtime::guest_clock;
    const gclk::Choice rtc = gclk::choose(std::getenv("NWII_RTC"), gclk::host_local_unix_seconds());
    ctx->start_clock(gclk::boot_timebase(ctx->tb_freq, rtc.local_unix)); // tb_base + DEC reference
    nwii::runtime::EventScheduler::get().start_at(ctx->read_timebase());
    std::cout << "[Clock] guest time at boot " << gclk::format(rtc.local_unix) << " (" << rtc.source
              << ") tb_base=" << ctx->tb_base << std::endl;
  }

  nwii::runtime::init_ipc_client(*ctx);

  // PERF2 sampling profiler (NWII_PROFILE=<hz>, off by default). It only reads
  // the published guest PC/LR and counters; see runtime/profiler.h.
  {
    extern uint64_t g_stat_frames;
    nwii::runtime::prof::GuestProbe probe;
    probe.pc = &ctx->pc;
    probe.lr = &ctx->lr;
    probe.inst = &ctx->inst_count;
    probe.frames = &g_stat_frames;
    extern uint64_t g_stat_xfb_copies;
    probe.xfb_copies = &g_stat_xfb_copies;
    probe.polls = &nwii::runtime::poll_call_count;
    probe.is_native = &nwii::runtime::in_recompiled_code;
    probe.guest_ptr = [](uint32_t addr, uint32_t size) -> const uint8_t * {
      return nwii::runtime::g_mmu ? nwii::runtime::g_mmu->get_ptr(addr, size) : nullptr;
    };
    if (nwii::runtime::prof::init(probe))
      nwii::runtime::prof::register_current_thread(1);
  }

  // PERF2: lets the poll recognise calls from the generated dispatcher and skip
  // its linear bounds scan for interpreted targets (ios.cpp; NWII_DISPATCH_SHORTCUT=0 off).
  nwii::runtime::set_dispatcher_function(reinterpret_cast<const void *>(&run_game));
  nwii::runtime::idle_loop_scan(*ctx); // PERF2: Dolphin-pattern idle loops (NWII_IDLE=0 off)

  std::thread cpu_thread(cpu_thread_func, ctx.get());

  const auto run_started = std::chrono::steady_clock::now();
  const char* run_limit_env = std::getenv("NWII_RUN_SECONDS");
  const unsigned long run_seconds = run_limit_env ? std::strtoul(run_limit_env, nullptr, 10) : 0;
  // Host-loop diagnostic switches, read once instead of with std::getenv on
  // every host tick/frame (M0.3).
  const bool host_sample = std::getenv("NWII_SAMPLE") != nullptr;
  const char *const host_peek = std::getenv("NWII_PEEK");
  const bool host_legacy_diag = std::getenv("NWII_LEGACY_DIAGNOSTICS") != nullptr;
  const bool host_xfb = std::getenv("NWII_XFB") != nullptr;
  auto within_run_limit = [&]() {
    if (run_seconds == 0 || std::chrono::steady_clock::now() - run_started <
                                std::chrono::seconds(run_seconds)) return true;
    std::cout << "[Test] Requested run duration reached; stopping." << std::endl;
    return false;
  };

  if (headless) {
    
    uint64_t tick = 0;
    uint64_t gxf12_iters = 0;
    uint64_t gx_work_seq = GX_WorkSeq();
    while (ctx->is_running && !cpu_finished && within_run_limit()) {
      prof::host_phase(prof::kHostSleep);
      // GXF12: wait at most 16 ms as before, but wake when the CPU thread's
      // idle parse completes a frame, and not at all while completed frames
      // wait, so each guest frame is cut on its own (headless GX fps = game
      // fps; the 16 ms sleep took ~31 ms here and merged 1-2 frames per 5 s).
      if (g_host_pacing.host_vi)
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      else if (GX_PendingFrames() == 0)
        GX_WaitWork(gx_work_seq, 16);
      gxf12::MaybeReport(nullptr, ++gxf12_iters);

      prof::host_phase(prof::kHostInput);
      nwii::runtime::input::InputManager::get().update();

      extern void ProcessGXFifo();
      {
        prof::host_phase(prof::kHostGx);
        const auto g0 = prof::g_on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        gx_work_seq = GX_WorkSeq(); // GXF12: read before the cut
        ProcessGXFifo();
        if (prof::g_on)
          prof::bump(prof::kGxUs, (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - g0).count());
        prof::host_phase(prof::kHostOther);
        prof::bump(prof::kHostIters);
      }
      if (g_host_pacing.host_vi) ctx->vblank_pending = true;
      if (host_sample && (++tick % 60) == 0) {
        std::cout << "[Sample] pc=0x" << std::hex << ctx->pc << " lr=0x"
                  << ctx->lr << " msr=0x" << ctx->msr << " r3=0x" << ctx->gpr[3]
                  << " cb=" << (ctx->in_callback ? 1 : 0) << " inst=" << std::dec
                  << ctx->inst_count << "\n";
        
        if (const char *env = host_peek) {
          uint32_t pa = 0, pw = 8;
          if (std::sscanf(env, "%x,%u", &pa, &pw) >= 1 && pa) {
            std::cout << "[Peek] " << std::hex << pa << ":";
            for (uint32_t i = 0; i < pw && i < 32; i++)
              std::cout << " " << ctx->mmu.read32(pa + i * 4);
            std::cout << std::dec << "\n";
          }
        }

        
        if (host_legacy_diag &&
            (tick % 300) == 0 && ctx->gpr[13] >= 0x80000000u) {
          uint32_t r13 = ctx->gpr[13];
          std::cout << "  [RunQ] bitmap=0x" << std::hex
                    << ctx->mmu.read32(r13 - 0x21c8) << " resched=0x"
                    << ctx->mmu.read32(r13 - 0x21c4) << " cur=0x"
                    << ctx->mmu.read32(0x800000E4)

                    << " retrace=" << std::dec << ctx->mmu.read32(r13 - 0x2358)
                    << std::hex << " postCB=0x" << ctx->mmu.read32(r13 - 0x2344)
                    << std::dec << "\n";

          
          uint32_t vs = ctx->mmu.read32(r13 - 0x29c0);

          
          uint32_t ef = ctx->mmu.read32(r13 - 0x2AF0);
          std::cout << "  [FlipG] vs=0x" << std::hex << vs
                    << " vsA2=" << std::dec << (int)ctx->mmu.read8(vs + 0xa2)
                    << " pend29b6=" << (int)ctx->mmu.read8(r13 - 0x29b6)
                    << " b29b5=" << (int)ctx->mmu.read8(r13 - 0x29b5)
                    << " b4808=" << (int)ctx->mmu.read8(r13 - 0x4808)
                    << " ef=0x" << std::hex << ef
                    << " efB0=0x" << (ef ? ctx->mmu.read32(ef + 0xb0) : 0)
                    << " efB4=0x" << (ef ? ctx->mmu.read32(ef + 0xb4) : 0)
                    << " efB8=" << std::dec << (ef ? (int)ctx->mmu.read8(ef + 0xb8) : -1)
                    << " efBE=" << (ef ? (int)ctx->mmu.read8(ef + 0xbe) : -1)
                    << " ef9C=" << (ef ? (int)ctx->mmu.read32(ef + 0x9c) : -1)
                    << " ddflag=" << (int)ctx->mmu.read8(r13 - 0x2368)
                    << std::hex << " tok=0x" << ctx->mmu.read16(r13 - 0x29b8)
                    << " pe_sr=0x" << nwii::runtime::hw::g_pe_sr
                    << std::dec << "\n";

          
          std::cout << "  [VIwrap] hwp=0x" << std::hex
                    << ctx->mmu.read32(r13 - 0x4118) << " shp=0x"
                    << ctx->mmu.read32(r13 - 0x239c) << " sdkPre=0x"
                    << ctx->mmu.read32(r13 - 0x2348) << " wrapPre=0x"
                    << ctx->mmu.read32(r13 - 0x237c) << std::dec << "\n";
        }

        
        
        if ((tick % 300) == 0) {
          uint32_t cur = ctx->mmu.read32(0x800000E4);
          uint32_t th = ctx->mmu.read32(0x800000DC);
          int guard = 0;
          while (th >= 0x80000000u && th < 0x81800000u && guard++ < 16) {

            uint16_t state = ctx->mmu.read16(th + 0x2C8);
            uint32_t prio = ctx->mmu.read32(th + 0x2D0);
            uint32_t wq = ctx->mmu.read32(th + 0x2DC);
            uint32_t srr0 = ctx->mmu.read32(th + 0x198);
            std::cout << "  [Thr] 0x" << std::hex << th << (th == cur ? "*" : " ")
                      << " st=" << std::dec << state << " pr=" << prio
                      << " wq=0x" << std::hex << wq << " srr0=0x" << srr0;
            if (wq >= 0x80000000u && wq < 0x81800000u) {
              std::cout << " wq[-2..5]:";
              for (int i = -2; i < 6; ++i)
                std::cout << " " << ctx->mmu.read32(wq + i * 4);
            }
            std::cout << std::dec << "\n";
            th = ctx->mmu.read32(th + 0x2FC);
          }
        }
      }
    }
  } else {
    
    std::vector<unsigned char> xfb_px;
    unsigned xfb_w = 0, xfb_h = 0;
    bool quit = false;
    bool shot_full_requested = false; // RES9: F12

    // GXF12: present thread (see gxf12::Presenter); the inline present below
    // stays for the cases it does not cover and as the A/B control.
    gxf12::Presenter presenter;
    SDL_Window *host_drawable = nullptr; // hidden window the host context is current on
    {
      // Default on; NWII_PRESENT_THREAD=0 = the old inline present (A/B).
      const char *e = std::getenv("NWII_PRESENT_THREAD");
      const bool want = !(e && *e == '0');
      if (want && gl_ctx && res9_selftest.dir.empty() && !host_xfb && !g_host_pacing.host_vi) {
        // The host context moves to a hidden window of the same pixel format,
        // so only the present thread uses the visible window's drawable.
        host_drawable = SDL_CreateWindow("NWiiRecomp GX", 0, 0, 16, 16,
                                         SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        const bool started =
            host_drawable && SDL_GL_MakeCurrent(host_drawable, gl_ctx) == 0 &&
            presenter.Start(window, host_drawable, gl_ctx, g_host_pacing.swap_interval,
                            &gxf12::FitAndPublish);
        if (!started) {
          SDL_GL_MakeCurrent(window, gl_ctx);
          if (host_drawable)
            SDL_DestroyWindow(host_drawable);
          host_drawable = nullptr;
          std::cout << "[GXF12] present thread unavailable (inline present)" << std::endl;
        }
      } else
        std::cout << "[GXF12] present thread off (inline present)" << std::endl;
      // INTERP17: in-between frames need the present thread (pacing) and a display composing at >= 50 Hz.
      {
        const double hz = presenter.on() ? presenter.composition_hz() : 0.0;
        const bool ok = presenter.on() && (hz <= 0.0 || hz >= 50.0);
        nwii::runtime::gx::InterpSetDisplayOk(ok);
        if (presenter.on()) {
          gxf12::g_interp_presenter = &presenter;
          nwii::runtime::gx::g_interpPresentHook = &gxf12::InterpPresentHook;
        }
        if (nwii::runtime::FrameInterpOn() || std::getenv("NWII_INTERP_STATS"))
          std::printf("[INTERP17] display composes at %.1f Hz (%s)%s\n", hz, presenter.composition_source(),
                      !presenter.on() ? ": no present thread, frame interpolation unavailable"
                      : ok ? "" : ": below 50 Hz, 60 fps impossible -> frame interpolation off");
      }
    }
    uint64_t gxf12_iters = 0;

    while (!quit && ctx->is_running && !cpu_finished && within_run_limit()) {
      gxf12::MaybeReport(presenter.on() ? &presenter : nullptr, ++gxf12_iters);
      gxf12::InterpMaybeReport(presenter.on() ? &presenter : nullptr); // INTERP17
      prof::host_phase(prof::kHostInput);
      if (!headless) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
          if (nwii::ui::HandleEvent(e)) // UI13 hook: the open in-game menu takes this event
            continue;
          if (e.type == SDL_QUIT) {
            quit = true;
          } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                     e.window.windowID == SDL_GetWindowID(window)) {
            // GXF12: with the hidden GX window open, closing the game window is
            // no longer "the last window closed", so SDL sends no SDL_QUIT.
            quit = true;
          } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            // RES9: F11 / Alt+Enter = borderless fullscreen, F12 = screenshot.
            const SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_F11 ||
                ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && (e.key.keysym.mod & KMOD_ALT)))
              res9::ToggleFullscreen(window);
            else if (k == SDLK_F12)
              shot_full_requested = true;
          } else if (e.type == SDL_WINDOWEVENT &&
                     e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED &&
                     e.window.windowID == SDL_GetWindowID(window) && // not the hidden GX window
                     e.window.data1 > 0 && e.window.data2 > 0) {
            // RES9: keep the configured size = the window size (mouse mapping).
            nwii::runtime::Config::get().window_width = e.window.data1;
            nwii::runtime::Config::get().window_height = e.window.data2;
          }
        }
      }

      nwii::runtime::input::InputManager::get().update();

      if (!headless) {
        glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo);
        // The renderer sets the XF viewport/scissor per draw batch itself.
        glViewport(0, 0, efb_w, efb_h);
        gpu_timer.begin(); // RES9 NWII_GPU_MS (no-op when off)
      }

      extern void GX_GetXfb(uint32_t*, unsigned*, unsigned*, unsigned*);
      uint32_t xfb_addr; unsigned xw, xh, xstride;
      GX_GetXfb(&xfb_addr, &xw, &xh, &xstride);
      if (nwii::runtime::hw::g_vi_top_field_base != 0) {
        xfb_addr = nwii::runtime::hw::g_vi_top_field_base;
      }

      if (xfb_addr && xw && xh && xw <= 720 && xh <= 576 && host_xfb && !headless) {
        xfb_px.resize((size_t)xw * xh * 4);
        auto clamp8 = [](float v) -> unsigned char { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); };
        for (unsigned y = 0; y < xh; ++y) {
          uint32_t row = (xfb_addr | 0x80000000u) + (uint32_t)y * xstride;
          for (unsigned x = 0; x < xw; x += 2) {
            uint8_t y0 = ctx->mmu.read8(row + x * 2 + 0);
            uint8_t cb = ctx->mmu.read8(row + x * 2 + 1);
            uint8_t y1 = ctx->mmu.read8(row + x * 2 + 2);
            uint8_t cr = ctx->mmu.read8(row + x * 2 + 3);
            float fcb = (float)cb - 128.0f, fcr = (float)cr - 128.0f;
            for (int k = 0; k < 2; ++k) {
              float yy = (float)(k ? y1 : y0);
              size_t p = ((size_t)y * xw + x + k) * 4;
              if (x + (unsigned)k >= xw) break;
              xfb_px[p + 0] = clamp8(yy + 1.371f * fcr);
              xfb_px[p + 1] = clamp8(yy - 0.336f * fcb - 0.698f * fcr);
              xfb_px[p + 2] = clamp8(yy + 1.732f * fcb);
              xfb_px[p + 3] = 255;
            }
          }
        }
        glBindTexture(GL_TEXTURE_2D, xfb_tex);
        if (xw != xfb_w || xh != xfb_h) {
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, xw, xh, 0, GL_RGBA, GL_UNSIGNED_BYTE, xfb_px.data());
          xfb_w = xw; xfb_h = xh;
        } else {
          glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, xw, xh, GL_RGBA, GL_UNSIGNED_BYTE, xfb_px.data());
        }
      }

      extern void ProcessGXFifo();
      prof::host_phase(prof::kHostGx);
      const auto prof_gx0 = prof::g_on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      extern uint64_t g_stat_frames;
      const uint64_t frames_before_gx = g_stat_frames;
      const uint64_t gx_work_seq = GX_WorkSeq(); // GXF12: read before the cut
      ProcessGXFifo();
      const bool new_frame = g_stat_frames != frames_before_gx;
      // Present only new frames, unless the old swap-interval-1 pacing is asked
      // for (inline present only; the present thread shows each new frame once).
      const bool present_now =
          (presenter.on() ? new_frame : (g_host_pacing.swap_interval == 1 || new_frame)) ||
          nwii::ui::WantsPresent(); // UI13 hook: menu/toast redraw without a new game frame
      const auto prof_gx1 = prof::g_on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      if (prof::g_on)
        prof::bump(prof::kGxUs, (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                    prof_gx1 - prof_gx0).count());
      prof::host_phase(prof::kHostPresent);

      

      if (!headless) {
        static auto t0 = std::chrono::steady_clock::now();
        static int stat_n = 0;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >=
            5) {
          t0 = now;
          extern uint64_t g_stat_frames, g_stat_draws;
          extern uint64_t g_stat_parse_us, g_stat_render_us;
          extern uint64_t nwii_stat_hash0(), nwii_stat_shaders();
          // The shown image (last XFB copy, 640x480), not the raw EFB: the
          // XFB copy clears the EFB for the next frame.
          std::vector<unsigned char> p;
          nwii::runtime::gx::ReadPresented(p);
          size_t nonblack = 0;
          for (size_t i = 0; i < p.size(); i += 4)
            if (p[i] | p[i + 1] | p[i + 2])
              ++nonblack;
          // Built in a local stream: std::cout's format flags are shared with
          // other threads, which used to leak std::hex into the label ("82s").
          std::ostringstream st;
          st << std::dec << "[STAT] " << ++stat_n * 5 << "s frames="
                    << g_stat_frames << " draws=" << g_stat_draws
                    << " parse_ms=" << g_stat_parse_us / 1000
                    << " render_ms=" << g_stat_render_us / 1000
                    << " efb_nonblack=" << nonblack << "/307200"
                    << " shaders=" << nwii_stat_shaders() << " hash0=0x"
                    << std::hex << nwii_stat_hash0();
          g_stat_parse_us = 0;
          g_stat_render_us = 0;

          st << " E0..E7=";
          for (int i = 0xE0; i <= 0xE7; ++i)
            st << nwii::runtime::gx::g_state.bp[i] << ",";

          
          st << std::dec << " projType="
                    << nwii::runtime::gx::g_state.projType
                    << " projSet=" << (int)nwii::runtime::gx::g_state.projSet
                    << " proj=";
          for (int i = 0; i < 6; ++i)
            st << nwii::runtime::gx::g_state.projection[i] << ",";

          
          uint32_t base = nwii::runtime::gx::g_state.texStages[0].base_addr;
          uint32_t toff = nwii::runtime::gx::g_state.texStages[0].tlut_offset;
          uint32_t tex_nz = 0, tlut_nz = 0;
          if (nwii::runtime::g_ctx_ptr) {
            for (uint32_t i = 0; i < 2048; ++i)
              if (nwii::runtime::g_ctx_ptr->mmu.read8(base + i))
                ++tex_nz;
          }
          for (uint32_t i = 0; i < 512; ++i) {
            uint32_t idx = toff + i;
            if (idx < sizeof(nwii::runtime::gx::g_state.tlutMem) &&
                nwii::runtime::gx::g_state.tlutMem[idx])
              ++tlut_nz;
          }
          st << " tex0=" << std::hex << base << std::dec << "/"
                    << nwii::runtime::gx::g_state.texStages[0].width << "x"
                    << nwii::runtime::gx::g_state.texStages[0].height << "/f"
                    << (int)nwii::runtime::gx::g_state.texStages[0].format
                    << " texnz=" << tex_nz << "/2048 tlutnz=" << tlut_nz << "/512";
          { // SOAK14 diagnostic: texture cache entries/clears, live EFB-copy textures/MB (renderer_gl.cpp)
            extern void nwii_stat_texmem(uint64_t&, uint64_t&, uint64_t&, uint64_t&);
            uint64_t tc = 0, tg = 0, en = 0, eb = 0;
            nwii_stat_texmem(tc, tg, en, eb);
            st << " texcache=" << tc << "/" << tg << " efbcopy=" << en << "/" << (eb >> 20) << "MB";
          }
          st << "\n";
          std::cout << st.str() << std::flush;
        }
      }

      if (!headless && present_now && presenter.on()) {
        // GXF12: hand the frame to the present thread (copy + fence, no swap).
        int pw = 0, ph = 0;
        nwii::runtime::gx::PresentSize(pw, ph);
        // INTERP17: a new frame carries its cut time; on the interpolation schedule it is shown P/2
        // after its in-between frame (kind 2), otherwise at once (kind 0: latency statistics only).
        gxf12::PresentTiming tm;
        const nwii::runtime::gx::InterpFrameInfo fi = nwii::runtime::gx::InterpLastFrameInfo();
        if (new_frame && fi.frame == g_stat_frames && fi.t_cut_us) {
          tm.kind = fi.paced ? 2 : 0;
          tm.t_cut_us = fi.t_cut_us;
          tm.period_us = fi.period_us;
        }
        presenter.Submit(nwii::ui::ComposeFrame(nwii::runtime::gx::PresentFramebuffer(), pw, ph), // UI13 hook
                         pw, ph, g_stat_frames,
                         res9::GameWidescreen(g_stat_frames),
                         nwii::runtime::Config::get().aspect_mode, &tm);
      } else if (!headless && present_now) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        int draw_w = 0, draw_h = 0;
        SDL_GL_GetDrawableSize(window, &draw_w, &draw_h);
        glViewport(0, 0, draw_w, draw_h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        // Present the last XFB copy (renderer-owned 640x480 framebuffer,
        // already scaled from the copy's source rect) to the window.
        // RES9: the present framebuffer is 640s x 480s; it is fitted into the
        // window at the game's aspect (16:9 / 4:3) with black bars.
        if (const unsigned present_fb = nwii::runtime::gx::PresentFramebuffer()) {
          extern uint64_t g_stat_frames;
          int pw = 0, ph = 0, dst[4];
          nwii::runtime::gx::PresentSize(pw, ph);
          res9::FitRect(draw_w, draw_h, res9::GameWidescreen(g_stat_frames),
                        nwii::runtime::Config::get().aspect_mode, dst);
          if (dst[2] > 0 && dst[3] > 0 && draw_w > 0 && draw_h > 0) {
            // Picture area for the mouse source (window-normalised, y from the top).
            nwii::runtime::input::g_present_rect[0].store(float(dst[0]) / draw_w);
            nwii::runtime::input::g_present_rect[1].store(float(draw_h - dst[1] - dst[3]) / draw_h);
            nwii::runtime::input::g_present_rect[2].store(float(dst[2]) / draw_w);
            nwii::runtime::input::g_present_rect[3].store(float(dst[3]) / draw_h);
          }
          if (dst[2] > 0 && dst[3] > 0) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, nwii::ui::ComposeFrame(present_fb, pw, ph)); // UI13 hook
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, pw, ph, dst[0], dst[1], dst[0] + dst[2], dst[1] + dst[3],
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);
          }
          res9_selftest.after_present(window, g_stat_frames, draw_w, draw_h, dst);
        }
      }
      if (!headless && present_now) {
        gpu_timer.end(g_stat_frames != frames_before_gx); // RES9 NWII_GPU_MS
        if (shot_full_requested) { // RES9: F12
          shot_full_requested = false;
          extern uint64_t g_stat_frames;
          res9::SaveFullScreenshot(g_stat_frames);
        }

        // M0.2: screenshots are indexed by game frame (the STAT counter,
        // g_stat_frames, right after the ProcessGXFifo that rendered it), not
        // by host-loop iterations, whose rate varies run to run. Default every
        // 30 game frames (NWII_SCREENSHOT_EVERY). The BMP is opaque (alpha 255,
        // RGB unchanged: what the window shows); NWII_SCREENSHOT_ALPHA=1 adds
        // <name>_alpha.bmp with the EFB alpha as grey.
        static const char *shot_pfx = std::getenv("NWII_SCREENSHOT");
        if (shot_pfx) {
          static const uint64_t shot_every = [] {
            const char *e = std::getenv("NWII_SCREENSHOT_EVERY");
            const long long v = e ? std::atoll(e) : 30;
            return (uint64_t)(v > 0 ? v : 30);
          }();
          static const bool shot_alpha = [] {
            const char *e = std::getenv("NWII_SCREENSHOT_ALPHA");
            return e && *e && *e != '0';
          }();
          // RES9: 640x480 (box-filtered at internal scale > 1) unless
          // NWII_SCREENSHOT_NATIVE=0, which keeps the full present size.
          static const bool shot_native = [] {
            const char *e = std::getenv("NWII_SCREENSHOT_NATIVE");
            return !(e && *e == '0');
          }();
          static uint64_t last_shot = 0;
          extern uint64_t g_stat_frames;
          const uint64_t fr = g_stat_frames;
          if (fr != last_shot && fr % shot_every == 0) {
            last_shot = fr;
            std::vector<unsigned char> px;
            int sw = nwii::runtime::gx::kPresentWidth, sh = nwii::runtime::gx::kPresentHeight;
            if (shot_native)
              nwii::runtime::gx::ReadPresented(px);
            else
              nwii::runtime::gx::ReadPresentedFull(px, sw, sh);
            char num[32];
            std::snprintf(num, sizeof num, "_%06llu", (unsigned long long)fr);
            const std::string base = std::string(shot_pfx) + num;
            nwii::runtime::gx::SaveBMP32(base + ".bmp", px.data(), sw, sh, 0);
            if (shot_alpha)
              nwii::runtime::gx::SaveBMP32(base + "_alpha.bmp", px.data(), sw,
                                           sh, 1);
          }
        }

        prof::host_phase(prof::kHostSwap);
        const auto prof_sw0 = prof::g_on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (prof::g_on)
          prof::bump(prof::kPresentUs, (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                           prof_sw0 - prof_gx1).count());
        if (!presenter.on()) // GXF12: the present thread swaps on its own
          SDL_GL_SwapWindow(window);
        if (prof::g_on)
          prof::bump(prof::kSwapUs, (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - prof_sw0).count());
      }
      prof::host_phase(prof::kHostAudio);

      if (audio_dev > 0) {
        if (SDL_GetQueuedAudioSize(audio_dev) < 1024 * 4 * 2) {
          static int16_t abuf[1024 * 2];
          nwii::runtime::hw::dsp_audio_pull(abuf, 1024);
          SDL_QueueAudio(audio_dev, abuf, 1024 * 4);
        }
      }
      
      prof::host_phase(prof::kHostOther);
      prof::bump(prof::kHostIters);
      if (g_host_pacing.host_vi) ctx->vblank_pending = true;
      if (headless) std::this_thread::sleep_for(std::chrono::milliseconds(16));
      else if (presenter.on()) {
        // GXF12: take the next completed frame at once; otherwise wait until
        // the CPU thread's idle parse completes one (or 2 ms: input, events,
        // and the FIFO drain when the guest never idles).
        if (!(new_frame && GX_PendingFrames() > 0)) {
          prof::host_phase(prof::kHostSleep);
          GX_WaitWork(gx_work_seq, 2);
        }
      } else if (!present_now) {
        // Nothing new to show: wait briefly instead of spinning on the FIFO.
        prof::host_phase(prof::kHostSleep);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }

    nwii::ui::Shutdown(); // UI13 hook: ImGui GL objects, before the context goes away
    nwii::runtime::gx::g_interpPresentHook = nullptr; // INTERP17
    gxf12::g_interp_presenter = nullptr;
    presenter.Stop(); // GXF12: before the shared context goes away
    if (host_drawable) {
      SDL_GL_MakeCurrent(window, gl_ctx);
      SDL_DestroyWindow(host_drawable);
    }
    if (!headless) {
      if (audio_dev > 0) SDL_CloseAudioDevice(audio_dev);
      SDL_GL_DeleteContext(gl_ctx);
      SDL_DestroyWindow(window);
      // INPUT13: a controller thread stuck in a driver holds SDL's joystick lock; SDL_Quit would wait forever.
      if (nwii::runtime::input::stop_controller_thread())
        SDL_Quit();
    }
  }

  nwii::runtime::prof::shutdown_and_report(); // PERF2 (no-op unless NWII_PROFILE)
  ctx->is_running = false;
  ctx->pc = 0; 
  if (cpu_thread.joinable())
    cpu_thread.join();
  GX_StopParseWorker(); // UIT14: the detached PERF13 parse worker must not parse from guest RAM ~ctx frees

  nwii::runtime::shutdown();
  return cpu_failed ? 1 : 0;
}
