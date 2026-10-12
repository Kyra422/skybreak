#pragma once
// Virtual MotionPlus producer state shared by the InputManager (producer, host
// input thread) and the SOUE01 KPAD/KMPLS adapter (consumer, guest CPU thread).
// Header-only on purpose: no build-list change is needed.
//
// Contract (verified against the ORIGINAL generated KMPLS code in
// outputs/pc-tests/pointer-mpls):
// - Rates use the MotionSample guest axes (x = pitch, y = yaw, z = roll) in
//   rad/s, in the remote (body) frame. Remote axes: X = left, Y = up (button
//   face), Z = forward (pointing end). +x = nose down, +y = turn left.
// - Orientation R maps remote axes to world axes (its columns are what KMPLS
//   publishes as KPADStatus mpls basis X/Y/Z at +C8/+D4/+E0). Identity = level,
//   face up, pointing at the screen. KMPLS integrates in the body frame.
// - The KPAD acceleration KMPLS uses for its accelerometer revision is
//   R^T * (0, -1, 0); (0, -1, 0) at rest keeps the original basis at identity.
// This is a virtual sensor model for mouse/stick/replay input, not measured
// hardware data.
#include "input/input_source.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace nwii::runtime::input {

// One InputManager update of one channel's virtual MotionPlus.
struct MotionStep {
    uint64_t sequence = 0;     // InputManager update sequence (MotionSample::sequence)
    uint64_t monotonic_ns = 0; // MotionSample::monotonic_ns
    float dt_seconds = 0.0f;   // producer interval covered by this step, (0, 0.1]
    float rate[3] = {0.0f, 0.0f, 0.0f};
    // KPAD accelerometer readings at the end of this step (g): remote (gravity of the integrated
    // orientation plus gesture linear acceleration) and Nunchuk. Used to expand one guest read into
    // several 200 Hz KPAD samples (resample_accel_bins).
    float accel[3] = {0.0f, -1.0f, 0.0f};
    float nunchuk_accel[3] = {0.0f, 0.0f, 1.0f};
};

struct VirtualRemoteOrientation {
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0; // unit quaternion, remote -> world

    void reset() { w = 1.0; x = y = z = 0.0; }

    // q <- q * exp(omega * dt / 2): body-frame rates, like the original KMPLS.
    void integrate(const float rate[3], double dt) {
        const double ox = rate[0] * dt, oy = rate[1] * dt, oz = rate[2] * dt;
        const double angle = std::sqrt(ox * ox + oy * oy + oz * oz);
        if (!(angle > 0.0) || !std::isfinite(angle)) return;
        const double s = std::sin(angle * 0.5) / angle, c = std::cos(angle * 0.5);
        const double bx = ox * s, by = oy * s, bz = oz * s;
        const double nw = w * c - x * bx - y * by - z * bz;
        const double nx = w * bx + x * c + y * bz - z * by;
        const double ny = w * by - x * bz + y * c + z * bx;
        const double nz = w * bz + x * by - y * bx + z * c;
        const double n = std::sqrt(nw * nw + nx * nx + ny * ny + nz * nz);
        w = nw / n; x = nx / n; y = ny / n; z = nz / n;
    }

