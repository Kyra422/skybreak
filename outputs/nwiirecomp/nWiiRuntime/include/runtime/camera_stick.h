#pragma once
// CTRL18-CAM (design work/claude-notes/CTRL18-design.md 3.5, 6; note work/claude-notes/CTRL18-CAM.md):
// the lock-free hand-over between the input layer and the two game-side CTRL18-CAM features.
//
//  1. Free camera ("camera stick"). Producers: the input sources (InputManager thread / any thread):
//     - analog orbit request: camera_stick_set(x, y), x/y in -1..1 (x > 0 = turn the view right, y > 0 = look up,
//       BEFORE the user's invert settings, which the camera applies). Latest value wins; set 0,0 on release or when
//       the routing changes. Switch HD scheme: right stick while L is held.
//     - relative orbit: camera_stick_add_degrees(yaw, pitch) (same sign convention), or
//       camera_mouse_pixels(dx, dy) = raw mouse motion in play mode (dx > 0 right, dy > 0 DOWN, screen pixels),
//       converted here with kMouseDegPerPx x [mouse] sensitivity_camera (g_free_camera.mouse_sensitivity).
//       Accumulated, drained once per guest frame.
//     Consumer: the guest CPU thread, once per guest frame (src/hle/ss_camera.cpp: frame tick in SS_KPADReadEx
//     channel 0, then the seam SS_CamGameExecuteSeam at fn_8006BF50 in dCameraGame_c::execute).
//     Feedback for input routing: camera_free_ok() is true while the game camera is the normal follow (Chase)
//     camera and the free camera may orbit (no Z-target, first person/aim, event/cutscene, map/pause menu, riding,
//     title). Route the right stick / mouse to the camera only while it is true (else aim/pointer as before).
//  2. Rumble: [input] rumble strength 0..100 (g_rumble_strength) and the device that drives each Wii Remote
//     (g_active_device: rumble is off while a remote is driven by keyboard/mouse). src/hle/ss_rumble.cpp forwards
//     the game's WPADControlMotor to the pad of the same slot through the INPUT13 pad_hub request path.
//
// Settings (g_free_camera, g_rumble_strength) are plain atomics: config/launcher/presets store into them at start
// and on live changes; env overrides for tests are applied by ss_camera.cpp / ss_rumble.cpp at their first use
// (NWII_FREECAM=0|1, NWII_CAM_SPEED=1..5, NWII_CAM_INVERT=x|y|xy, NWII_RUMBLE=0..100).
// Header-only, SDL-free and CPUContext-free (tests include it alone).
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace nwii::runtime {
struct CPUContext;
}

namespace nwii::runtime::input {

// ---- 1. Free camera -------------------------------------------------------------------------------------------
// Analog request in milli-units (-1000..1000), latest value wins.
inline std::atomic<int32_t> g_cam_stick_x_milli{0}, g_cam_stick_y_milli{0};
// Relative request in milli-degrees, accumulated (fetch_add) by producers, drained (exchange 0) per guest frame.
inline std::atomic<int32_t> g_cam_delta_yaw_mdeg{0}, g_cam_delta_pitch_mdeg{0};

inline int32_t camera_stick_milli_(float v) {
    if (!std::isfinite(v)) return 0;
    v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
    return static_cast<int32_t>(std::lround(v * 1000.0f));
}
inline void camera_stick_set(float x, float y) {
    g_cam_stick_x_milli.store(camera_stick_milli_(x), std::memory_order_relaxed);
    g_cam_stick_y_milli.store(camera_stick_milli_(y), std::memory_order_relaxed);
}
inline void camera_stick_add_degrees(float yaw_deg, float pitch_deg) {
    const auto mdeg = [](float d) -> int32_t {
        if (!std::isfinite(d)) return 0;
        d = d < -3600.0f ? -3600.0f : d > 3600.0f ? 3600.0f : d;
        return static_cast<int32_t>(std::lround(d * 1000.0f));
    };
    if (const int32_t y = mdeg(yaw_deg)) g_cam_delta_yaw_mdeg.fetch_add(y, std::memory_order_relaxed);
    if (const int32_t p = mdeg(pitch_deg)) g_cam_delta_pitch_mdeg.fetch_add(p, std::memory_order_relaxed);
}

// Published by the seam every guest frame the game camera runs; cleared by the frame tick when it did not run.
inline std::atomic<bool> g_guest_free_camera_ok{false};
inline bool camera_free_ok() { return g_guest_free_camera_ok.load(std::memory_order_relaxed); }

struct FreeCameraSettings {
    std::atomic<bool> enabled{true};       // [camera] free_camera. false = vanilla, bit-exact (seam does nothing)
    std::atomic<bool> scheme_allows{true}; // input layer: false while the "Wii (beweging/gyro)" scheme is active
    std::atomic<int> speed{3};             // [camera] speed 1..5 (90/135/180/240/300 deg/s at full stick)
    std::atomic<bool> invert_x{false};     // [camera] invert_x
    std::atomic<bool> invert_y{false};     // [camera] invert_y
    std::atomic<int> auto_recentre_s{0};   // [camera] auto_recentre_s: 0 = off (user decision: no auto-return)
    std::atomic<float> mouse_sensitivity{1.0f}; // [mouse] sensitivity_camera (x kMouseDegPerPx)
};
inline FreeCameraSettings g_free_camera;

inline constexpr float kMouseDegPerPx = 0.15f;
inline void camera_mouse_pixels(float dx, float dy) {
    const float k = kMouseDegPerPx * g_free_camera.mouse_sensitivity.load(std::memory_order_relaxed);
    camera_stick_add_degrees(dx * k, -dy * k); // mouse up = look up
}

inline bool free_camera_active() {
    return g_free_camera.enabled.load(std::memory_order_relaxed) &&
           g_free_camera.scheme_allows.load(std::memory_order_relaxed);
}

// ---- 2. Rumble ------------------------------------------------------------------------------------------------
inline std::atomic<int> g_rumble_strength{100}; // [input] rumble 0..100 (0 = off)
enum class ActiveDevice : int { Unknown = 0, Gamepad = 1, KeyboardMouse = 2 };
// Which device last drove Wii Remote k (set by the input sources; Unknown = rumble goes to the pad if one is there).
inline std::array<std::atomic<int>, 4> g_active_device{};

// ---- Frame tick ------------------------------------------------------------------------------------------------
// Called by the game adapter once per guest frame (SOUE01: ss_boot.cpp SS_KPADReadEx, channel 0) when set.
// ss_camera.cpp installs it at static initialisation; a build without ss_camera.cpp (unit tests) leaves it null.
inline void (*g_ss_frame_tick)(nwii::runtime::CPUContext&) = nullptr;

} // namespace nwii::runtime::input
