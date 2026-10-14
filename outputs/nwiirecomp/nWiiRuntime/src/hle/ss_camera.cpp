// CTRL18-CAM (T2, design work/claude-notes/CTRL18-design.md 3.5, study work/ctrl18/camera-hook-study.md):
// SOUE01 free/orbit camera through ONE opt-in game hook, plus the once-per-guest-frame tick that latches the
// camera-stick input (include/runtime/camera_stick.h) and drives the rumble forwarding (ss_rumble.cpp).
//
// Hook. [hle_hooks] "8006BF50" = "SS_CamGameExecuteSeam". fn_8006BF50 is an empty function (one blr) with a single
// caller, 0x8007EB08 at the start of dCameraGame_c::execute (0x8007EAD0), r28 = the game camera (mr r28,r3),
// before this frame's trend/mode/handler decision, the handler (sCamDefs 0x8050E750) that writes mView2 (+0x38)
// and fn_80080090's collision (mView2 -> mView).
//  - OFF ([camera] free_camera = false, or the Wii-motion scheme): ++ctx.inst_count (the replaced blr) and return.
//    No guest read, no guest write: bit-exact with the original stub (whose body is ++inst_count; pc = lr).
//  - ON, gate closed or no input: guest reads only.
//  - ON, gate open, input: rotate last frame's mView2 eye around its target by (dyaw, dpitch) keeping the distance,
//    elevation clamped. THE ONLY GAME WRITE: 3 floats, gamecam +0x38..+0x40. The game's own Chase handler (R/U
//    limits, smoothing, chkCameraPoint) and fn_80080090's line checks then run unchanged: walls pull the eye in as
//    in vanilla, standing still keeps the yaw, running trails behind Link, the pitch springs back, Z re-centres.
//    Movement is camera-relative already (dAcPy_c::handlesInputMaybe: Link+0x4420 = dCamera_c::getYRot + stick).
// Gate (previous frame's decision, read before this frame's getNewMode):
//    dScGame_c::CAMERAS[0] valid, no title manager, r28 == CAMERAS[0] + 0x2B4 (mGameCam1), mActiveCameraIdx (+0xDA8)
//    == 0 (game camera), no event (EventManager +0x184 == 0), mode (+0x6C) == 0 (normal), handler (+0x74) == 1
//    (Chase), no re-place/wait flags (+0x58 & 0x4081), no Z attention (+0x79), Link ActionFlagsCont (+0x368) &
//    0x5D7 == 0 (no first person / item aim), not riding (+0x339 == 0), Chase handler object past its first-run
//    frame (+0 != 0) and its frame counter (+0x24) advanced by exactly 1 since the last seam call (the camera
//    really ran last frame: no pause/map early return), no game menu pointer shown.
//    Explicitly NOT orbiting: Z-target/lock-on (modes 1/6/9), Z re-centre (7), first person/aim (2/3/4/5/8), fixed/
//    turn/tracking cameras (Dive, SkyDive, Loftwing, tightrope, minecart, Beetle: handler != 1), events/cutscenes,
//    map/pause menus, title.
// Input (per guest frame, fixed step = 1/30 s, deterministic for a given input sequence): analog request x speed
// (5 levels: 90/135/180/240/300 deg/s) with a 0.15 s ramp-up, plus relative degrees (mouse), clamped to 10 deg/frame
// (INTERP16/17 camera-cut guards: > 12 deg/frame spike, > 50 deg/frame), remainder carried (capped). Invert X/Y.
// Optional auto-return behind Link ([camera] auto_recentre_s > 0; default 0 = off, user decision).
//
// Env: NWII_FREECAM=0|1, NWII_CAM_SPEED=1..5, NWII_CAM_INVERT=x|y|xy, NWII_CAM_RECENTRE=<s> (overrides);
//      NWII_CAM_TEST=<yaw deg/s>,<pitch deg/s>[,<delay frames>[,<frames>]] test feed: a stick held at that rate (same ramp) while
//      the gate is open (after <delay> gate-open frames, for <frames> gate-open frames; default 0 / unlimited);
//      NWII_CAM_LOG=1 "[SS freecam]" lines (gate changes with the reason, orbit writes every 15 frames, stats every
//      300 frames); also on with NWII_CAM_TEST.
#include "ss_camera.h"
#include "runtime/camera_stick.h"
#include "../input/virtual_motion.h" // g_guest_pointer_cursor (game menu pointer, published by SS_KPADReadEx)
#include "runtime/cpu_context.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using nwii::runtime::CPUContext;
namespace in = nwii::runtime::input;