    // m[row][col]; column j is remote axis j (X, Y, Z) in world coordinates.
    void matrix(double m[3][3]) const {
        m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - w * z); m[0][2] = 2 * (x * z + w * y);
        m[1][0] = 2 * (x * y + w * z); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - w * x);
        m[2][0] = 2 * (x * z - w * y); m[2][1] = 2 * (y * z + w * x); m[2][2] = 1 - 2 * (x * x + y * y);
    }

    // KPAD acceleration of a still remote in this orientation: R^T * (0,-1,0).
    void gravity(float out[3]) const {
        double m[3][3];
        matrix(m);
        for (int j = 0; j < 3; ++j) out[j] = static_cast<float>(-m[1][j]);
    }

    // Remote-frame angular velocity (rad/s) that turns the pointing axis (Z)
    // onto the world direction `target` within `dt`, by the shortest rotation.
    // Clamped per axis.
    // SOAK14: out[2] = a roll rate that levels the remote (its X axis back onto world up x pointing direction),
    // gain 6/s, at most 2 rad/s; callers that aim with these rates apply it unless an explicit roll input is held.
    // Shortest rotations of the pointing axis do not keep roll at zero over closed aim paths (holonomy: 48 menu-sized
    // aims gave 23 deg of roll), and recentring did not level the remote. Dolphin's emulated pointer is roll-free (roll
    // only from the Tilt input). A/B: NWII_AIM_LEVEL_ROLL=0 = out[2] stays 0 (pre-SOAK14).
    void rates_toward(const double target[3], double dt, float max_rate, float out[3]) const {
        out[0] = out[1] = out[2] = 0.0f;
        double m[3][3];
        matrix(m);
        double t[3] = {target[0], target[1], target[2]};
        const double tn = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (!(dt > 0.0) || !(tn > 0.0) || !std::isfinite(tn)) return;
        out[2] = level_roll_rate(m, dt, max_rate);
        // Target in remote coordinates: R^T * t.
        double b[3];
        for (int j = 0; j < 3; ++j) b[j] = (m[0][j] * t[0] + m[1][j] * t[1] + m[2][j] * t[2]) / tn;
        // Rotation axis = Z x b, angle = acos(Z . b), Z = (0,0,1) in the body frame.
        const double ax = -b[1], ay = b[0];
        const double sin_a = std::sqrt(ax * ax + ay * ay);
        const double angle = std::atan2(sin_a, b[2]);
        if (!(sin_a > 1e-12)) return;
        const double scale = angle / (sin_a * dt);
        const double rx = ax * scale, ry = ay * scale;
        const auto clamp = [max_rate](double v) {
            return static_cast<float>(v > max_rate ? max_rate : (v < -max_rate ? -max_rate : v));
        };
        out[0] = clamp(rx);
        out[1] = clamp(ry);
    }

    // SOAK14: body roll rate (about Z) that turns the remote's X axis onto L = normalize(world up x Z), i.e. level, face
    // up; 0 for a remote pointing (almost) vertically, where "level" is undefined, or with NWII_AIM_LEVEL_ROLL=0.
    static float level_roll_rate(const double m[3][3], double dt, float max_rate) {
        static const bool on = [] {
            const char* e = std::getenv("NWII_AIM_LEVEL_ROLL");
            return !(e && e[0] == '0');
        }();
        if (!on || !(dt > 0.0)) return 0.0f;
        double lx = m[2][2], lz = -m[0][2];
        const double n = std::sqrt(lx * lx + lz * lz);
        if (!(n > 0.1)) return 0.0f;
        lx /= n; lz /= n;
        // phi = signed angle X -> L about the pointing axis Z (column 2).
        const double cx = m[1][0] * lz, cy = m[2][0] * lx - m[0][0] * lz, cz = -m[1][0] * lx; // X x L
        const double phi = std::atan2(cx * m[0][2] + cy * m[1][2] + cz * m[2][2], m[0][0] * lx + m[2][0] * lz);
        const double cap = std::min(double(max_rate), 2.0);
        double r = std::clamp(phi * 6.0, -cap, cap);
        if (std::fabs(r * dt) > std::fabs(phi)) r = phi / dt;
        return std::isfinite(r) ? static_cast<float>(r) : 0.0f;
    }
};

// Steps of one channel, kept so that a guest that reads KPAD less often than the
// producer updates still receives every producer interval (like the 200 Hz WPAD
// sample ring of real hardware), instead of only the latest snapshot.
class MotionStepHistory {
public:
    // INPUT10: real sensors deliver several steps per update (gamepad_motion.h, >= 4 ms each), so
    // 4096 steps keep ~16 s at 250 steps/s (~68 s of one-step-per-update virtual input at 60 Hz).
    static constexpr size_t kCapacity = 4096;

