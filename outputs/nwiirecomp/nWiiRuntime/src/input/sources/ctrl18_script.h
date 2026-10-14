#pragma once
// CTRL18-INPUT (design section 7, track T9): the "controls" live script. Selected by NWII_SS_REPLAY_CTRL18=1 together
// with the load-save replay (probe.py --input-replay load-save): once the save is loaded and Link is controllable
// ("[Load] LOADED"), BootReplaySource hands the controls to this script (marked CTRL18-INPUT hook lines in
// boot_replay_source.h). It drives VIRTUAL PAD INTENTS (a Switch-HD pad: sticks and HD buttons) through the production
// switch-hd translator (hd_translator.h, the same code the gamepad source runs) and reports per step what the game did,
// from read-only observations only (game_observation.h, game_context.h): "[Ctrl18Test] step=<name> <PASS|FAIL|INFO> ...".
// Nothing is written to guest memory.
// Steps: idle (negative control), walk, free-camera orbit + walk, 8 right-stick slashes, R3 thrust, hold-up raise +
// strike, L-R-L spin, B at the put-away prompt, B-dash, L3 shield, Y (camera behind Link), item wheel (ZR + stick),
// first person (D-up) + right-stick look, pouch (R), gear (+), map (-), idle again.
// Env: NWII_SS_CTRL18_STEPS = comma list of step names to run (default all).
#include "input/input_source.h"
#include "../game_observation.h"
#include "../game_context.h"
#include "../hd_translator.h"
#include "../motion_gesture.h"
#include "../virtual_motion.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace nwii::runtime::input {

class Ctrl18Script {
public:
    static bool requested() {
        const char* e = std::getenv("NWII_SS_REPLAY_CTRL18");
        return e && e[0] && e[0] != '0';
    }

    // BootReplaySource (load-save, after LOADED): one update while the script owns the controls. Returns no Wii A
    // (everything is applied in apply()); the Nunchuk stick comes from the translator in apply().
    bool update(const GameObservation& o, uint64_t now, bool fresh, float& sx, float& sy) {
        if (!active_) begin(now);
        now_ = now;
        if (fresh) observe(o);
        sx = sy = 0.0f;
        return false;
    }

    // End of BootReplaySource::update: the translated Wii signals of this update.
    void apply(WiimoteState& m) {
        if (!active_ || done_) return;
        const double dt = m.motion.dt_seconds;
        PadIntents in;
        program(in);
        in.dt = dt > 0.0 && dt <= 0.1 ? dt : 0.0;
        const GameContext ctx = g_guest_context.get();
        const LiveFlags live = LiveFlags::read(0);
        WiiOut out;
        tr_.step(in, ctx, live, g_virtual_gesture[0], g_virtual_orientation[0], g_pointer_aim[0], out);
        if (in.held[kHdCamera]) ctrl18_feed_camera(aim_axis(in.rx, in.right_dead), aim_axis(in.ry, in.right_dead));
        else ctrl18_feed_camera(0.0f, 0.0f);
        merge_virtual_motion(m.motion, true, out.pitch, out.yaw, out.roll);
        m.buttons |= out.buttons;
        m.nunchuk.available = true;
        m.nunchuk.stick_x = out.stick_x;
        m.nunchuk.stick_y = out.stick_y;
        m.nunchuk.buttons |= out.nunchuk_buttons;
        buttons_seen_ |= out.buttons;
        nunchuk_seen_ |= out.nunchuk_buttons;
        situation_ = out.situation;
    }

private:
    static constexpr uint64_t kMs = 1'000'000ull;
    struct Step {
        const char* name;
        double seconds;
        std::function<void(double, PadIntents&)> drive;
        std::function<void(Step&)> judge;
    };
    // Per-step accumulators of read-only observations.
    struct Acc {
        uint32_t frames = 0;
        uint8_t remote_or = 0, nunchuk_or = 0;
        uint32_t sword_or = 0, aim_or = 0;
        int attacks = 0;
        int16_t last_angle = 0;
        std::vector<int16_t> angles;
        std::vector<int> attack_dirs, actions;
        float start_pos[3] = {0, 0, 0}, end_pos[3] = {0, 0, 0};
        int16_t start_move = 0, end_move = 0, start_stick = 0, end_stick = 0;
        uint16_t item_start = 0, item_end = 0, equipped_start = 0, equipped_end = 0;
        bool cam_modes[16] = {};
        bool pointer_cursor = false, event = false;
        std::vector<std::array<float, 3>> trail; // Link positions, one per observed frame
        std::vector<int16_t> moves;              // LINK +0x4420 per observed frame
        int8_t wheel_seen = -1;
        float mz_start[3] = {0, 0, 1}, mz_end[3] = {0, 0, 1};
        int prompt_a_start = -1;
    };

