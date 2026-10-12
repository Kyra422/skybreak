#pragma once
// INPUT10 (M4.3): real controller motion sensors (DualSense, DualShock 4, Switch Pro: SDL gyroscope +
// accelerometer) as the sensors of the virtual Wii Remote + MotionPlus. SDL-free on purpose: the
// InputManager (built without SDL in several harnesses) consumes g_measured_motion; the SDL event
// capture lives in sources/gamepad_source.cpp.
//
// Axes. SDL game-controller sensors (SDL_sensor.h): X = right, Y = up (out of the face), Z = toward the
// player; gyro in rad/s, counter-clockwise positive (right-hand rule); accelerometer = specific force in
// m/s^2 (+9.81 on Y lying face up at rest). The virtual remote (virtual_motion.h): X = left, Y = up
// (button face), Z = forward (pointing end); +x = nose down, +y = turn left.
// Natural grip: the pad held face up in both hands, its far edge toward the screen = a remote held level
// and pointed at the screen. That is a fixed 180 degree turn about the up axis (det +1):
//     remote = (-x, y, -z) of the controller vector, for rates and for forces alike,
// so tilting the far edge up/down = remote pitch, turning left/right = remote yaw, rolling = remote roll.
// KPAD acceleration = -(specific force)/g in remote axes: (0,-1,0) level at rest, the same convention as
// the virtual gravity (virtual_motion.h gravity(), motion_gesture.h). Because gyro and accelerometer come
// from one rigid body and get the same rotation, the original KMPLS accelerometer revision and the
// dPad::ex_c::acc_c swing / shake detectors see physically consistent data; nothing is filtered or
// shaped except (optionally) a constant gyro bias measured while the controller lies still.
//
// Time base. Every sensor sample keeps its own hardware interval (SDL timestamp_us), and samples are
// merged into sub-steps of >= 4 ms (angle-preserving). The InputManager records each sub-step as one
// MotionStep of the update (several steps share the update's sequence), so ss_motion.cpp's 200 Hz
// KMPLS resampling and ss_boot.cpp's multi-sample KPAD reads see the real rate/accel profile instead of
// one value per host frame. Rates are clamped to the producer contract (kVirtualMotionMaxRateRadS).
#include "virtual_motion.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nwii::runtime::input {

inline constexpr double kStandardGravity = 9.80665;

struct SensorEvent {
    uint64_t t_us = 0;  // sensor timestamp (SDL timestamp_us); 0 = not provided by the hardware
    bool gyro = true;   // false = accelerometer
    float v[3] = {0, 0, 0};
};

struct MeasuredSubStep {
    float dt = 0.0f;                 // seconds, (0, 0.1]
    float rate[3] = {0, 0, 0};       // remote body rates, rad/s (x pitch nose-down, y yaw left, z roll)
    float accel[3] = {0, -1.0f, 0};  // KPAD acceleration, g
};
inline constexpr size_t kMaxMeasuredSubSteps = 32;
// Written by the gamepad source during an InputManager update (sequence = that update's
// MotionSample::sequence), consumed by InputManager::update_locked right after the sources ran.
// Producer-owned like g_virtual_gesture: only the InputManager update thread touches it.
struct MeasuredMotionFrame {
    uint64_t sequence = 0;
    size_t count = 0;
    std::array<MeasuredSubStep, kMaxMeasuredSubSteps> steps{};
};
inline MeasuredMotionFrame g_measured_motion[4];

// Gamepad motion mode for every controller: 0 = off (sticks/buttons), 1 = gyro. -1 until the gamepad
// source applies [input] gamepad_motion; toggled in game by the gyro_toggle binding (hold 1 s).
inline std::atomic<int> g_gamepad_motion_mode{-1};
// True while a controller's real sensors drive channel i (gyro mode and the controller has sensors).
inline std::atomic<bool> g_gyro_drives_channel[4];

inline void controller_to_remote(const float v[3], float out[3]) {
    out[0] = -v[0];
    out[1] = v[1];
    out[2] = -v[2];
}
inline void controller_accel_to_kpad(const float a[3], float out[3]) {
    float r[3];
    controller_to_remote(a, r);
    for (int k = 0; k < 3; ++k) out[k] = static_cast<float>(-r[k] / kStandardGravity);
}