    void push(const MotionStep& step) {
        std::lock_guard lock(mutex_);
        ring_[pushed_ % kCapacity] = step;
        ++pushed_;
    }
    void clear() {
        std::lock_guard lock(mutex_);
        pushed_ = 0;
    }
    // Copies the steps with after < sequence <= upto, oldest first. Returns the
    // count; *latest_present tells whether the step `upto` itself is recorded;
    // *lost tells whether older steps in the range were already overwritten.
    size_t since(uint64_t after, uint64_t upto, MotionStep* out, size_t max,
                 bool* latest_present, bool* lost) const {
        std::lock_guard lock(mutex_);
        const size_t available = pushed_ < kCapacity ? static_cast<size_t>(pushed_) : kCapacity;
        size_t first = available; // index (0 = oldest retained) of the first step > after
        for (size_t i = available; i-- > 0;) {
            const MotionStep& s = ring_[(pushed_ - available + i) % kCapacity];
            if (s.sequence <= after) break;
            first = i;
        }
        size_t count = 0;
        bool present = false;
        for (size_t i = first; i < available && count < max; ++i) {
            const MotionStep& s = ring_[(pushed_ - available + i) % kCapacity];
            if (s.sequence > upto) break;
            present = present || s.sequence == upto;
            out[count++] = s;
        }
        if (latest_present) *latest_present = present;
        if (lost) *lost = first == 0 && pushed_ > kCapacity && available > 0 &&
                          ring_[(pushed_ - available) % kCapacity].sequence > after + 1;
        return count;
    }

private:
    mutable std::mutex mutex_;
    std::array<MotionStep, kCapacity> ring_{};
    uint64_t pushed_ = 0;
};

inline MotionStepHistory g_virtual_motion_history[4];
// Time-weighted accelerometer averages of the producer steps in (after, upto], resampled into n
// equal bins of about `period` seconds (n = round(total / period), clamped to [1, max_bins]), oldest
// first - the 200 Hz sample stream a real remote delivers between two KPADRead calls. Returns 0 when
// the history does not contain `upto` (or `after` is 0), so callers keep a single sample.
struct AccelBin {
    float accel[3];
    float nunchuk_accel[3];
};
inline size_t resample_accel_bins(const MotionStepHistory& history, uint64_t after, uint64_t upto,
                                  size_t max_bins, AccelBin* out, double period = 0.005) {
    if (after == 0 || upto <= after || max_bins == 0) return 0;
    static thread_local std::vector<MotionStep> steps(MotionStepHistory::kCapacity); // heap, not TLS
    bool latest = false, lost = false;
    const size_t count = history.since(after, upto, steps.data(), steps.size(), &latest, &lost);
    if (!latest || count == 0) return 0;
    double total = 0;
    for (size_t i = 0; i < count; ++i) total += steps[i].dt_seconds;
    if (!(total > 0) || !std::isfinite(total)) return 0;
    const long long wanted = std::llround(total / period);
    const size_t n = static_cast<size_t>(std::clamp<long long>(wanted, 1, static_cast<long long>(max_bins)));
    const double width = total / n;
    size_t step = 0;
    double step_left = steps[0].dt_seconds;
    for (size_t k = 0; k < n; ++k) {
        double a[3] = {0, 0, 0}, na[3] = {0, 0, 0}, need = width;
        while (need > 1e-15 && step < count) {
            const double take = std::min(need, step_left);
            for (int j = 0; j < 3; ++j) {
                a[j] += steps[step].accel[j] * take;
                na[j] += steps[step].nunchuk_accel[j] * take;
            }
            need -= take;
            step_left -= take;
            if (step_left <= 1e-12 && ++step < count) step_left = steps[step].dt_seconds;
        }
        const double covered = width - std::max(need, 0.0);
        for (int j = 0; j < 3; ++j) {
            out[k].accel[j] = static_cast<float>(covered > 0 ? a[j] / covered : steps[count - 1].accel[j]);
            out[k].nunchuk_accel[j] = static_cast<float>(covered > 0 ? na[j] / covered : steps[count - 1].nunchuk_accel[j]);
        }
    }
    return n;
}


