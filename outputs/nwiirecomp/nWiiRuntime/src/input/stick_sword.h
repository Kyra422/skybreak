#pragma once
// CTRL18-INPUT (design section 3.4, track T4): stick sword recogniser, Skyward Sword HD button-only style.
// Fed with a virtual stick (x right, y up, -1..1) once per InputManager update; emits sword events that the
// translator (hd_translator.h) turns into VirtualGestureController calls (motion_gesture.h), which the ORIGINAL game
// code then reads as Wii Remote swings (verified in outputs/pc-tests/ctrl18). The same state machine serves the
// gamepad right stick and the mouse (MouseSwordStick below: the drag while the sword button is held = the stick).
//  - Flick: leaving the neutral zone (< neutral) and reaching the rim (> rim) within flick_time = one swing in the
//    flick direction, snapped to the 8 HD directions; it fires at the rim edge (fire_on_release: when the stick is back
//    in the neutral zone). A slow push is no swing. Every flick fires at most once (no double fire at a sector
//    boundary); the stick must come back to neutral before the next one.
//  - Straight up waits: released quickly = swing up; held in the up sector at the rim for raise_time = raise the sword
//    (Skyward Strike charge) until the stick leaves the up sector. After that the raise stays up for strike_window: a
//    new flick from the centre in that window is the strike (swing from the raised pose), otherwise it is lowered.
//  - Spin: three flicks alternating along one axis (L-R-L / R-L-R = horizontal, U-D-U / D-U-D = vertical) within
//    spin_window: the third flick becomes a spin attack instead of a swing.
//  - disabled (camera held, aim / wheel / menu contexts): no events; a raise is released; afterwards the stick must
//    return to neutral first (no swing from a stick that was already deflected).
// Time = the summed update dt (deterministic with InputManager::update_at). Header-only: no build-list change.
#include <cmath>
#include <cstdint>

namespace nwii::runtime::input {

struct SwordTuning {
    float neutral = 0.30f;
    float rim = 0.85f;
    double flick_time = 0.12;    // s, neutral -> rim
    double raise_time = 0.25;    // s at the rim in the up sector
    double strike_window = 0.40; // s after a raise is let go
    double spin_window = 0.50;   // s from the first to the third flick
    bool fire_on_release = false;
};

struct SwordEvent {
    enum Kind : uint8_t { None, Swing, RaiseOn, RaiseOff, Spin } kind = None;
    float dx = 0, dy = 0;  // Swing: unit direction (x right, y up)
    bool vertical = false; // Spin
    bool strike = false;   // Swing from a raised sword (Skyward Strike)
};

class SwordRecognizer {
public:
    SwordTuning tuning{};

    // 8 HD directions: 0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left, 7 up-left.
    static int sector8(float x, float y) {
        const double a = std::atan2(double(x), double(y)); // 0 = up, clockwise
        int s = int(std::lround(a / (3.14159265358979323846 / 4.0)));
        return (s % 8 + 8) % 8;
    }
    static void sector_dir(int s, float& dx, float& dy) {
        static const float k = 0.70710678f;
        static const float dirs[8][2] = {{0, 1}, {k, k}, {1, 0}, {k, -k}, {0, -1}, {-k, -k}, {-1, 0}, {-k, k}};
        dx = dirs[s & 7][0];
        dy = dirs[s & 7][1];
    }

