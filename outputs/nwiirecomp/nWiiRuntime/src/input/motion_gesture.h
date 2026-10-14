#pragma once
// Synthetic Wii Remote + Nunchuk motion gestures (M4.2): sword swings in 8 directions, thrust,
// Nunchuk flick (shield), spin attack and "raise". Producers (keyboard/mouse, gamepad, the
// explicit test replay) only REQUEST a gesture; the InputManager steps the controller once per
// update and turns it into ordinary sensor data of the virtual remote (virtual_motion.h):
// - gyro: a world-frame angular-velocity profile, converted to body rates (x = pitch, nose down
//   positive; y = yaw, turn left positive; z = roll) and integrated into the shared orientation, so
//   the original KMPLS gyro output and the reported gravity stay consistent;
// - accelerometer: KPAD acc = R^T*(0,-1,0) - a/g, where a is the linear acceleration of the sensor
//   mounted kArmRadius in front of the rotation pivot (alpha x p + w x (w x p)) plus explicit
//   linear strokes (thrust), clamped per axis to the sensor range;
// - Nunchuk: an acceleration impulse/shake added to the Nunchuk reading, clamped.
// Nothing here touches guest memory or game state; the game sees only KPAD samples.
//
// Model reference: Dolphin WiimoteEmu (Source/Core/Core/HW/WiimoteEmu/Dynamics.cpp EmulateSwing /
// EmulateShake; InputCommon/ControllerEmu/ControlGroup/Force.cpp defaults: swing angle 90 deg,
// distance 50 cm, speed 16 m/s, return speed 2 m/s; shake 6 Hz, 10 cm). Dolphin rotates and moves the
// remote toward a swing target with a fast out-stroke and a slow return, and derives accelerometer and
// MotionPlus data from that motion. Here the out-stroke is a raised-sine angular profile (closed-form
// angle integral, exact for any producer dt), followed by a short hold and a speed-limited return to
// the pre-gesture pointing direction ("home"); the return stays below 1 rev/s so it cannot read as a
// swing (SS dPad::ex_c::acc_c flags need |mpls_rot| > 2 rev/s, d_pad.cpp fn_80058540).
// Header-only on purpose: no build-list change is needed.
#include "virtual_motion.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace nwii::runtime::input {

inline constexpr double kGravityMs2 = 9.80665;
inline constexpr float kRemoteAccelRangeG = 3.6f;  // ADXL330-class remote accelerometer, per axis
inline constexpr float kNunchukAccelRangeG = 3.0f; // conservative Nunchuk range, per axis

struct GestureTuning {
    double swing_angle = 1.6;      // rad (~92 deg, Dolphin default swing angle 90 deg)
    double swing_time = 0.15;      // s; peak rate 2*angle/time = 21.3 rad/s = 3.4 rev/s
    double hold_time = 0.06;       // s at the end of the stroke
    double return_max_rate = 5.5;  // rad/s (0.88 rev/s)
    double return_gain = 7.0;      // 1/s proportional return
    double return_min_rate = 0.6;  // rad/s floor near home
    double return_timeout = 1.5;   // s
    double arm_radius = 0.35;      // m, pivot (elbow) to accelerometer
    double thrust_time = 0.20;     // s, one accelerate/decelerate cycle along the pointing axis
    double thrust_peak_g = 4.0;
    double flick_time = 0.16;      // Nunchuk flick (shield): one cycle
    double flick_peak_g = 2.6;
    double shake_time = 0.30;      // Nunchuk shake (spin attack): shake_cycles cycles
    double shake_cycles = 2.0;
    double shake_peak_g = 3.0;
    double raise_rate = 6.0;       // rad/s max while turning toward "up"
};

struct GestureStepOutput {
    bool remote_active = false;  // the gesture owns the remote rates this step
    float rate[3] = {0, 0, 0};   // body rad/s
    float accel[3] = {0, 0, 0};  // KPAD-frame linear part (g), added to gravity
    bool nunchuk_active = false;
    float nunchuk_accel[3] = {0, 0, 0}; // KPAD-frame delta (g)
};

class VirtualGestureController {
public:
    enum class Kind { Stroke, Hold, Return, Thrust, Raise };

    GestureTuning tuning{};
    uint32_t requested = 0; // gestures accepted (for logs and tests)