    void begin(uint64_t now) {
        active_ = true;
        t0_ = now;
        const char* only = std::getenv("NWII_SS_CTRL18_STEPS");
        build();
        if (only && only[0]) {
            std::vector<Step> keep;
            const std::string list = std::string(",") + only + ",";
            for (auto& s : steps_)
                if (list.find(std::string(",") + s.name + ",") != std::string::npos) keep.push_back(s);
            steps_.swap(keep);
        }
        std::printf("[Ctrl18Test] start: %zu steps, virtual Switch-HD pad intents through the switch-hd translator\n",
                    steps_.size());
        std::fflush(stdout);
        start_step(now);
    }
    double t_s() const { return double(now_ - t0_) / 1e9; }
    double step_t() const { return double(now_ - step_ns_) / 1e9; }

    void start_step(uint64_t now) {
        if (index_ >= steps_.size()) {
            done_ = true;
            std::printf("[Ctrl18Test] done: %d PASS, %d FAIL, %d INFO (t=%.1f s)\n", pass_, fail_, info_, t_s());
            std::fflush(stdout);
            return;
        }
        step_ns_ = now;
        acc_ = Acc{};
        have_start_ = false;
        buttons_seen_ = nunchuk_seen_ = 0;
        tr_.sword(); // keep the recogniser (no reset: stick history continues)
    }
    void program(PadIntents& in) {
        if (index_ >= steps_.size()) return;
        Step& s = steps_[index_];
        // A running event (dialogue / cutscene) is answered with A pulses before a step's own input.
        if (event_now_ && s.drive) {
            if (std::fmod(step_t(), 0.8) < 0.15) in.held[kHdAction] = true;
            return;
        }
        const double t = step_t();
        if (t < s.seconds) {
            if (s.drive) s.drive(t, in);
            return;
        }
        if (s.judge) s.judge(s);
        ++index_;
        start_step(now_);
    }
    void observe(const GameObservation& o) {
        event_now_ = o.event_state > 0;
        const GameContext c = g_guest_context.get();
        if (!have_start_) {
            have_start_ = true;
            std::memcpy(acc_.start_pos, o.pos, sizeof acc_.start_pos);
            acc_.last_angle = o.attack_angle;
            acc_.start_move = c.move_angle;
            acc_.start_stick = c.stick_angle;
            acc_.item_start = o.item_state;
            acc_.equipped_start = c.equipped_b;
            std::memcpy(acc_.mz_start, c.game_mz, sizeof acc_.mz_start);
            acc_.prompt_a_start = a_action(c);
        }
        ++acc_.frames;
        acc_.remote_or |= o.remote_motion;
        acc_.nunchuk_or |= o.nunchuk_motion;
        acc_.sword_or |= o.sword_states;
        acc_.aim_or |= c.aim_flags;
        if (o.attack_angle != acc_.last_angle) {
            acc_.last_angle = o.attack_angle;
            acc_.angles.push_back(o.attack_angle);
            ++acc_.attacks;
        }
        if (acc_.attack_dirs.empty() || acc_.attack_dirs.back() != o.attack_direction) acc_.attack_dirs.push_back(o.attack_direction);
        if (acc_.actions.empty() || acc_.actions.back() != o.current_action) acc_.actions.push_back(o.current_action);
        std::memcpy(acc_.end_pos, o.pos, sizeof acc_.end_pos);
        acc_.trail.push_back({o.pos[0], o.pos[1], o.pos[2]});
        acc_.moves.push_back(c.move_angle);
        acc_.end_move = c.move_angle;
        acc_.end_stick = c.stick_angle;
        acc_.item_end = o.item_state;
        acc_.equipped_end = c.equipped_b;
        if (c.cam_mode >= 0 && c.cam_mode < 16) acc_.cam_modes[c.cam_mode] = true;
        acc_.pointer_cursor = acc_.pointer_cursor || g_guest_pointer_cursor.load();
        acc_.event = acc_.event || o.event_state > 0;
        if (c.wheel_index >= 0) acc_.wheel_seen = c.wheel_index;
        std::memcpy(acc_.mz_end, c.game_mz, sizeof acc_.mz_end);
    }