    // One update. Returns the number of events written to out (max 3).
    int update(float x, float y, double dt, bool enabled, SwordEvent out[3]) {
        int n = 0;
        if (dt > 0 && dt < 1) t_ += dt;
        const float mag = std::sqrt(x * x + y * y);
        if (!enabled) {
            if (raised_) { out[n++] = event(SwordEvent::RaiseOff); raised_ = false; }
            strike_until_ = -1;
            pending_ = -1;
            up_hold_ = 0;
            wait_neutral_ = mag >= tuning.neutral;
            out_ = false;
            return n;
        }
        if (wait_neutral_) {
            if (mag < tuning.neutral) wait_neutral_ = false;
            else return n;
        }
        // Strike window over: lower the sword.
        if (strike_until_ >= 0 && t_ > strike_until_ && !raised_) {
            strike_until_ = -1;
            out[n++] = event(SwordEvent::RaiseOff);
        }
        if (mag < tuning.neutral) {
            if (out_) {
                out_ = false;
                if (raised_) {
                    // Let go of the raise: keep it up for the strike window.
                    raised_ = false;
                    strike_until_ = t_ + tuning.strike_window;
                } else if (pending_ >= 0) {
                    n += fire(pending_, out + n);
                }
                pending_ = -1;
                up_hold_ = 0;
            }
            return n;
        }
        if (!out_) { out_ = true; leave_t_ = t_; flicked_ = false; pending_ = -1; up_hold_ = 0; }
        const int sec = sector8(x, y);
        if (raised_ && sec != 0) {
            // The stick left the up sector without coming back to the centre (rolled along the rim): the raise is let
            // go (strike window), but only a new flick from the centre is the strike.
            raised_ = false;
            strike_until_ = t_ + tuning.strike_window;
            flicked_ = true;
        }
        if (!flicked_ && mag >= tuning.rim) {
            flicked_ = true;
            const bool quick = t_ - leave_t_ <= tuning.flick_time + 1e-9;
            const bool strike = strike_until_ >= 0;
            if (quick || strike) {
                if (strike) {
                    n += fire(sec, out + n, true);
                    strike_until_ = -1;
                    out[n++] = event(SwordEvent::RaiseOff);
                } else if (sec == 0 || tuning.fire_on_release) {
                    pending_ = sec; // up: swing on release unless it turns into a raise
                } else {
                    n += fire(sec, out + n);
                }
            }
        }
        if (!raised_ && strike_until_ < 0 && sec == 0 && mag >= tuning.rim) {
            up_hold_ += dt > 0 && dt < 1 ? dt : 0;
            if (up_hold_ >= tuning.raise_time - 1e-9) {
                raised_ = true;
                pending_ = -1;
                out[n++] = event(SwordEvent::RaiseOn);
            }
        } else if (sec != 0) {
            up_hold_ = 0;
        }
        return n;
    }
    // The stick is in use by the sword (aim from the same stick is suppressed).
    bool busy() const { return out_ || raised_ || wait_neutral_; }
    bool raised() const { return raised_ || strike_until_ >= 0; }
    void require_neutral() { wait_neutral_ = true; out_ = false; pending_ = -1; }
    double time() const { return t_; }

private:
    SwordEvent event(SwordEvent::Kind k) const { SwordEvent e; e.kind = k; return e; }
    int fire(int sec, SwordEvent* out, bool strike = false) {
        SwordEvent e;
        // Spin: this flick and the two before it alternate along one axis within the window.
        const bool horizontal = sec == 2 || sec == 6, vertical = sec == 0 || sec == 4;
        if (!strike && (horizontal || vertical) && hits_ >= 2) {
            const int a = hist_[(hits_ - 2) % 2], b = hist_[(hits_ - 1) % 2];
            const double ta = hist_t_[(hits_ - 2) % 2];
            if (a == sec && b == (sec + 4) % 8 && t_ - ta <= tuning.spin_window + 1e-9) {
                e.kind = SwordEvent::Spin;
                e.vertical = vertical;
                hits_ = 0; // a fourth flick starts a new sequence
                out[0] = e;
                return 1;
            }
        }
        hist_[hits_ % 2] = sec;
        hist_t_[hits_ % 2] = t_;
        ++hits_;
        e.kind = SwordEvent::Swing;
        e.strike = strike;
        sector_dir(sec, e.dx, e.dy);
        out[0] = e;
        return 1;
    }
    double t_ = 0, leave_t_ = 0, up_hold_ = 0, strike_until_ = -1;
    bool out_ = false, flicked_ = false, raised_ = false, wait_neutral_ = false;
    int pending_ = -1;
    int hist_[2] = {-1, -1};
    double hist_t_[2] = {0, 0};
    uint32_t hits_ = 0;
};

// Mouse sword: while the sword button is held, the mouse drag is a virtual stick (radius_px = full deflection).
// After every fired swing the drag starts again from the centre, so the next drag is the next flick.
struct MouseSwordStick {
    float radius_px = 60.0f;
    float ax = 0, ay = 0; // accumulated drag (x right, y up)
    void add(float dx_px, float dy_px_down) {
        ax += dx_px;
        ay -= dy_px_down;
        const float m = std::sqrt(ax * ax + ay * ay);
        if (m > radius_px) { ax *= radius_px / m; ay *= radius_px / m; } // clamp, like a stick rim
    }
    void reset() { ax = ay = 0; }
    float x() const { return ax / radius_px; }
    float y() const { return ay / radius_px; }
};

inline const char* sword_sector_name_nl(int sec) {
    static const char* n[8] = {"omhoog", "rechts-boven", "rechts", "rechts-onder", "omlaag", "links-onder", "links",
                               "links-boven"};
    return n[sec & 7];
}

} // namespace nwii::runtime::input