// Producer-side only: keeps the virtual orientation's tilt on the measured gravity (complementary
// filter, body rate = gain * (measured x predicted)), so stick/mouse/gesture control resumes from the
// real pose when gyro mode is switched off. The game never sees this correction.
inline void correct_tilt(VirtualRemoteOrientation& o, const float accel[3], double dt, double gain = 2.0) {
    const double n = std::sqrt(double(accel[0]) * accel[0] + double(accel[1]) * accel[1] + double(accel[2]) * accel[2]);
    if (!(dt > 0.0) || !(n > 0.0) || std::fabs(n - 1.0) > 0.15) return;
    float g[3];
    o.gravity(g);
    const double m[3] = {accel[0] / n, accel[1] / n, accel[2] / n};
    const float w[3] = {static_cast<float>(gain * (m[1] * g[2] - m[2] * g[1])),
                        static_cast<float>(gain * (m[2] * g[0] - m[0] * g[2])),
                        static_cast<float>(gain * (m[0] * g[1] - m[1] * g[0]))};
    o.integrate(w, dt);
}

// One controller's sensor stream -> sub-steps for each InputManager update.
class GyroMotionTracker {
public:
    bool auto_calibrate = true;
    bool allow_bias_learning = true; // Freeze bias learning during intentional hybrid aim/puzzle motion.
    // [input] gyro_grip_tilt (radians): the far edge is raised this much when the pad is held "level".
    // The virtual remote is the pad turned by this angle nose-down about its X axis (one fixed rotation
    // for gyro and accelerometer alike, so the data stay those of one rigid body). 0 = face up is level.
    double grip_tilt = 0.0;

    void reset() {
        samples_.clear();
        allow_bias_learning = true;
        has_pending_ = false;
        have_gyro_ = have_accel_ = have_prev_t_ = false;
        last_gyro_[0] = last_gyro_[1] = last_gyro_[2] = 0.0f;
        last_accel_[0] = 0.0f; last_accel_[1] = float(kStandardGravity); last_accel_[2] = 0.0f;
        nominal_period_ = 1.0 / 250.0;
        samples_received = samples_delivered = 0; // per activation (the samples/s log line)
        window_reset();
        // The bias estimate belongs to the device and survives a mode toggle; reset_bias() clears it.
    }
    void set_bias(const float v[3]) { std::copy(v,v+3,bias_);bias_valid_=true;window_reset(); }
    void reset_bias() { bias_[0] = bias_[1] = bias_[2] = 0.0f; bias_valid_ = false; bias_updates = 0; }
    // Drops queued samples (stalled producer: their interval cannot be delivered) but keeps the bias and
    // the last readings; the next sample starts a new time base.
    void discard() {
        samples_.clear();
        has_pending_ = false;
        have_prev_t_ = false;
    }

    void add(const SensorEvent& e) {
        if (!std::isfinite(e.v[0]) || !std::isfinite(e.v[1]) || !std::isfinite(e.v[2])) return;
        if (e.t_us && have_prev_t_ && e.t_us < prev_t_us_) return; // delayed packets cannot replace newer readings
        if (e.t_us == 0) {
            // Untimed hardware: a gyro event starts a sample, the next accelerometer event completes it.
            if (e.gyro) {
                finalize_pending();
                start_pending(0, false);
            } else if (!has_pending_ || pending_.has_accel) {
                finalize_pending();
                start_pending(0, false);
            }
        } else if (!has_pending_ || pending_.t_us != e.t_us) {
            finalize_pending();
            start_pending(e.t_us, true);
        }
        float* dst = e.gyro ? pending_.gyro : pending_.accel;
        for (int k = 0; k < 3; ++k) dst[k] = e.v[k];
        (e.gyro ? pending_.has_gyro : pending_.has_accel) = true;
    }