    // ---- judging helpers ----
    double moved() const {
        return std::hypot(double(acc_.end_pos[0] - acc_.start_pos[0]), double(acc_.end_pos[2] - acc_.start_pos[2]));
    }
    // Link's displacement heading (atan2(dx, dz), the game's angle convention: 0 = +z) vs the angle the game moves him
    // toward (LINK +0x4420 = camera yaw + stick angle; stick forward = the camera's forward).
    double heading_diff() const {
        // Per frame of the second half of the walk (Link first turns toward the wanted direction, and the follow camera
        // keeps turning while he runs): the direction of that frame's step vs the direction the game moves him toward
        // in that frame (LINK +0x4420). Median of the frames that moved; 180 = none moved.
        const size_t n = acc_.trail.size();
        std::vector<double> d;
        for (size_t k = n / 2 + 1; k < n; ++k) {
            const double dx = acc_.trail[k][0] - acc_.trail[k - 1][0], dz = acc_.trail[k][2] - acc_.trail[k - 1][2];
            if (std::hypot(dx, dz) < 2.0) continue;
            const double h = std::atan2(dx, dz) * 180.0 / 3.14159265358979323846;
            const double want = acc_.moves[k] * 360.0 / 65536.0;
            d.push_back(std::fabs(std::remainder(h - want, 360.0)));
        }
        if (d.empty()) return 180.0;
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        return d[d.size() / 2];
    }
    bool heading_ok() const { return heading_diff() < 25.0; }
    // Blocked or sliding along scenery: the second half's typical step is under half of the fastest step before it.
    bool blocked() const {
        const size_t n = acc_.trail.size();
        if (n < 8) return true;
        std::vector<double> step;
        double fastest = 0.0;
        for (size_t k = 1; k < n; ++k) {
            const double l = std::hypot(double(acc_.trail[k][0] - acc_.trail[k - 1][0]),
                                        double(acc_.trail[k][2] - acc_.trail[k - 1][2]));
            if (k <= n / 2) fastest = std::max(fastest, l);
            else step.push_back(l);
        }
        std::nth_element(step.begin(), step.begin() + step.size() / 2, step.end());
        return fastest < 2.0 || step[step.size() / 2] < 0.5 * fastest;
    }
    std::string heading_text() const {
        char b[96];
        std::snprintf(b, sizeof b, " (moved %.0f units, heading off the camera-relative direction by %.0f deg)", moved(),
                      heading_diff());
        return b;
    }
    std::string list(const std::vector<int>& v) const {
        std::string s;
        for (int x : v) s += (s.empty() ? "" : ",") + std::to_string(x);
        return s;
    }
    void report(const char* name, int verdict, const std::string& what) { // 1 PASS, 0 FAIL, -1 INFO
        const char* v = verdict > 0 ? "PASS" : verdict == 0 ? "FAIL" : "INFO";
        (verdict > 0 ? pass_ : verdict == 0 ? fail_ : info_)++;
        std::printf("[Ctrl18Test] t=%.1f step=%s %s %s | frames=%u motion=0x%02X/0x%02X sword_or=0x%X aim_or=0x%X "
                    "actions=%s moved=%.0f item=%u->%u B=%u->%u wheel=%d situation='%s'\n",
                    t_s(), name, v, what.c_str(), acc_.frames, unsigned(acc_.remote_or), unsigned(acc_.nunchuk_or),
                    acc_.sword_or, acc_.aim_or, list(acc_.actions).c_str(), moved(), unsigned(acc_.item_start),
                    unsigned(acc_.item_end), unsigned(acc_.equipped_start), unsigned(acc_.equipped_end), int(acc_.wheel_seen),
                    situation_name_nl(situation_));
        std::fflush(stdout);
    }
    static void flick(double t, double t0, float x, float y, PadIntents& in) {
        const double u = t - t0;
        const double k = u < 0 ? 0 : u < 0.025 ? u / 0.025 : u < 0.125 ? 1.0 : u < 0.15 ? 1.0 - (u - 0.125) / 0.025 : 0.0;
        if (k > 0) { in.rx = float(x * k); in.ry = float(y * k); }
    }
    static int sector_of_angle(int16_t a) {
        // attack angle = atan2s(v.y, -v.x) of the swing vector (game_observation.h); 0x4000 steps = 90 deg.
        return ((int(a) + 0x1000) & 0xFFFF) / 0x2000; // 8 sectors of the game's own angle
    }