    // Sword swing toward screen direction (dx right, dy up); magnitude ignored.
    void swing(double dx, double dy) {
        const double n = std::sqrt(dx * dx + dy * dy);
        if (!(n > 1e-6) || !std::isfinite(n)) return;
        Segment s{Kind::Stroke};
        s.dx = dx / n; s.dy = dy / n; s.angle = tuning.swing_angle; s.duration = tuning.swing_time;
        queue_strokes({s});
    }
    void thrust() {
        Segment s{Kind::Thrust};
        s.duration = tuning.thrust_time; s.peak = tuning.thrust_peak_g;
        prepare_new_motion();
        remote_.push_back(s);
        push_hold_return();
        ++requested;
    }
    // Nunchuk flick (raise shield / shield bash): forward acceleration impulse.
    void nunchuk_flick() {
        Impulse i;
        i.dir[2] = 1.0; i.peak = tuning.flick_peak_g; i.duration = tuning.flick_time; i.cycles = 1.0;
        nunchuk_.push_back(i);
        ++requested;
    }
    // Spin attack: both controllers shaken together - two remote strokes (out and back) and a
    // Nunchuk shake along the same axis (horizontal: x; vertical: y), with a forward component so
    // the original non-MPLS shake detector (acc_c fn_80058450) also sees its z condition.
    void spin(bool vertical = false) {
        Segment a{Kind::Stroke}, b{Kind::Stroke};
        a.dx = vertical ? 0 : 1; a.dy = vertical ? -1 : 0;
        b.dx = -a.dx; b.dy = -a.dy;
        a.angle = b.angle = tuning.swing_angle;
        a.duration = b.duration = tuning.swing_time;
        queue_strokes({a, b});
        Impulse i;
        i.dir[vertical ? 1 : 0] = 1.0; i.dir[2] = 0.6;
        i.peak = tuning.shake_peak_g; i.duration = tuning.shake_time; i.cycles = tuning.shake_cycles;
        nunchuk_.push_back(i);
    }
    // INPUT10: the Nunchuk half of a spin attack alone (same impulse as spin()), for gyro mode where the
    // real controller provides the remote's out-and-back swing.
    void nunchuk_shake(bool vertical = false) {
        Impulse i;
        i.dir[vertical ? 1 : 0] = 1.0; i.dir[2] = 0.6;
        i.peak = tuning.shake_peak_g; i.duration = tuning.shake_time; i.cycles = tuning.shake_cycles;
        nunchuk_.push_back(i);
        ++requested;
    }
    // Held "raise": turn the pointing axis toward world up (Skyward Strike pose) until released.
    void set_raised(bool raised) {
        if (raised) {
            for (auto& s : remote_) if (s.kind == Kind::Raise && !s.released) return;
            prepare_new_motion();
            remote_.push_back(Segment{Kind::Raise});
            ++requested;
        } else {
            for (auto& s : remote_) if (s.kind == Kind::Raise) s.released = true;
        }
    }
    // Point the remote forward again (world +Z, level), slowly, like a player re-aiming at the screen.
    void recenter() {
        prepare_new_motion();
        home_pending_ = false;
        home_[0] = 0; home_[1] = 0; home_[2] = 1;
        home_q_[0] = 1; home_q_[1] = home_q_[2] = home_q_[3] = 0; // SOAK14: level, no roll
        remote_.push_back(Segment{Kind::Return});
        ++requested;
    }
    bool remote_active() const { return !remote_.empty(); }
    bool nunchuk_active() const { return !nunchuk_.empty(); }
    bool active() const { return remote_active() || nunchuk_active(); }
    void reset() { remote_.clear(); nunchuk_.clear(); home_pending_ = false; }