namespace nwii::runtime::ss_camera {
namespace {

// SOUE01 addresses (main.dol; decomp d_camera.h, d_camera_game.h, d_a_player.h, event_manager.h).
constexpr uint32_t kCameras0 = 0x80575788;  // dScGame_c::CAMERAS[0] -> dCamera_c
constexpr uint32_t kLink = 0x8057578C;      // dAcPy_c::LINK
constexpr uint32_t kEventMgr = 0x80575378;  // EventManager::sInstance (mState +0x184)
constexpr uint32_t kTitleMgr = 0x805756B4;  // dTitleMgr_c::sInstance
constexpr uint32_t kCamDefs = 0x8050E750;   // sCamDefs: 16 bytes per handler {handler*, ...}
constexpr uint32_t kTrends = 0x80575358;    // camparam trends (name +0, 0x24 each), log only
constexpr uint32_t kGameCam1 = 0x2B4, kActiveIdx = 0xDA8;                     // dCamera_c
constexpr uint32_t kGFlags = 0x58, kGTrend = 0x68, kGMode = 0x6C, kGHandler = 0x74, kGAttention = 0x79;
constexpr uint32_t kGEye = 0x38, kGAt = 0x44;                                 // dCameraGame_c mView2
constexpr uint32_t kGFlagsBlock = 0x1 | 0x80 | 0x4000;                        // re-place / wait for pad / re-place
constexpr uint32_t kHState = 0x00, kHCounter = 0x24;                          // GameCameraHandler
constexpr uint32_t kLinkRotY = 0xBA, kLinkRiding = 0x339, kLinkActCont = 0x368, kLinkAimMask = 0x5D7;
constexpr float kFrameS = 1.0f / 30.0f;
constexpr float kSpeedDegS[5] = {90.0f, 135.0f, 180.0f, 240.0f, 300.0f};
constexpr float kRampS = 0.15f, kCarryCapDeg = 90.0f, kRecentreDegPerFrame = 3.0f;
constexpr double kPi = 3.14159265358979323846;

bool valid(uint32_t p, uint32_t size) {
    return (p >= 0x80000000u && uint64_t(p) + size <= 0x81800000u) ||
           (p >= 0x90000000u && uint64_t(p) + size <= 0x94000000u);
}

struct TestFeed { bool on = false; float yaw = 0, pitch = 0; uint64_t delay = 0, frames = ~0ull; };

struct State {
    bool env_done = false;
    bool log = false;
    TestFeed test;
    Stats st;
    // frame tick / sample
    uint64_t tick = 0, seam_tick = 0;
    bool gate_last = false;      // the seam's last decision
    float rate_x = 0, rate_y = 0; // ramped stick rates (deg/s, before invert)
    float carry_yaw = 0, carry_pitch = 0;
    float s_yaw = 0, s_pitch = 0; // this frame's sample (deg, after invert)
    bool s_pending = false;
    uint64_t open_frames = 0;    // consecutive gate-open frames (test feed clock)
    uint64_t test_frames = 0;
    uint64_t idle_frames = 0;    // gate-open frames without orbit input (auto-return)
    bool user_moved = false;
    // seam
    uint32_t last_h = 0, last_counter = 0;
    const char* last_reason = "";
    uint64_t last_log_tick = 0;
};
State& S() {
    static State s;
    return s;
}

void apply_env() {
    State& s = S();
    if (s.env_done) return;
    s.env_done = true;
    if (const char* e = std::getenv("NWII_FREECAM"); e && e[0]) in::g_free_camera.enabled.store(e[0] != '0');
    if (const char* e = std::getenv("NWII_CAM_SPEED"); e && e[0]) {
        const long v = std::strtol(e, nullptr, 10);
        if (v >= 1 && v <= 5) in::g_free_camera.speed.store(int(v));
    }
    if (const char* e = std::getenv("NWII_CAM_INVERT"); e && e[0]) {
        in::g_free_camera.invert_x.store(std::strchr(e, 'x') != nullptr || std::strchr(e, 'X') != nullptr);
        in::g_free_camera.invert_y.store(std::strchr(e, 'y') != nullptr || std::strchr(e, 'Y') != nullptr);
    }
    if (const char* e = std::getenv("NWII_CAM_RECENTRE"); e && e[0])
        in::g_free_camera.auto_recentre_s.store(std::max(0, int(std::strtol(e, nullptr, 10))));
    s.test = {};
    if (const char* e = std::getenv("NWII_CAM_TEST"); e && e[0]) {
        double v[4] = {0, 0, 0, -1};
        int n = 0;
        const char* p = e;
        while (n < 4 && *p) {
            char* end = nullptr;
            v[n++] = std::strtod(p, &end);
            if (end == p) break;
            p = end;
            while (*p == ',' || *p == ' ') ++p;
        }
        if (n >= 1 && std::isfinite(v[0]) && std::isfinite(v[1])) {
            s.test.on = true;
            s.test.yaw = float(std::clamp(v[0], -600.0, 600.0));
            s.test.pitch = float(std::clamp(v[1], -600.0, 600.0));
            s.test.delay = v[2] > 0 ? uint64_t(v[2]) : 0;
            s.test.frames = v[3] >= 0 ? uint64_t(v[3]) : ~0ull;
        }
    }
    const char* l = std::getenv("NWII_CAM_LOG");
    s.log = (l && l[0] && l[0] != '0') || s.test.on;
    if (s.log) {
        std::printf("[SS freecam] settings: free_camera=%d scheme_allows=%d speed=%d invert_x=%d invert_y=%d "
                    "auto_recentre_s=%d test=%s (yaw %.1f pitch %.1f deg/s, delay %llu, frames %lld)\n",
                    int(in::g_free_camera.enabled.load()), int(in::g_free_camera.scheme_allows.load()),
                    in::g_free_camera.speed.load(), int(in::g_free_camera.invert_x.load()),
                    int(in::g_free_camera.invert_y.load()), in::g_free_camera.auto_recentre_s.load(),
                    s.test.on ? "on" : "off", double(s.test.yaw), double(s.test.pitch),
                    (unsigned long long)s.test.delay,
                    s.test.frames == ~0ull ? -1ll : (long long)s.test.frames);
        std::fflush(stdout);
    }
}

void clear_sample(State& s) {
    s.rate_x = s.rate_y = 0;
    s.carry_yaw = s.carry_pitch = 0;
    s.s_yaw = s.s_pitch = 0;
    s.s_pending = false;
}

float ramp(float cur, float target, float max_step) {
    // Speeding up is ramped (0.15 s from 0 to full speed); stopping or slowing down is immediate; a reversal
    // starts again from 0.
    if (target == 0.0f) return 0.0f;
    if (cur != 0.0f && (target > 0.0f) != (cur > 0.0f)) cur = 0.0f;
    if (std::fabs(target) <= std::fabs(cur)) return target;
    return cur + std::clamp(target - cur, -max_step, max_step);
}

void installed_tick(CPUContext& ctx) { frame_tick(ctx); }
const bool s_installed = (in::g_ss_frame_tick = &installed_tick, true);

} // namespace

Stats stats() { return S().st; }

void reset_for_test() {
    S() = State{};
    in::g_guest_free_camera_ok.store(false);
}

bool orbit_eye(const float eye[3], const float at[3], float dyaw_deg, float dpitch_deg, float out[3]) {
    const double dx = double(eye[0]) - at[0], dy = double(eye[1]) - at[1], dz = double(eye[2]) - at[2];
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) || !std::isfinite(dyaw_deg) ||
        !std::isfinite(dpitch_deg))
        return false;
    const double rh = std::hypot(dx, dz), r = std::sqrt(rh * rh + dy * dy);
    if (r < 1.0 || rh < 1e-3) return false;
    // View direction f = at - eye = -d. In the game's right-handed, y-up world the view's right vector is
    // f x up, so "turn the view right" rotates f (and d) clockwise seen from above: the yaw atan2(x, z) decreases.
    const double yaw = std::atan2(dx, dz) - dyaw_deg * (kPi / 180.0);
    double nx, ny, nz;
    if (dpitch_deg == 0.0f) {
        nx = rh * std::sin(yaw);
        nz = rh * std::cos(yaw);
        ny = dy; // pure yaw: height kept exactly
    } else {
        const double el = std::atan2(dy, rh);
        const double lo = std::min(double(kElMinDeg) * kPi / 180.0, el);
        const double hi = std::max(double(kElMaxDeg) * kPi / 180.0, el);
        // look up = the eye goes down (elevation decreases)
        const double nel = std::clamp(el - dpitch_deg * (kPi / 180.0), lo, hi);
        nx = r * std::cos(nel) * std::sin(yaw);
        nz = r * std::cos(nel) * std::cos(yaw);
        ny = r * std::sin(nel);
    }
    out[0] = float(at[0] + nx);
    out[1] = float(at[1] + ny);
    out[2] = float(at[2] + nz);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