    void build() {
        steps_.clear();
        steps_.push_back({"idle", 3.0, nullptr, [this](Step& s) {
            const bool quiet = !acc_.remote_or && !acc_.nunchuk_or && acc_.attacks == 0 && moved() < 10.0;
            report(s.name, quiet ? 1 : 0, "negative control: no input -> no motion flags, no attack, Link still");
        }});
        steps_.push_back({"walk", 2.0, [](double, PadIntents& in) { in.ly = 1.0f; }, [this](Step& s) {
            report(s.name, moved() < 40.0 || blocked() ? -1 : heading_ok() ? 1 : 0,
                   "left stick forward 2 s -> Link walks toward the camera's forward (INFO = blocked by scenery)" +
                       heading_text());
        }});
        steps_.push_back({"orbit", 1.5, [](double, PadIntents& in) { in.held[kHdCamera] = true; in.rx = 1.0f; },
                          [this](Step& s) {
            const int d = int(int16_t(acc_.end_move - acc_.end_stick)) - int(int16_t(acc_.start_move - acc_.start_stick));
            const double deg = int16_t(d) * 360.0 / 65536.0;
            char b[160];
            std::snprintf(b, sizeof b, "L + right stick 1.5 s -> camera yaw change %.0f deg (needs the CTRL18-CAM seam; "
                          "no sword: %s)", deg, acc_.attacks ? "NO" : "ok");
            report(s.name, acc_.attacks ? 0 : (std::fabs(deg) > 30.0 ? 1 : -1), b);
        }});
        steps_.push_back({"walk2", 2.0, [](double, PadIntents& in) { in.ly = 1.0f; }, [this](Step& s) {
            report(s.name, moved() < 40.0 || blocked() ? -1 : heading_ok() ? 1 : 0,
                   "walk after the orbit: toward the NEW camera forward (INFO = blocked by scenery)" + heading_text());
        }});
        static const float d = 0.70710678f;
        static const float dirs[8][2] = {{0, 1}, {d, d}, {1, 0}, {d, -d}, {0, -1}, {-d, -d}, {-1, 0}, {-d, d}};
        steps_.push_back({"slashes", 8 * 0.9 + 0.5, [](double t, PadIntents& in) {
            const int k = int(t / 0.9);
            if (k < 8) flick(t, k * 0.9, dirs[k][0], dirs[k][1], in);
        }, [this](Step& s) {
            report(s.name, acc_.attacks >= 7 ? 1 : 0,
                   "8 right-stick flicks (U, UR, R, DR, D, DL, L, UL) -> " + std::to_string(acc_.attacks) +
                   " attack starts (Link +0x444C changes); attack dirs " + list(acc_.attack_dirs));
        }});
        steps_.push_back({"thrust", 1.5, [](double t, PadIntents& in) { in.held[kHdThrust] = t < 0.1; }, [this](Step& s) {
            report(s.name, (acc_.remote_or & 0x10) ? 1 : 0, "R3 -> remote shake bit (Link +0x4365 & 0x10)");
        }});
        steps_.push_back({"raise", 2.5, [](double t, PadIntents& in) {
            if (t < 1.2) in.ry = float(std::min(1.0, t / 0.025));
            flick(t, 1.35, 0, -1, in);
        }, [this](Step& s) {
            report(s.name, acc_.attacks >= 1 ? 1 : 0,
                   std::string("hold up 1.2 s (raise; Skyward Strike needs the Goddess Sword) then flick down -> attack; "
                               "SKYWARD_STRIKE bit ") + ((acc_.sword_or & kSwordSkywardStrike) ? "seen" : "not seen"));
        }});
        steps_.push_back({"spin", 2.5, [](double t, PadIntents& in) {
            flick(t, 0.0, -1, 0, in);
            flick(t, 0.17, 1, 0, in);
            flick(t, 0.34, -1, 0, in);
        }, [this](Step& s) {
            bool spin = false;
            for (int a : acc_.attack_dirs) spin = spin || (a >= 6 && a <= 9);
            report(s.name, spin ? 1 : 0, "fast L-R-L -> spin attack (mAttackDirection 6-9: " + list(acc_.attack_dirs) +
                                         "), Nunchuk shake bit " + ((acc_.nunchuk_or & 0x10) ? "seen" : "not seen"));
        }});
        steps_.push_back({"sheathe", 2.0, [](double t, PadIntents& in) { in.held[kHdDash] = t > 0.3 && t < 0.45; },
                          [this](Step& s) {
            char b[160];
            std::snprintf(b, sizeof b, "B at the A prompt %d (41 = put away) -> item in hand %u -> %u (0x11 = sword)",
                          acc_.prompt_a_start, unsigned(acc_.item_start), unsigned(acc_.item_end));
            const bool was_sword = acc_.item_start == kItemSword;
            // At another A prompt (talk, examine) the Wii has no sheathe input: INFO.
            report(s.name, was_sword && prompt_is_cancel(acc_.prompt_a_start) ? (acc_.item_end != kItemSword ? 1 : 0) : -1, b);
        }});
        steps_.push_back({"dash", 2.5, [](double, PadIntents& in) { in.ly = 1.0f; in.held[kHdDash] = true; },
                          [this](Step& s) {
            report(s.name, (acc_.sword_or & kSwordSprinting) ? 1 : 0,
                   "B held + stick forward -> SPRINTING (Link +0x348 & 0x40), moved " + std::to_string(int(moved())));
        }});
        steps_.push_back({"shield", 1.5, [](double t, PadIntents& in) { in.held[kHdShield] = t < 0.1; }, [this](Step& s) {
            report(s.name, (acc_.nunchuk_or & 0x10) ? 1 : 0,
                   std::string("L3 -> Nunchuk shake bit (Link +0x4364 & 0x10); SHIELD_OUT ") +
                   ((acc_.sword_or & kSwordShieldOut) ? "seen" : "not seen (no shield in this save?)"));
        }});
        steps_.push_back({"recentre", 1.0, [](double t, PadIntents& in) { in.held[kHdRecentre] = t < 0.1; },
                          [this](Step& s) {
            report(s.name, acc_.cam_modes[7] ? 1 : -1, "Y -> Z tap -> camera mode 7 (behind Link) seen: " +
                                                     std::string(acc_.cam_modes[7] ? "yes" : "no"));
        }});
        steps_.push_back({"wheel", 3.0, [](double t, PadIntents& in) {
            in.held[kHdItem] = t < 2.0;
            if (t > 0.8 && t < 1.8) { in.rx = 1.0f; } // right = wheel index 6 (beetle)
        }, [this](Step& s) {
            const bool opened = (acc_.sword_or & kSwordBWheelOpen) != 0;
            char b[200];
            std::snprintf(b, sizeof b, "ZR held -> B wheel %s; right stick right -> highlighted index %d (6 expected); "
                          "equipped B %u -> %u", opened ? "open" : "NOT open", int(acc_.wheel_seen),
                          unsigned(acc_.equipped_start), unsigned(acc_.equipped_end));
            report(s.name, opened ? (acc_.wheel_seen == 6 ? 1 : 0) : -1, b);
        }});
        steps_.push_back({"put_away_item", 1.5, [](double t, PadIntents& in) { in.held[kHdDash] = t > 0.2 && t < 0.35; },
                          [this](Step& s) {
            char b[160];
            std::snprintf(b, sizeof b, "B with the item out -> item in hand %u -> %u (11 = none)", unsigned(acc_.item_start),
                          unsigned(acc_.item_end));
            const bool had_item = acc_.item_start != kItemNone && acc_.item_start != kItemSword;
            report(s.name, had_item ? (acc_.item_end == kItemNone ? 1 : 0) : -1, b);
        }});
        steps_.push_back({"first_person", 3.0, [](double t, PadIntents& in) {
            in.held[kHdFirstPerson] = t < 0.1 || (t > 2.6 && t < 2.7);
            if (t > 0.8 && t < 1.8) in.rx = 0.8f;
        }, [this](Step& s) {
            const double yaw0 = std::atan2(acc_.mz_start[0], acc_.mz_start[2]), yaw1 = std::atan2(acc_.mz_end[0], acc_.mz_end[2]);
            const bool fp = (acc_.aim_or & 0x40) || acc_.cam_modes[2];
            char b[200];
            std::snprintf(b, sizeof b, "D-up -> first person %s; right stick right 1 s -> the game's mMPLS.mZ yaw %.0f deg",
                          fp ? "on" : "NOT on", (yaw1 - yaw0) * 57.2958);
            report(s.name, fp ? 1 : 0, b);
        }});
        steps_.push_back({"pouch", 1.6, [](double t, PadIntents& in) { in.held[kHdPouch] = t < 1.0; }, [this](Step& s) {
            report(s.name, (acc_.sword_or & kSwordPouchOpen) ? 1 : -1, "R held -> pouch wheel (Link +0x348 & 0x100)");
        }});
        // Menus: press, wait until the game shows its pointer (read-only), 1 s later press again, check it closed.
        const auto menu_drive = [this](int button) {
            return [this, button](double t, PadIntents& in) {
                if (t < 0.05) menu_open_t_ = -1;
                if (menu_open_t_ < 0 && t > 0.2 && g_guest_pointer_cursor.load()) menu_open_t_ = t;
                // Not open yet after 2.2 s / 3.7 s (a HUD animation can swallow the press): press again.
                const bool retry = menu_open_t_ < 0 && ((t > 2.2 && t < 2.35) || (t > 3.7 && t < 3.85));
                in.held[button] = t < 0.15 || retry ||
                                  (menu_open_t_ >= 0 && t > menu_open_t_ + 1.0 && t < menu_open_t_ + 1.15);
            };
        };
        const auto menu_judge = [this](const char* what) {
            return [this, what](Step& s) {
                const bool opened = menu_open_t_ >= 0, closed = !g_guest_pointer_cursor.load();
                char b[200];
                std::snprintf(b, sizeof b, "%s: opened (the game's pointer shown) %s after %.1f s; closed again: %s", what,
                              opened ? "yes" : "NO", opened ? menu_open_t_ : 0.0, closed ? "yes" : "NO");
                report(s.name, opened && closed ? 1 : 0, b);
            };
        };
        steps_.push_back({"gear", 7.5, menu_drive(kHdGear), menu_judge("+ = gear / pause menu")});
        steps_.push_back({"map", 7.5, menu_drive(kHdMap), menu_judge("- = map")});
        steps_.push_back({"idle_end", 3.0, nullptr, [this](Step& s) {
            const bool quiet = !acc_.remote_or && !acc_.nunchuk_or && acc_.attacks == 0;
            report(s.name, quiet ? 1 : 0, "negative control at the end: no stray motion flags or attacks");
        }});
    }

    bool active_ = false, done_ = false, have_start_ = false, event_now_ = false;
    uint64_t t0_ = 0, now_ = 0, step_ns_ = 0;
    size_t index_ = 0;
    std::vector<Step> steps_;
    Acc acc_;
    HdPadTranslator tr_;
    uint32_t buttons_seen_ = 0, nunchuk_seen_ = 0;
    Situation situation_ = Situation::Play;
    int pass_ = 0, fail_ = 0, info_ = 0;
    double menu_open_t_ = -1;
};

} // namespace nwii::runtime::input
