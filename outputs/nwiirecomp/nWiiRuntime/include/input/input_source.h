#pragma once
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace nwii::runtime::input {

struct GameCubePadState {
    uint16_t buttons;
    int8_t stick_x, stick_y;
    int8_t substick_x, substick_y;
    uint8_t trigger_l, trigger_r;
    uint8_t analog_a, analog_b;
    int8_t err;
};

struct NunchukState {
    bool available = false;
    uint32_t buttons = 0; // KPAD masks: Z = 0x2000, C = 0x4000.
    float stick_x = 0.0f, stick_y = 0.0f;
    float accel_x = 0.0f, accel_y = 0.0f, accel_z = 1.0f;
    float accel_speed = 0.0f;
};

enum class MotionSourceKind { None, Virtual };

struct MotionSample {
    bool available = false;
    MotionSourceKind kind = MotionSourceKind::None;
    uint64_t sequence = 0;
    uint64_t monotonic_ns = 0;
    float dt_seconds = 0.0f;
    // Guest axes: x = pitch, y = yaw, z = roll, in radians per second.
    float gyro_x_rad_s = 0.0f, gyro_y_rad_s = 0.0f, gyro_z_rad_s = 0.0f;
    bool sensor_measured = false; // original sensor drift correction only for the physical Wii-motion stream
};

inline constexpr uint64_t kMotionMaxAgeNs = 100'000'000;
// Producer contract for virtual gyro rates. A sword swing needs > 2 rev/s (12.6 rad/s) on
// the KMPLS output (SS dPad::ex_c::acc_c, d_pad.cpp fn_80058540); real swings reach
// 1000-2000 deg/s. 34 rad/s (1948 deg/s) stays inside the MotionPlus fast-mode range that
// ss_motion.cpp encodes (1200 deg/s per 4096 counts around 8192, i.e. up to ~2400 deg/s).
inline constexpr float kVirtualMotionMaxRateRadS = 34.0f;
// Continuous pointer/aim producers (mouse tracking, sticks, replay steering) keep the
// previous 8 rad/s limit; only gestures (motion_gesture.h) use the full range.
inline constexpr float kPointerMotionMaxRateRadS = 8.0f;

// Area of the window that shows the game picture (RES9 black bars), normalised to the
// window: x, y-from-top, width, height. Written by the host loop on present, read by
// the mouse source so the pointer maps to the picture, not to the bars.
inline std::atomic<float> g_present_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};

inline uint64_t motion_now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline bool motion_sample_fresh(const MotionSample& sample, uint64_t now_ns) {
    return sample.available && sample.kind == MotionSourceKind::Virtual && sample.sequence != 0 &&
        sample.monotonic_ns <= now_ns && now_ns - sample.monotonic_ns <= kMotionMaxAgeNs &&
        std::isfinite(sample.dt_seconds) && sample.dt_seconds > 0.0f && sample.dt_seconds <= 0.1f &&
        std::isfinite(sample.gyro_x_rad_s) && std::isfinite(sample.gyro_y_rad_s) &&
        std::isfinite(sample.gyro_z_rad_s);
}

// Combine explicit virtual controls per axis. A neutral source cannot erase
// another source's movement; the first source wins an equal-magnitude tie.
// Timestamp, sequence and dt belong to the InputManager frame, not a source.
inline void merge_virtual_motion(MotionSample& sample, bool source_available,
                                 float pitch, float yaw, float roll) {
    if (!source_available) return;
    sample.kind = MotionSourceKind::Virtual;
    if (sample.sequence == 0 || !std::isfinite(sample.dt_seconds) ||
        sample.dt_seconds <= 0.0f || sample.dt_seconds > 0.1f ||
        !std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll)) return;
    sample.available = true;
    const auto merge_axis = [](float& destination, float source) {
        source = std::clamp(source, -kVirtualMotionMaxRateRadS, kVirtualMotionMaxRateRadS);
        if (std::abs(source) > std::abs(destination)) destination = source;
    };
    merge_axis(sample.gyro_x_rad_s, pitch);
    merge_axis(sample.gyro_y_rad_s, yaw);
    merge_axis(sample.gyro_z_rad_s, roll);
}

// Sources contribute once per input update. Keep the strongest stick (first
// source wins ties), while combining buttons; a neutral source cannot erase
// another source's active stick. InputManager clears this state every frame.
inline void merge_nunchuk(NunchukState& destination, const NunchukState& source) {
    if (!source.available) return;
    const uint32_t buttons = destination.buttons | source.buttons;
    const float old_length = destination.stick_x * destination.stick_x +
                             destination.stick_y * destination.stick_y;
    const float new_length = source.stick_x * source.stick_x +
                             source.stick_y * source.stick_y;
    if (!destination.available || new_length > old_length)
        destination = source;
    destination.available = true;
    destination.buttons = buttons;
}

struct WiimoteState {
    uint32_t buttons;
    float ir_x, ir_y;
    float accel_x, accel_y, accel_z;
    float gyro_pitch, gyro_yaw, gyro_roll;
    int8_t err;
    NunchukState nunchuk{};
    MotionSample motion{};
    float ir_horizon_x=1.0f, ir_horizon_y=0.0f;
};

class IInputSource {
public:
    virtual ~IInputSource() = default;

    
    
    virtual void update(GameCubePadState pads[4], WiimoteState motes[4]) = 0;
};

} 