void frame_tick(CPUContext& ctx) {
    (void)ctx;
    apply_env();
    State& s = S();
    ++s.tick;
    ++s.st.ticks;
    // The seam did not run in the previous guest frame (game camera inactive: event, map camera, title...): the
    // gate is closed for input routing.
    if (s.seam_tick + 1 != s.tick) {
        s.gate_last = false;
        s.open_frames = 0;
        in::g_guest_free_camera_ok.store(false, std::memory_order_relaxed);
    }
    ss_rumble::frame_tick();
    if (s.log && s.tick % 300 == 0) {
        const auto r = ss_rumble::stats();
        std::printf("[SS freecam] stats t=%llu seam=%llu open=%llu orbit_writes=%llu reads=%llu writes=%llu | "
                    "rumble motor_calls=%llu on=%llu on_frames=%llu pad_requests=%llu\n",
                    (unsigned long long)s.tick, (unsigned long long)s.st.seam_calls,
                    (unsigned long long)s.st.gate_open, (unsigned long long)s.st.orbit_writes,
                    (unsigned long long)s.st.guest_reads, (unsigned long long)s.st.guest_writes,
                    (unsigned long long)r.motor_calls, (unsigned long long)r.motor_on_calls,
                    (unsigned long long)r.on_frames, (unsigned long long)r.pad_requests);
        std::fflush(stdout);
    }
    // Drain the relative input every frame (also while the gate is closed: no build-up).
    const float m_yaw = float(in::g_cam_delta_yaw_mdeg.exchange(0, std::memory_order_relaxed)) / 1000.0f;
    const float m_pitch = float(in::g_cam_delta_pitch_mdeg.exchange(0, std::memory_order_relaxed)) / 1000.0f;
    s.s_pending = false;
    if (!in::free_camera_active() || !s.gate_last) {
        clear_sample(s);
        return;
    }
    const int speed = std::clamp(in::g_free_camera.speed.load(std::memory_order_relaxed), 1, 5);
    const float full = kSpeedDegS[speed - 1];
    const float max_step = full * kFrameS / kRampS;
    float tx = float(in::g_cam_stick_x_milli.load(std::memory_order_relaxed)) / 1000.0f * full;
    float ty = float(in::g_cam_stick_y_milli.load(std::memory_order_relaxed)) / 1000.0f * full;
    // The test feed acts like a stick held at a fixed rate (same ramp), so it also exercises the ramp.
    if (s.test.on && s.open_frames >= s.test.delay && s.test_frames < s.test.frames) {
        tx += s.test.yaw;
        ty += s.test.pitch;
        ++s.test_frames;
    }
    s.rate_x = ramp(s.rate_x, tx, max_step);
    s.rate_y = ramp(s.rate_y, ty, max_step);
    // Rates are clamped per frame below; only the relative (mouse) input carries a remainder to later frames.
    const float rel_yaw = m_yaw + s.carry_yaw, rel_pitch = m_pitch + s.carry_pitch;
    const float ry = std::clamp(s.rate_x * kFrameS, -kMaxStepDeg, kMaxStepDeg);
    const float rp = std::clamp(s.rate_y * kFrameS, -kMaxStepDeg, kMaxStepDeg);
    const float yaw = ry + rel_yaw, pitch = rp + rel_pitch;
    float cy = std::clamp(yaw, -kMaxStepDeg, kMaxStepDeg), cp = std::clamp(pitch, -kMaxStepDeg, kMaxStepDeg);
    s.carry_yaw = std::clamp(yaw - cy, -kCarryCapDeg, kCarryCapDeg);
    s.carry_pitch = std::clamp(pitch - cp, -kCarryCapDeg, kCarryCapDeg);
    if (std::fabs(s.carry_yaw) < 1e-3f) s.carry_yaw = 0.0f; // no float residue
    if (std::fabs(s.carry_pitch) < 1e-3f) s.carry_pitch = 0.0f;
    if (std::fabs(cy) < 1e-4f) cy = 0.0f;
    if (std::fabs(cp) < 1e-4f) cp = 0.0f;
    s.s_yaw = in::g_free_camera.invert_x.load(std::memory_order_relaxed) ? -cy : cy;
    s.s_pitch = in::g_free_camera.invert_y.load(std::memory_order_relaxed) ? -cp : cp;
    s.s_pending = s.s_yaw != 0.0f || s.s_pitch != 0.0f;
}

} // namespace nwii::runtime::ss_camera