// Set by a game adapter (SS: ss_boot.cpp, once MotionPlus+Nunchuk mode 5 is
// configured) when the channel's pointer is driven by MotionPlus orientation.
// Live sources use it to pick the mouse mapping; it changes no guest data.
inline std::atomic<bool> g_guest_motionplus_pointer[4];
// Which live device last aimed: 0 = mouse (absolute mouse aim active), 1 = gamepad
// (mouse position ignored). Mouse movement/buttons claim it; right stick/face buttons claim it back.
inline std::atomic<int> g_aim_owner{0};
// Set by the SS adapter (ss_boot.cpp, read-only) while dPadManager is in ModeProc_PointCenter
// (mode 6, "point the Wii Remote at the screen"). That screen needs a visible sensor bar (valid
// DPD), so live sources then report a centred IR pointer and steer the virtual remote level,
// exactly like the boot replay does.
inline std::atomic<bool> g_guest_point_center{false};
// INPUT13: set by the SS adapter (ss_boot.cpp, read-only) while the game's own menu pointer is shown
// during play (dCsGame_c cursor active with type POINTER_DEF/POINTER_CAT: pause menu, map, shop,
// deposit, ...). The right stick then moves that pointer like in the title menus instead of acting as
// sword flicks (a quick push used to swing and freeze the pointer while held).
inline std::atomic<bool> g_guest_pointer_cursor{false};
// Producer-owned orientation per channel. Only the InputManager update thread
// (update_locked and the sources it polls) may touch it.
inline VirtualRemoteOrientation g_virtual_orientation[4];

// Half range of SS's MotionPlus pointer: dPad::ex_c::field_0x70 default 0x1200
// (d_pad.cpp setField_0x70), i.e. pointer pos 1.0 = 0x1200/0x10000 turn.
inline constexpr double kPointerHalfRangeRad = 0x1200 * (2.0 * 3.14159265358979323846 / 65536.0);

// World pointing direction that SS's pointer maps to pos (x right, y down,
// -1..1), assuming the game's centre reference is the identity orientation.
// Inverse of ex_c::fn_80056790 with the reference frame of centerCursor:
// pos.x = atan2(-v.x, v.z) / range, pos.y = atan2(-v.y, v.z) / range.
inline void pointer_direction(double pos_x, double pos_y, double out[3]) {
    const double tx = std::tan(-pos_x * kPointerHalfRangeRad); // v.x / v.z
    const double ty = std::tan(pos_y * kPointerHalfRangeRad);  // -v.y / v.z
    const double n = std::sqrt(1.0 + tx * tx + ty * ty);
    out[0] = tx / n;  // X = left: pointer right means v.x < 0
    out[1] = -ty / n; // Y = up: pointer down means v.y < 0
    out[2] = 1.0 / n;
}

// SOAK14: the game's own pointer, read back by the SS adapter at every channel-0 KPAD read (ss_boot.cpp, read-only:
// dPad::ex_c::m_current_ex pos, x right / y down, -1..1, the game clamps it) and the number of read-backs.
inline std::atomic<float> g_guest_pointer_x{0.0f};
inline std::atomic<float> g_guest_pointer_y{0.0f};
inline std::atomic<uint32_t> g_guest_pointer_reads{0};

// SOAK14: closed-loop absolute aiming. SS revises its MotionPlus estimate on its own (d_pad.cpp: KPAD MplsDirRevise
// toward a fixed base "30 deg down, straight ahead" while Link attacks, AccRevise toward gravity; the DPD revise
// cannot help because the virtual sensor bar is reported invisible), so after sword attacks the game's pointer is no
// longer the pointer of our virtual orientation (soak: 0.2-0.5 screen halves after a few swings, later pinned at the
// edge). Every producer update observe() compares the read-back with the pointer of our orientation while the
// remote rests (>= 120 ms, so the game's read saw the same pose) and learns offset = game - ours; aim() turns a
// screen target into the direction that puts the GAME's pointer there. Without read-backs (unit fixtures, other
// games) the offset stays 0 = the open-loop model. NWII_POINTER_AIM_LOOP=0 switches it off (A/B).
struct PointerAimLoop {
    float off_x = 0.0f, off_y = 0.0f; // learned game - ours (screen halves)
    uint32_t learned = 0;             // read-backs used
    double still_s = 0.0;
    float last_x = 9.0f, last_y = 9.0f;
    uint32_t reads = 0;