    // Advance by dt seconds (producer interval). `o` is the orientation before this step.
    void step(double dt, const VirtualRemoteOrientation& o, GestureStepOutput& out) {
        out = GestureStepOutput{};
        if (!(dt > 0.0) || dt > 0.1 || !std::isfinite(dt)) return;
        double m[3][3];
        o.matrix(m);
        const double z_w[3] = {m[0][2], m[1][2], m[2][2]};
        double rot_w[3] = {0, 0, 0}, alpha_w[3] = {0, 0, 0}, lin_w[3] = {0, 0, 0};
        out.remote_active = !remote_.empty();
        double left = dt;
        while (left > 1e-12 && !remote_.empty()) {
            Segment& s = remote_.front();
            if (!s.started) start(s, m);
            double used = left;
            bool done = false;
            switch (s.kind) {
            case Kind::Stroke: {
                const double t0 = s.t, t1 = std::min(s.duration, s.t + left);
                used = t1 - t0;
                const double d = s.angle * (profile(t1, s.duration) - profile(t0, s.duration));
                const double tm = 0.5 * (t0 + t1), T = s.duration;
                const double a = s.angle * (2.0 / T) * (kPi / T) * std::sin(2.0 * kPi * tm / T);
                for (int k = 0; k < 3; ++k) {
                    rot_w[k] += s.axis[k] * d;
                    alpha_w[k] += s.axis[k] * a * (used / dt);
                }
                done = t1 >= s.duration - 1e-12;
                break;
            }
            case Kind::Hold:
                used = std::min(left, s.duration - s.t);
                done = s.t + used >= s.duration - 1e-12;
                break;
            case Kind::Thrust: {
                const double t0 = s.t, t1 = std::min(s.duration, s.t + left);
                used = t1 - t0;
                const double tm = 0.5 * (t0 + t1);
                const double a = s.peak * kGravityMs2 * std::sin(2.0 * kPi * tm / s.duration);
                for (int k = 0; k < 3; ++k) lin_w[k] += z_w[k] * a * (used / dt);
                done = t1 >= s.duration - 1e-12;
                break;
            }
            case Kind::Return:
            case Kind::Raise: {
                const bool raise = s.kind == Kind::Raise && !s.released;
                if (s.kind == Kind::Raise && s.released) { s.kind = Kind::Return; s.t = 0; }
                const double up[3] = {0, 1, 0};
                const double* target = raise ? up : home_;
                double axis[3], err = 0;
                // SOAK14: the return restores the whole latched home orientation, roll included (returning only the
                // pointing direction let roll accumulate over gestures - stroke and shortest return do not commute -
                // until the remote was upside down and up/down swings came out reversed in the game).
                if (!raise && roll_fix()) home_turn(o, axis, err);
                else turn(z_w, target, axis, err);
                const double max_rate = raise ? tuning.raise_rate : tuning.return_max_rate;
                // Proportional approach with a floor, so the return ends in finite time.
                const double speed = std::min(max_rate, std::max(tuning.return_min_rate, tuning.return_gain * err));
                const double d = std::min(err, speed * left);
                for (int k = 0; k < 3; ++k) rot_w[k] += axis[k] * d;
                used = left;
                if (!raise) done = err - d < 1e-3 || s.t + used >= tuning.return_timeout;
                break;
            }
            }
            s.t += used;
            left -= used;
            if (done) {
                const bool was_return = s.kind == Kind::Return;
                remote_.pop_front();
                // A Return ends the step: the next segment (if any) starts next update.
                if (was_return) break;
            }
        }
        if (out.remote_active) {
            // Body rates: R^T * (rotation vector / dt); angular acceleration likewise.
            double w_b[3], al_b[3], lin_b[3];
            for (int j = 0; j < 3; ++j) {
                w_b[j] = (m[0][j] * rot_w[0] + m[1][j] * rot_w[1] + m[2][j] * rot_w[2]) / dt;
                al_b[j] = m[0][j] * alpha_w[0] + m[1][j] * alpha_w[1] + m[2][j] * alpha_w[2];
                lin_b[j] = m[0][j] * lin_w[0] + m[1][j] * lin_w[1] + m[2][j] * lin_w[2];
            }
            // Sensor at p = (0, 0, r) in the body frame (in front of the pivot).
            const double r = tuning.arm_radius;
            const double p[3] = {0, 0, r};
            double a_b[3];
            const double axp[3] = {al_b[1] * p[2] - al_b[2] * p[1], al_b[2] * p[0] - al_b[0] * p[2],
                                   al_b[0] * p[1] - al_b[1] * p[0]};
            const double wxp[3] = {w_b[1] * p[2] - w_b[2] * p[1], w_b[2] * p[0] - w_b[0] * p[2],
                                   w_b[0] * p[1] - w_b[1] * p[0]};
            const double wxwxp[3] = {w_b[1] * wxp[2] - w_b[2] * wxp[1], w_b[2] * wxp[0] - w_b[0] * wxp[2],
                                     w_b[0] * wxp[1] - w_b[1] * wxp[0]};
            for (int k = 0; k < 3; ++k) {
                a_b[k] = axp[k] + wxwxp[k] + lin_b[k];
                out.rate[k] = static_cast<float>(std::clamp(w_b[k], -double(kVirtualMotionMaxRateRadS),
                                                            double(kVirtualMotionMaxRateRadS)));
                out.accel[k] = static_cast<float>(-a_b[k] / kGravityMs2);
            }
        }
        // Nunchuk impulses run in sequence, independently of (in parallel with) the remote.
        out.nunchuk_active = !nunchuk_.empty();
        left = dt;
        while (left > 1e-12 && !nunchuk_.empty()) {
            Impulse& i = nunchuk_.front();
            const double t0 = i.t, t1 = std::min(i.duration, i.t + left);
            const double used = t1 - t0;
            const double tm = 0.5 * (t0 + t1);
            const double a = i.peak * std::sin(2.0 * kPi * i.cycles * tm / i.duration);
            for (int k = 0; k < 3; ++k) out.nunchuk_accel[k] += static_cast<float>(-i.dir[k] * a * (used / dt));
            i.t = t1;
            left -= used;
            if (t1 >= i.duration - 1e-12) nunchuk_.pop_front();
        }
    }