    // Sub-steps for one InputManager update of `update_dt` seconds. Returns 0 only before the first
    // sensor sample or when this poll received no new sensor interval.
    size_t produce(double update_dt, MeasuredSubStep* out, size_t max) {
        finalize_pending();
        if (max == 0) return 0;
        // A poll without a hardware sample covers no additional sensor time.
        // The raw Wii path holds its last published motion sample; the hybrid
        // path emits no angular delta. Neither invents zero-rate intervals.
        if (samples_.empty()) return 0;
        // Per-sample intervals from the hardware timestamps.
        size_t untimed = 0;
        for (const auto& s : samples_) untimed += !s.timed;
        std::vector<double>& d = durations_;
        d.assign(samples_.size(), nominal_period_);
        for (size_t k = 0; k < samples_.size(); ++k) {
            const Sample& s = samples_[k];
            if (!s.timed) {
                d[k] = std::clamp(update_dt / double(untimed), 1e-4, 0.05);
                continue;
            }
            if (have_prev_t_ && s.t_us <= prev_t_us_) {
                // Paired gyro/accel events can cross a host-poll boundary. A repeated
                // timestamp updates the last readings, but covers no new rotation.
                d[k] = 0;
                continue;
            }
            if (have_prev_t_ && s.t_us > prev_t_us_) {
                const double dd = double(s.t_us - prev_t_us_) * 1e-6;
                if (dd <= 0.05) {
                    d[k] = dd;
                    nominal_period_ += 0.05 * (std::clamp(dd, 0.0005, 0.05) - nominal_period_);
                } else {
                    d[k] = 0; // A hardware gap is not a synthetic extra nominal-period rotation.
                    window_reset();
                }
            }
            prev_t_us_ = s.t_us;
            have_prev_t_ = true;
        }
        // Keep at most max * 0.1 s (a longer backlog cannot be expressed; the oldest part is dropped).
        double total = 0;
        for (double v : d) total += v;
        size_t first = 0;
        while (total > double(max) * 0.1 && first + 1 < samples_.size()) total -= d[first++];
        const double width = std::clamp(total / double(max), 0.004, 0.1);
        size_t n = 0;
        double acc_dt = 0, angle[3] = {0, 0, 0}, accel_sum[3] = {0, 0, 0};
        const auto flush = [&] {
            if (!(acc_dt > 0)) return;
            MeasuredSubStep s;
            s.dt = static_cast<float>(acc_dt);
            for (int k = 0; k < 3; ++k) {
                s.rate[k] = static_cast<float>(std::clamp(angle[k] / acc_dt, -double(kVirtualMotionMaxRateRadS),
                                                          double(kVirtualMotionMaxRateRadS)));
                s.accel[k] = static_cast<float>(accel_sum[k] / acc_dt);
            }
            if (n < max) out[n++] = s;
            else { // merge into the last sub-step (angle-preserving)
                MeasuredSubStep& l = out[max - 1];
                const double t = double(l.dt) + acc_dt;
                for (int k = 0; k < 3; ++k) {
                    l.rate[k] = static_cast<float>((double(l.rate[k]) * l.dt + double(s.rate[k]) * acc_dt) / t);
                    l.accel[k] = static_cast<float>((double(l.accel[k]) * l.dt + double(s.accel[k]) * acc_dt) / t);
                }
                l.dt = static_cast<float>(std::min(t, 0.1));
            }
            acc_dt = 0;
            angle[0] = angle[1] = angle[2] = accel_sum[0] = accel_sum[1] = accel_sum[2] = 0;
        };
        for (size_t k = first; k < samples_.size(); ++k) {
            const Sample& s = samples_[k];
            if (!(d[k] > 0)) continue;
            observe_still(s.gyro, s.accel, d[k]);
            float g[3] = {s.gyro[0], s.gyro[1], s.gyro[2]};
            if (auto_calibrate && bias_valid_)
                for (int a = 0; a < 3; ++a) g[a] -= bias_[a];
            float rate[3], acc[3];
            controller_to_remote(g, rate);
            controller_accel_to_kpad(s.accel, acc);
            if (grip_tilt != 0.0) {
                const double c = std::cos(grip_tilt), sn = std::sin(grip_tilt);
                for (float* v : {rate, acc}) {
                    const double y = v[1], z = v[2];
                    v[1] = static_cast<float>(c * y + sn * z);
                    v[2] = static_cast<float>(-sn * y + c * z);
                }
            }
            for (int a = 0; a < 3; ++a) {
                angle[a] += double(rate[a]) * d[k];
                accel_sum[a] += double(acc[a]) * d[k];
            }
            acc_dt += d[k];
            if (acc_dt >= width - 1e-9) flush();
        }
        flush();
        samples_.clear();
        samples_delivered += n;
        return n;
    }