extern "C" void SS_CamGameExecuteSeam(CPUContext& ctx) {
    using namespace nwii::runtime::ss_camera;
    ++ctx.inst_count; // the replaced body: one blr (the generated stub counts it the same way)
    apply_env();      // host-side only (env overrides, once)
    if (!in::free_camera_active()) {
        // OFF: no guest access at all.
        in::g_guest_free_camera_ok.store(false, std::memory_order_relaxed);
        return;
    }
    State& s = S();
    ++s.st.seam_calls;
    s.seam_tick = s.tick;
    auto& mmu = ctx.mmu;
    uint64_t reads = 0;
    const auto r32 = [&](uint32_t a) { ++reads; return mmu.read32(a); };
    const auto r16 = [&](uint32_t a) { ++reads; return mmu.read16(a); };
    const auto r8 = [&](uint32_t a) { ++reads; return mmu.read8(a); };
    const auto rf = [&](uint32_t a) { ++reads; return mmu.read_f32(a); };

    const char* reason = nullptr;
    int32_t mode = -1, trend = -1;
    uint32_t handler = ~0u, flags = 0;
    const uint32_t cam = r32(kCameras0);
    const uint32_t g = ctx.gpr[28];
    const uint32_t link = r32(kLink);
    uint32_t h = 0;
    if (!valid(cam, 0xDB0)) reason = "no camera";
    else if (valid(r32(kTitleMgr), 4)) reason = "title";
    else if (g != cam + kGameCam1) reason = "not the game camera";
    else if (r32(cam + kActiveIdx) != 0) reason = "camera not active";
    if (!reason) {
        mode = int32_t(r32(g + kGMode));
        handler = r32(g + kGHandler);
        flags = r32(g + kGFlags);
        trend = int32_t(r32(g + kGTrend));
        h = r32(kCamDefs + 1 * 16); // Chase handler object (GameCameraHandler01)
    }
    // The handler counter is tracked on every call so the "ran last frame" test sees consecutive frames.
    const uint32_t counter = valid(h, 0x28) ? r32(h + kHCounter) : 0;
    const bool counter_ok = h != 0 && h == s.last_h && counter == s.last_counter + 1;
    s.last_h = h;
    s.last_counter = counter;
    if (!reason) {
        const uint32_t ev = r32(kEventMgr);
        if (valid(ev, 0x188) && r32(ev + 0x184) != 0) reason = "event";
        else if (mode != 0) reason = "mode";
        else if (handler != 1) reason = "handler";
        else if (flags & kGFlagsBlock) reason = "re-place/wait flags";
        else if (r8(g + kGAttention) != 0) reason = "Z attention";
        else if (!valid(link, 0x4620)) reason = "no Link";
        else if (r32(link + kLinkActCont) & kLinkAimMask) reason = "aim/first person";
        else if (r8(link + kLinkRiding) != 0) reason = "riding";
        else if (!valid(h, 0x28) || r8(h + kHState) == 0) reason = "handler first run";
        else if (!counter_ok) reason = "camera did not run last frame";
        else if (in::g_guest_pointer_cursor.load(std::memory_order_relaxed)) reason = "menu pointer";
    }
    const bool ok = reason == nullptr;
    in::g_guest_free_camera_ok.store(ok, std::memory_order_relaxed);
    if (ok) {
        ++s.st.gate_open;
        ++s.open_frames;
    } else {
        s.open_frames = 0;
        s.idle_frames = 0;
    }
    if (s.log && (ok != s.gate_last || (!ok && std::strcmp(reason, s.last_reason) != 0))) {
        char tname[17] = {};
        const uint32_t trends = valid(cam, 4) ? r32(kTrends) : 0;
        if (valid(trends, 4) && trend >= 0 && trend < 256)
            for (uint32_t i = 0; i < 16; ++i)
                if (!(tname[i] = char(r8(trends + uint32_t(trend) * 0x24 + i)))) break;
        for (char& c : tname) if (c && (c < 0x20 || c > 0x7E)) c = '?';
        std::printf("[SS freecam] t=%llu gate %s%s%s mode=%d handler=%d trend=%d'%s' flags=0x%X\n",
                    (unsigned long long)s.tick, ok ? "OPEN" : "CLOSED", ok ? "" : ": ", ok ? "" : reason, mode,
                    int(handler), trend, tname, flags);
        std::fflush(stdout);
    }
    s.gate_last = ok;
    s.last_reason = ok ? "" : reason;

    float dyaw = 0, dpitch = 0;
    bool user = false;
    if (ok && s.s_pending) {
        dyaw = s.s_yaw;
        dpitch = s.s_pitch;
        user = true;
    }
    s.s_pending = false; // a sample is used by at most one seam call
    // Optional auto-return behind Link (default off): after auto_recentre_s seconds without orbit input, once the
    // player had turned the camera, the eye's yaw moves back toward Link's back at <= 3 deg/frame.
    const int recentre_s = in::g_free_camera.auto_recentre_s.load(std::memory_order_relaxed);
    if (ok && !user && recentre_s > 0 && s.user_moved && ++s.idle_frames >= uint64_t(recentre_s) * 30u) {
        const float ex = rf(g + kGEye), ez = rf(g + kGEye + 8), ax = rf(g + kGAt), az = rf(g + kGAt + 8);
        const double cur = std::atan2(double(ex) - ax, double(ez) - az) * 180.0 / kPi;
        const double behind = double(int16_t(r16(link + kLinkRotY))) * (360.0 / 65536.0) + 180.0;
        double diff = std::remainder(cur - behind, 360.0); // > 0: turning the view right (yaw decreasing) reduces it
        if (std::fabs(diff) <= 2.0) s.user_moved = false;
        else dyaw = float(std::clamp(diff, -double(kRecentreDegPerFrame), double(kRecentreDegPerFrame)));
    }
    if (user) {
        s.idle_frames = 0;
        s.user_moved = true;
    }
    s.st.guest_reads += reads;
    if (!ok || (dyaw == 0.0f && dpitch == 0.0f)) return; // ON but idle or gated: reads only
    const float eye[3] = {rf(g + kGEye), rf(g + kGEye + 4), rf(g + kGEye + 8)};
    const float at[3] = {rf(g + kGAt), rf(g + kGAt + 4), rf(g + kGAt + 8)};
    s.st.guest_reads += 6;
    float out[3];
    if (!orbit_eye(eye, at, dyaw, dpitch, out)) return;
    mmu.write_f32(g + kGEye, out[0]);
    mmu.write_f32(g + kGEye + 4, out[1]);
    mmu.write_f32(g + kGEye + 8, out[2]);
    s.st.guest_writes += 3;
    ++s.st.orbit_writes;
    if (s.log && (s.tick - s.last_log_tick >= 15 || s.st.orbit_writes == 1)) {
        s.last_log_tick = s.tick;
        const double y0 = std::atan2(double(eye[0]) - at[0], double(eye[2]) - at[2]) * 180.0 / kPi;
        const double y1 = std::atan2(double(out[0]) - at[0], double(out[2]) - at[2]) * 180.0 / kPi;
        std::printf("[SS freecam] t=%llu orbit dyaw=%.2f dpitch=%.2f%s eye (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f) "
                    "at (%.1f,%.1f,%.1f) yaw %.1f->%.1f writes=%llu\n",
                    (unsigned long long)s.tick, double(dyaw), double(dpitch), user ? "" : " (auto-return)",
                    double(eye[0]), double(eye[1]), double(eye[2]), double(out[0]), double(out[1]), double(out[2]),
                    double(at[0]), double(at[1]), double(at[2]), y0, y1, (unsigned long long)s.st.orbit_writes);
        std::fflush(stdout);
    }
}