    // Clamp a KPAD acceleration vector per axis to the sensor range.
    static void clamp_accel(float v[3], float range) {
        for (int k = 0; k < 3; ++k) v[k] = std::isfinite(v[k]) ? std::clamp(v[k], -range, range) : 0.0f;
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    struct Segment {
        Kind kind = Kind::Hold;
        double dx = 0, dy = 0;      // Stroke: screen direction (x right, y up)
        double axis[3] = {0, 0, 0}; // Stroke: world rotation axis, resolved at start
        double angle = 0, duration = 0, peak = 0;
        double t = 0;
        bool started = false, released = false;
    };
    struct Impulse {
        double dir[3] = {0, 0, 0}; // Nunchuk frame (KPAD axes)
        double peak = 0, duration = 0.1, cycles = 1, t = 0;
    };

    // Normalized cumulative raised-sine profile: integral of (2/T) sin^2(pi t / T) from 0 to t.
    static double profile(double t, double T) {
        t = std::clamp(t, 0.0, T);
        return (t - T / (2.0 * kPi) * std::sin(2.0 * kPi * t / T)) / T;
    }
    // Shortest rotation (world axis, angle) turning pointing direction `from` onto `to`.
    static void turn(const double from[3], const double to[3], double axis[3], double& angle) {
        const double c[3] = {from[1] * to[2] - from[2] * to[1], from[2] * to[0] - from[0] * to[2],
                             from[0] * to[1] - from[1] * to[0]};
        const double s = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        const double d = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
        angle = std::atan2(s, d);
        if (s > 1e-12) { for (int k = 0; k < 3; ++k) axis[k] = c[k] / s; }
        else { axis[0] = axis[1] = axis[2] = 0; angle = 0; }
    }
    void start(Segment& s, const double m[3][3]) {
        s.started = true;
        if (s.kind != Kind::Stroke) return;
        // World axis: tip moving right = turn right (about -Up); tip moving up = nose up (about
        // -Left_h), where Left_h is the horizontal part of the remote's X (left) axis, so a vertical
        // swing stays vertical for a pitched (raised) remote. Fallback: Up x heading.
        double lx = m[0][0], lz = m[2][0];
        // SOAK14: "left" from the heading (world up x pointing direction), so "up" is screen up whatever the remote's
        // roll; the remote's own X axis (flipped when it is upside down) only for a remote pointing (almost) vertically.
        if (roll_fix() && std::sqrt(m[2][2] * m[2][2] + m[0][2] * m[0][2]) > 0.1) { lx = m[2][2]; lz = -m[0][2]; }
        double n = std::sqrt(lx * lx + lz * lz);
        if (n < 1e-3) { lx = m[2][2]; lz = -m[0][2]; n = std::sqrt(lx * lx + lz * lz); }
        if (n < 1e-9) { lx = 1; lz = 0; n = 1; }
        lx /= n; lz /= n;
        s.axis[0] = -s.dy * lx;
        s.axis[1] = -s.dx;
        s.axis[2] = -s.dy * lz;
        const double a = std::sqrt(s.axis[0] * s.axis[0] + s.axis[1] * s.axis[1] + s.axis[2] * s.axis[2]);
        for (double& v : s.axis) v /= a;
    }
    // Keep strokes/thrusts at the front (in progress or queued); drop everything from the first
    // hold/return/raise on. Starting from idle latches a new "home" (see latch_home).
    void prepare_new_motion() {
        if (remote_.empty()) { home_pending_ = true; return; }
        std::deque<Segment> keep;
        for (auto& s : remote_) {
            if (s.kind != Kind::Stroke && s.kind != Kind::Thrust) break;
            keep.push_back(s);
        }
        remote_.swap(keep);
    }
    void queue_strokes(std::initializer_list<Segment> strokes) {
        // At most two strokes may wait behind the one in progress (key repeat / mashing).
        size_t pending = 0;
        for (auto& s : remote_) pending += (s.kind == Kind::Stroke && !s.started);
        if (pending >= 2) return;
        prepare_new_motion();
        for (const auto& s : strokes) remote_.push_back(s);
        push_hold_return();
        ++requested;
    }
    void push_hold_return() {
        Segment h{Kind::Hold};
        h.duration = tuning.hold_time;
        remote_.push_back(h);
        remote_.push_back(Segment{Kind::Return});
    }

    std::deque<Segment> remote_;
    std::deque<Impulse> nunchuk_;
    bool home_pending_ = false;
    double home_[3] = {0, 0, 1};
    double home_q_[4] = {1, 0, 0, 0}; // SOAK14: whole home orientation (w, x, y, z), remote -> world

    // SOAK14 A/B switch: NWII_GESTURE_ROLL_FIX=0 = the pre-SOAK14 pointing-only return and remote-X stroke axis.
    static bool roll_fix() {
        static const bool on = [] {
            const char* e = std::getenv("NWII_GESTURE_ROLL_FIX");
            return !(e && e[0] == '0');
        }();
        return on;
    }
    // World rotation (axis, angle) from the current orientation to the latched home orientation: q_home * conj(q).
    void home_turn(const VirtualRemoteOrientation& o, double axis[3], double& angle) const {
        const double aw = home_q_[0], ax = home_q_[1], ay = home_q_[2], az = home_q_[3];
        const double bw = o.w, bx = -o.x, by = -o.y, bz = -o.z;
        double w = aw * bw - ax * bx - ay * by - az * bz;
        double x = aw * bx + ax * bw + ay * bz - az * by;
        double y = aw * by - ax * bz + ay * bw + az * bx;
        double z = aw * bz + ax * by - ay * bx + az * bw;
        if (w < 0) { w = -w; x = -x; y = -y; z = -z; } // shortest way
        const double s = std::sqrt(x * x + y * y + z * z);
        angle = 2.0 * std::atan2(s, w);
        if (s > 1e-12) { axis[0] = x / s; axis[1] = y / s; axis[2] = z / s; }
        else { axis[0] = axis[1] = axis[2] = 0; angle = 0; }
    }

public:
    // Called by the InputManager before step(): latches the pointing direction as "home" when a
    // gesture started from idle (the orientation is producer-owned, so it is read there).
    void latch_home(const VirtualRemoteOrientation& o) {
        if (!home_pending_) return;
        home_pending_ = false;
        double m[3][3];
        o.matrix(m);
        home_[0] = m[0][2]; home_[1] = m[1][2]; home_[2] = m[2][2];
        home_q_[0] = o.w; home_q_[1] = o.x; home_q_[2] = o.y; home_q_[3] = o.z;
    }
};

// Stick flick -> swing (gamepad right stick): leaving the neutral zone (< 0.3) and reaching the rim
// (> 0.85) within 120 ms fires one swing in the stick direction; the stick must come back to neutral
// before the next flick, and aim from that stick is suppressed until then. Slower deflections are
// ordinary aim. (Skyward Sword HD's button-only scheme uses right-stick flicks for the sword.)
struct StickFlick {
    double t = 0, leave_t = -1;
    bool armed = true, suppress = false;
    bool update(float x, float y, double dt, float& dx, float& dy) {
        if (dt > 0 && dt < 1) t += dt;
        const float mag = std::sqrt(x * x + y * y);
        if (mag < 0.3f) { armed = true; leave_t = -1; suppress = false; return false; }
        if (armed && leave_t < 0) leave_t = t;
        if (armed && mag > 0.85f) {
            armed = false;
            if (t - leave_t <= 0.12) { dx = x; dy = y; suppress = true; return true; }
        }
        return false;
    }
};

// Producer-owned per channel, like g_virtual_orientation: only the InputManager update thread
// (update_locked and the sources it polls) may touch it.
inline VirtualGestureController g_virtual_gesture[4];

}