    bool has_data() const { return have_gyro_ && have_accel_; }
    const float* bias() const { return bias_; }
    bool bias_valid() const { return bias_valid_; }
    double nominal_period() const { return nominal_period_; }
    // INPUT13 recentre: the pad's far-edge-up pitch (rad) from the low-passed accelerometer (SDL axes: far
    // edge raised by a -> y = g cos a, z = -g sin a), i.e. the grip_tilt that makes the current grip read as a
    // level remote aimed at the screen. False without a ~1 g reading or for a pad held beyond +-75 deg.
    bool grip_from_gravity(double& pitch) const {
        if (!have_accel_) return false;
        const double y = accel_lp_[1], z = accel_lp_[2], x = accel_lp_[0];
        const double n = std::sqrt(x * x + y * y + z * z);
        if (!(std::fabs(n - kStandardGravity) < 0.25 * kStandardGravity)) return false;
        const double a = std::atan2(-z, y);
        if (!std::isfinite(a) || std::fabs(a) > 75.0 * 3.14159265358979323846 / 180.0) return false;
        pitch = a;
        return true;
    }
    uint64_t samples_received = 0, samples_delivered = 0;
    uint32_t bias_updates = 0;

private:
    struct Sample {
        uint64_t t_us = 0;
        bool timed = false, has_gyro = false, has_accel = false;
        float gyro[3] = {0, 0, 0};
        float accel[3] = {0, 0, 0};
    };
    void start_pending(uint64_t t_us, bool timed) {
        pending_ = Sample{};
        pending_.t_us = t_us;
        pending_.timed = timed;
        has_pending_ = true;
    }
    void finalize_pending() {
        if (!has_pending_) return;
        has_pending_ = false;
        Sample s = pending_;
        if (s.has_gyro) { std::copy(s.gyro, s.gyro + 3, last_gyro_); have_gyro_ = true; }
        else std::copy(last_gyro_, last_gyro_ + 3, s.gyro);
        if (s.has_accel) {
            // INPUT13: low-passed copy for grip_from_gravity (about 20 ms at 1000 Hz, 80 ms at 250 Hz).
            for (int k = 0; k < 3; ++k)
                accel_lp_[k] = have_accel_ ? accel_lp_[k] + 0.05f * (s.accel[k] - accel_lp_[k]) : s.accel[k];
            std::copy(s.accel, s.accel + 3, last_accel_);
            have_accel_ = true;
        }
        else std::copy(last_accel_, last_accel_ + 3, s.accel);
        if (samples_.size() < 4096) samples_.push_back(s);
        ++samples_received;
    }
    void window_reset() {
        win_time_ = 0;
        win_sum_[0] = win_sum_[1] = win_sum_[2] = 0;
    }
    // Still-controller gyro bias: a >= 1 s window where the gyro stays within 0.06 rad/s of the window
    // mean, the accelerometer reads 1 g (+-0.6 m/s^2) and does not move by more than 0.5 m/s^2. Window
    // means below 0.05 rad/s per axis (2.9 deg/s; SDL already applies the factory calibration) update the
    // bias (first one directly, then halfway). A deliberate rotation changes the accelerometer (pitch/roll)
    // or leaves the window; the original KMPLS zero-drift compensation works the same way.
    void observe_still(const float g[3], const float a[3], double d) {
        if (!auto_calibrate || !allow_bias_learning) { window_reset(); return; }
        if (!(d > 0)) return;
        const double an = std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2]);
        bool ok = std::fabs(an - kStandardGravity) < 0.6;
        if (ok && win_time_ > 0) {
            for (int k = 0; k < 3; ++k) {
                ok = ok && std::fabs(g[k] - win_sum_[k] / win_time_) < 0.06 &&
                     std::fabs(a[k] - win_accel0_[k]) < 0.5;
            }
        }
        if (!ok) {
            window_reset();
            if (std::fabs(an - kStandardGravity) >= 0.6) return;
        }
        if (win_time_ == 0) std::copy(a, a + 3, win_accel0_);
        for (int k = 0; k < 3; ++k) win_sum_[k] += double(g[k]) * d;
        win_time_ += d;
        if (win_time_ >= 1.0) {
            double mean[3];
            bool small = true;
            for (int k = 0; k < 3; ++k) {
                mean[k] = win_sum_[k] / win_time_;
                small = small && std::fabs(mean[k]) < 0.05;
            }
            if (small) {
                for (int k = 0; k < 3; ++k)
                    bias_[k] = bias_valid_ ? static_cast<float>(bias_[k] + 0.5 * (mean[k] - bias_[k]))
                                           : static_cast<float>(mean[k]);
                bias_valid_ = true;
                ++bias_updates;
            }
            window_reset();
        }
    }

    std::vector<Sample> samples_;
    std::vector<double> durations_;
    Sample pending_{};
    bool has_pending_ = false;
    float last_gyro_[3] = {0, 0, 0};
    float last_accel_[3] = {0, float(kStandardGravity), 0};
    float accel_lp_[3] = {0, float(kStandardGravity), 0};
    bool have_gyro_ = false, have_accel_ = false;
    uint64_t prev_t_us_ = 0;
    bool have_prev_t_ = false;
    double nominal_period_ = 1.0 / 250.0;
    float bias_[3] = {0, 0, 0};
    bool bias_valid_ = false;
    double win_time_ = 0, win_sum_[3] = {0, 0, 0};
    float win_accel0_[3] = {0, 0, 0};
};

} // namespace nwii::runtime::input