    static bool enabled() {
        static const bool on = [] {
            const char* e = std::getenv("NWII_POINTER_AIM_LOOP");
            return !(e && e[0] == '0');
        }();
        return on;
    }
    // Pointer of an orientation under the identity-centre model (inverse of pointer_direction).
    static bool pointer_of(const VirtualRemoteOrientation& o, float& px, float& py) {
        double m[3][3];
        o.matrix(m);
        const double vx = m[0][2], vy = m[1][2], vz = m[2][2];
        if (!(vz > 0.0)) return false;
        px = static_cast<float>(std::atan2(-vx, vz) / kPointerHalfRangeRad);
        py = static_cast<float>(std::atan2(-vy, vz) / kPointerHalfRangeRad);
        return std::isfinite(px) && std::isfinite(py);
    }
    void observe(const VirtualRemoteOrientation& o, double dt, float game_x, float game_y, uint32_t game_reads) {
        float ox, oy;
        if (!pointer_of(o, ox, oy)) { still_s = 0.0; last_x = last_y = 9.0f; reads = game_reads; return; }
        if (std::fabs(ox - last_x) < 0.003f && std::fabs(oy - last_y) < 0.003f) still_s += dt > 0.0 && dt < 0.2 ? dt : 0.0;
        else still_s = 0.0;
        last_x = ox; last_y = oy;
        if (game_reads == reads) return;
        reads = game_reads;
        if (!enabled() || still_s < 0.12 || !std::isfinite(game_x) || !std::isfinite(game_y) ||
            std::fabs(game_x) > 1.5f || std::fabs(game_y) > 1.5f)
            return;
        // A clamped read-back (|pos| = 1) under-estimates the offset; the corrected aim then moves the pointer
        // inward and the next read-back completes it.
        learn_axis(off_x, game_x, ox, learn_ox_, learn_gx_);
        learn_axis(off_y, game_y, oy, learn_oy_, learn_gy_);
        ++learned;
    }
    // REVSOAK14: SS clamps its own pointer (d_pad.cpp ex_c::fn_80056790): x to +-1, y to +-1 or, while dLytMap_c
    // exists but is hidden, to +-(1 - letterbox/height) during Z-targeting and events. Learning a clamped read-back as
    // the offset made a mouse resting beyond the clamp turn the virtual remote on without end (+30 deg in 5 s of
    // letterbox; afterwards the pointer stayed pinned at the edge). So a read-back at +-1 only pushes the offset
    // outward (the true pointer is at or beyond the edge), and one that did not follow our pointer since the last
    // learned sample (pinned at the letterbox, or not following the remote at all) teaches nothing - except near the
    // centre (|pos| < 0.25, no clamp is that tight): SS re-anchors its pointer to exactly 0 (centerCursor; soak logs:
    // after every menu close), and a second re-anchor to the same 0 is a new reference that must be learned.
    // A/B: NWII_POINTER_AIM_PIN=0 = every read-back is learned (SOAK14).
    float learn_ox_ = 9.0f, learn_gx_ = 9.0f, learn_oy_ = 9.0f, learn_gy_ = 9.0f; // last learned sample per axis
    static bool pin_rule() {
        static const bool on = [] {
            const char* e = std::getenv("NWII_POINTER_AIM_PIN");
            return !(e && e[0] == '0');
        }();
        return on;
    }
    static void learn_axis(float& off, float game, float ours, float& ours_l, float& game_l) {
        const float want = game - ours;
        if (pin_rule()) {
            if (std::fabs(game) >= 0.9995f) {
                if (game > 0.0f ? want > off : want < off) off += 0.5f * (want - off);
                return;
            }
            if (std::fabs(game) >= 0.25f && std::fabs(ours - ours_l) > 0.0005f && std::fabs(game - game_l) < 0.002f)
                return;
        }
        off += 0.5f * (want - off);
        ours_l = ours;
        game_l = game;
    }
    // World direction to turn the remote to so that the game's pointer lands on (tx, ty).
    void aim(double tx, double ty, double out[3]) const { pointer_direction(tx - off_x, ty - off_y, out); }
    // The game's pointer the player sees, estimated from our orientation (pointer_of + offset).
    bool game_pointer(const VirtualRemoteOrientation& o, float& px, float& py) const {
        if (!pointer_of(o, px, py)) return false;
        px += off_x; py += off_y;
        return true;
    }
};
// Producer-owned like g_virtual_orientation (InputManager update thread only).
inline PointerAimLoop g_pointer_aim[4];

}
