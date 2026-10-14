#pragma once
// SOAK14 (wave 14): long soak / exploration replay script.
//
// Selected by NWII_SS_REPLAY_SOAK=1 (or NWII_SS_INPUT_REPLAY=soak once a caller maps that name) together with the
// new-game-long replay (probe.py --input-replay new-game-long). When the PROG11 progression director has finished
// its route in Skyloft (F000), BootReplaySource hands the controls to this script (marked "SOAK14" hook lines in
// boot_replay_source.h) and it plays like a restless player for as long as the run lasts:
//   walk / run / dash, long outward runs (ledges, island edge), roll attempts, camera (C first person, Z re-centre),
//   Z-targeting with strafing, sword swings in 8 directions + thrust + spin (motion_gesture.h), shield bash
//   (Nunchuk flick), A at the game's own A-button prompts (talk, examine/read, open, climb, jump/dive, pick up/throw,
//   sit/stand, dash/roll), the pause menu (+), the map (1), the item wheel (B held) and the pouch (- held) with the
//   menu pointer moved by a virtual right stick through the same target-cursor model as INPUT13's game-menu path
//   (gamepad_source.cpp: cursor += stick^2 * 1.6/s, the virtual remote turned onto it with rates_toward).
// Events (dialogs, cutscenes) get A pulses; menus that stay open, Link stuck in place or no progress for 60 s
// trigger an "unstick" sequence. Everything is logged as "[Soak]" lines, with a one-line summary every 60 s.
//
// Inputs: only read-only observations (game_observation.h: stage, Link position/facing/action, EventManager state,
// A-button prompt; virtual_motion.h: g_guest_pointer_cursor; boot_replay_source.h: the game's pointer read-back).
// Outputs: Wii Remote / Nunchuk buttons, the Nunchuk stick, gestures and pointer rates, like a player.
// Nothing is written to guest memory.
//
// Env (all optional): NWII_SS_SOAK_SEED (default 14), NWII_SS_SOAK_TETHER (max distance from the soak's home
// point in the current stage before it walks back, default 2500 units, 0 = off), NWII_SS_SOAK_ACTS (comma list of
// activity names to restrict the random choice to), NWII_SS_SOAK_VERBOSE=1 (log every activity start),
// NWII_SS_SOAK_CHOICE=1 (in events: a Nunchuk stick flick before every A pulse, so dialog choices get selected).
#include "input/input_source.h"
#include "../game_observation.h"
#include "../motion_gesture.h"
#include "../virtual_motion.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace nwii::runtime::input {

class SoakScript {
public:
    enum Act {
        kIdle, kWalk, kRun, kDash, kRoll, kSwings, kZTarget, kSpin, kShield, kCamera, kZTap, kPause, kMap, kWheel,
        kPouch, kOutward, kPrompt, kEvent, kUnstick, kHome, kActCount
    };
    static const char* act_name(int a) {
        static const char* names[kActCount] = {"idle", "walk", "run", "dash", "roll", "swings", "ztarget", "spin",
                                               "shield", "camera", "ztap", "pause", "map", "wheel", "pouch", "outward",
                                               "prompt", "event", "unstick", "home"};
        return a >= 0 && a < kActCount ? names[a] : "?";
    }
    // Button masks: Wii Remote (KPAD, as gamepad_source.cpp maps them) and the Nunchuk C/Z shifted up by 4 bits.
    static constexpr uint32_t kBtnLeft = 0x0001, kBtnRight = 0x0002, kBtnDown = 0x0004, kBtnUp = 0x0008,
                              kBtnPlus = 0x0010, kBtnTwo = 0x0100, kBtnOne = 0x0200, kBtnB = 0x0400, kBtnA = 0x0800,
                              kBtnMinus = 0x1000, kNunZ = 0x20000, kNunC = 0x40000;
    // A-button prompt values (LytDoButtonRelated Act_IE_e, d_lyt_unknowns.h).
    enum Prompt {
        kPClimbUp = 0, kPJump = 1, kPDropDown = 2, kPLetGo = 3, kPOpen = 4, kPDrop = 5, kPThrow = 6, kPRoll = 8,
        kPGrab = 11, kPTalk = 16, kPExamine = 17, kPLeap = 24, kPDash = 25, kPGetOut = 26, kPPickUp = 28, kPSit = 29,
        kPStand = 30, kPDrop2 = 57, kPRead = 59, kPJumpDown = 68, kPDive = 85, kPEtcDash = 95, kPEtcRoll = 100,
        kPNone = 152
    };

    struct Stats {
        uint64_t frames = 0;
        uint32_t acts[kActCount] = {};
        uint32_t events = 0, long_events = 0, talks = 0, talk_events = 0, prompts_pressed = 0;
        uint32_t menus = 0, menus_opened = 0, menus_closed = 0, menus_stuck = 0, menu_cursor_frames = 0;
        uint32_t strokes = 0, menu_strokes = 0, strokes_ok = 0; // menu_strokes: with the game's menu pointer shown
        float stroke_err_max = 0, stroke_err_sum = 0;
        uint32_t stalls = 0, falls = 0, softlocks = 0, nan = 0, stage_changes = 0, gestures = 0;
        uint32_t prompt_seen[160] = {};
        float dist = 0;
    };

    static bool requested() {
        const char* e = std::getenv("NWII_SS_REPLAY_SOAK");
        const char* r = std::getenv("NWII_SS_INPUT_REPLAY");
        return (e && e[0] && e[0] != '0') || (r && std::strcmp(r, "soak") == 0);
    }
    bool active() const { return active_; }
    const Stats& stats() const { return st_; }
    uint32_t cells() const { return cells_n_; } // distinct 1000-unit area cells visited
    int route_reached() const { return route_reached_; } // NWII_SS_SOAK_ROUTE waypoints reached
    int act() const { return act_; }

    // One input update while the soak owns the controls. `fresh` = a new game frame was observed.
    // Returns the A button; the Nunchuk stick goes to sx/sy.
    bool update(const GameObservation& o, uint64_t now, bool fresh, float& sx, float& sy) {
        if (!active_) begin(o, now);
        now_ = now;
        if (fresh) observe(o, now);
        step(o, now, fresh);
        uint32_t b = hold_ | (tap_on_ ? tap_mask_ : 0);
        cur_buttons_ = b;
        sx = stick_x_;
        sy = stick_y_;
        return (b & kBtnA) != 0;
    }

    // End of BootReplaySource::update: the soak's other buttons and the pointer (virtual right stick).
    // game_x/y = the game's own pointer read back (dPad::ex_c pos, g_replay_pointer_x/y), used by the next update.
    void apply(WiimoteState& m, float game_x, float game_y) {
        gptr_x_ = game_x;
        gptr_y_ = game_y;
        if (!active_) return;
        // NWII_SS_SOAK_PTRLOG=1 (diagnostic): every 0.5 s the pointer of our virtual remote orientation (what the
        // target-cursor model assumes the game shows) next to the game's own pointer, with the activity.
        static const bool ptrlog = [] { const char* e = std::getenv("NWII_SS_SOAK_PTRLOG"); return e && e[0] == '1'; }();
        if (ptrlog && now_ - ptrlog_ns_ >= 500 * kMs) {
            ptrlog_ns_ = now_;
            double mm[3][3];
            g_virtual_orientation[0].matrix(mm);
            const double ox = std::atan2(-mm[0][2], mm[2][2]) / kPointerHalfRangeRad;
            const double oy = std::atan2(-mm[1][2], mm[2][2]) / kPointerHalfRangeRad;
            std::printf("[Soak] ptr t=%.1f act=%s cursor=%d ours=(%.3f,%.3f) game=(%.3f,%.3f) d=(%.3f,%.3f) "
                        "learned=(%.3f,%.3f)/%u roll=%.1fdeg\n",
                        t_s(), act_name(act_), int(cursor()), ox, oy, game_x, game_y, game_x - ox, game_y - oy,
                        g_pointer_aim[0].off_x, g_pointer_aim[0].off_y, g_pointer_aim[0].learned,
                        std::atan2(mm[1][0], mm[1][1]) * 57.2958);
        }
        m.buttons |= cur_buttons_ & 0xFFFF;
        m.nunchuk.buttons |= (cur_buttons_ >> 4) & 0x6000;
        const float dt = m.motion.dt_seconds;
        if (!(dt > 0.0f && dt <= 0.1f) || point_mode_ == 0) return;
        // INPUT13 game-menu pointer model (gamepad_source.cpp): squared stick response, ~1.6 screen halves/s at full
        // deflection, the remote turned onto the target with transient rates (the KMPLS zero-drift correction never
        // sees a held rate). Screen axes: x right, y down.
        const auto resp = [](float v) { v = std::clamp(v, -1.0f, 1.0f); return std::copysign(v * v, v); };
        cursor_x_ = std::clamp(cursor_x_ + resp(rs_x_) * 1.6f * dt, -1.0f, 1.0f);
        cursor_y_ = std::clamp(cursor_y_ + resp(rs_y_) * 1.6f * dt, -1.0f, 1.0f);
        double target[3];
        g_pointer_aim[0].aim(cursor_x_, cursor_y_, target); // closed loop on the game's pointer (virtual_motion.h)
        float rates[3];
        g_virtual_orientation[0].rates_toward(target, dt, kPointerMotionMaxRateRadS, rates);
        merge_virtual_motion(m.motion, true, rates[0], rates[1], rates[2]); // rates[2]: level roll
    }

private:
    static constexpr uint64_t kMs = 1'000'000ull;

    // ---------------------------------------------------------------- helpers
    uint32_t rnd() { seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5; return seed_; }
    float frand() { return (rnd() >> 8) * (1.0f / 16777216.0f); }
    int irand(int lo, int hi) { return lo + int(rnd() % uint32_t(hi - lo + 1)); }
    double t_s() const { return (now_ - start_ns_) / 1e9; }
    static bool event_running(const GameObservation& o) { return o.event_state > 0; }
    bool menu_act() const { return act_ == kPause || act_ == kMap || act_ == kWheel || act_ == kPouch; }
    static bool cursor() { return g_guest_pointer_cursor.load(); }

    void tap(uint32_t mask, uint32_t frames = 6, uint64_t ms = 200) {
        tap_mask_ = mask; tap_on_ = true; tap_frames_ = frames; tap_until_ = now_ + ms * kMs; tap_gap_ = 0;
    }
    bool tap_busy() const { return tap_on_ || tap_gap_ > 0; }
    void tap_tick(bool fresh) {
        if (!fresh) return;
        if (tap_on_) {
            if (tap_frames_ > 0) --tap_frames_;
            if (tap_frames_ == 0 && now_ >= tap_until_) { tap_on_ = false; tap_gap_ = 6; }
        } else if (tap_gap_ > 0) {
            --tap_gap_;
        }
    }

    void log_line(const char* text) { std::printf("[Soak] t=%.1f %s\n", t_s(), text); std::fflush(stdout); }

    void begin(const GameObservation& o, uint64_t now) {
        active_ = true;
        start_ns_ = minute_ns_ = progress_ns_ = now_ = now;
        if (const char* e = std::getenv("NWII_SS_SOAK_SEED")) seed_ = uint32_t(std::strtoul(e, nullptr, 0)) | 1u;
        // SOAK14 explore mode: leave the start area (tether 9000, 5x the outward runs and longer ones, every
        // dive/jump/leap prompt taken).
        if (const char* e = std::getenv("NWII_SS_SOAK_EXPLORE")) explore_ = e[0] == '1';
        if (explore_) tether_ = 9000.0f;
        // Explore route: NWII_SS_SOAK_ROUTE="x,z;x,z;..." (world units, the start stage): the outward runs walk these
        // waypoints in order (reached within 300 units; skipped after two outward runs that ended in stalls), then
        // the frontier heading takes over. Implies explore mode; tether 20000.
        if (const char* e = std::getenv("NWII_SS_SOAK_ROUTE")) {
            const char* c = e;
            while (*c && route_n_ < 32) {
                char* end = nullptr;
                const float x = std::strtof(c, &end);
                if (end == c || *end != ',') break;
                c = end + 1;
                const float z = std::strtof(c, &end);
                if (end == c) break;
                route_[route_n_][0] = x; route_[route_n_][1] = z; ++route_n_;
                c = end;
                while (*c == ';' || *c == ' ') ++c;
            }
            if (route_n_) { explore_ = true; tether_ = 20000.0f; std::memcpy(route_stage_, o.stage, sizeof(route_stage_)); }
        }
        if (const char* e = std::getenv("NWII_SS_SOAK_CHOICE")) choice_ = e[0] == '1';
        if (choice_) choice_salt_ = rnd() % 4u;
        if (const char* e = std::getenv("NWII_SS_SOAK_TETHER")) tether_ = float(std::atof(e));
        if (const char* e = std::getenv("NWII_SS_SOAK_VERBOSE")) verbose_ = e[0] == '1';
        if (const char* e = std::getenv("NWII_SS_SOAK_ACTS")) {
            std::string list = std::string(",") + e + ",";
            allowed_mask_ = 0;
            for (int a = 0; a < kActCount; ++a)
                if (list.find(std::string(",") + act_name(a) + ",") != std::string::npos) allowed_mask_ |= 1u << a;
            if (!allowed_mask_) allowed_mask_ = ~0u;
        }
        std::memcpy(stage_, o.stage, sizeof(stage_));
        room_ = o.room; layer_ = o.layer;
        minute_frame_ = o.frame;
        std::printf("[Soak] t=0.0 SOAK START stage=%s r%d l%d pos=(%.0f,%.0f,%.0f) seed=%u tether=%.0f acts=0x%X "
                    "explore=%d route=%d (NWII_SS_REPLAY_SOAK; read-only observation, controller input only)\n", o.stage,
                    o.room, o.layer, o.pos[0], o.pos[1], o.pos[2], seed_, tether_, allowed_mask_, int(explore_), route_n_);
        std::fflush(stdout);
        set_home(o);
        pick_next(o);
    }

    void set_home(const GameObservation& o) {
        if (!o.link_present || !std::isfinite(o.pos[0]) || !std::isfinite(o.pos[2])) { home_valid_ = false; return; }
        home_x_ = o.pos[0]; home_y_ = o.pos[1]; home_z_ = o.pos[2]; home_valid_ = true;
    }

    // ---------------------------------------------------------------- observation, statistics, anomalies
    void observe(const GameObservation& o, uint64_t now) {
        ++st_.frames;
        ++act_frames_;
        if (o.link_present && !(std::isfinite(o.pos[0]) && std::isfinite(o.pos[1]) && std::isfinite(o.pos[2]))) {
            if (++st_.nan <= 5) {
                char b[160];
                std::snprintf(b, sizeof b, "ANOMALY: Link position not finite (frame %u action %d act %s)", o.frame,
                              o.current_action, act_name(act_));
                log_line(b);
            }
            return;
        }
        if (std::strcmp(o.stage, stage_) != 0 || o.room != room_ || o.layer != layer_) {
            char b[200];
            const double secs = (now - stage_ns_) / 1e9;
            std::snprintf(b, sizeof b, "stage %s r%d l%d -> %s r%d l%d e%d (%.0f s in the previous one) pos=(%.0f,%.0f,%.0f)",
                          stage_, room_, layer_, o.stage, o.room, o.layer, o.entrance, secs, o.pos[0], o.pos[1],
                          o.pos[2]);
            log_line(b);
            std::memcpy(stage_, o.stage, sizeof(stage_));
            room_ = o.room; layer_ = o.layer;
            stage_ns_ = now;
            ++st_.stage_changes;
            home_valid_ = false; // re-anchored at the first controllable frame in the new stage
            f_init_ = false;
            progress(now);
        }
        if (o.event_state != last_event_ || std::strcmp(o.event_name, last_event_name_) != 0) {
            if (event_running(o) && !(last_event_ > 0)) {
                ++st_.events;
                event_ns_ = now;
                long_event_logged_ = false;
                if (now - talk_press_ns_ < 2500 * kMs) ++st_.talk_events;
            }
            char b[200];
            std::snprintf(b, sizeof b, "event %d '%s' -> %d '%s' pos=(%.0f,%.0f,%.0f) act=%s", last_event_,
                          last_event_name_, o.event_state, o.event_name, o.pos[0], o.pos[1], o.pos[2], act_name(act_));
            log_line(b);
            last_event_ = o.event_state;
            std::memcpy(last_event_name_, o.event_name, sizeof(last_event_name_));
            progress(now);
        }
        if (event_running(o) && !long_event_logged_ && now - event_ns_ > 150'000 * kMs) {
            long_event_logged_ = true;
            ++st_.long_events;
            char b[160];
            std::snprintf(b, sizeof b, "LONG EVENT '%s' running for 150 s (A pulses, B every 4th pulse)", o.event_name);
            log_line(b);
        }
        const bool c = cursor();
        if (c) ++st_.menu_cursor_frames;
        last_cursor_ = c; // (menus opening/closing is not progress: a physically stuck Link can still open menus)
        if (o.do_action >= 0 && o.do_action < 160) ++st_.prompt_seen[o.do_action];
        if (o.do_action != last_prompt_logged_ && o.do_action >= 0 && o.do_action < 160 &&
            !(seen_prompt_[o.do_action / 32] & (1u << (o.do_action % 32)))) {
            seen_prompt_[o.do_action / 32] |= 1u << (o.do_action % 32);
            char b[160];
            std::snprintf(b, sizeof b, "new A prompt %d (shown %d) at %s pos=(%.0f,%.0f,%.0f)", o.do_action, o.do_shown,
                          o.stage, o.pos[0], o.pos[1], o.pos[2]);
            log_line(b);
        }
        last_prompt_logged_ = o.do_action;
        if (swing_req_d_ >= 0 && o.attack_angle != swing_angle0_) {
            static const char* names[9] = {"right", "left", "up", "down", "up-right", "up-left", "down-right", "down-left",
                                           "thrust"};
            char b[160];
            std::snprintf(b, sizeof b, "swing %s (roll %.0f deg) -> attack_angle %.0f deg dir %u after %.2f s", names[swing_req_d_],
                          swing_roll_, o.attack_angle * (360.0 / 65536.0), unsigned(o.attack_direction),
                          (now - swing_req_ns_) / 1e9);
            log_line(b);
            swing_req_d_ = -1;
        } else if (swing_req_d_ >= 0 && now - swing_req_ns_ > 1500 * kMs) {
            static const char* names[9] = {"right", "left", "up", "down", "up-right", "up-left", "down-right", "down-left",
                                           "thrust"};
            if (verbose_) {
                char b[120];
                std::snprintf(b, sizeof b, "swing %s (roll %.0f deg): no new attack angle in 1.5 s", names[swing_req_d_],
                              swing_roll_);
                log_line(b);
            }
            swing_req_d_ = -1;
        }
        last_attack_angle_ = o.attack_angle;
        // Movement history (steering estimate, stalls, falls, distance).
        if (o.link_present) {
            const uint32_t k = hn_ % 64;
            if (hn_ > 0) {
                const uint32_t p = (hn_ - 1) % 64;
                const float d = std::hypot(o.pos[0] - hx_[p], o.pos[2] - hz_[p]);
                if (d < 200) st_.dist += d; // ignore teleports (respawn, stage load)
            }
            hx_[k] = o.pos[0]; hy_[k] = o.pos[1]; hz_[k] = o.pos[2]; hsx_[k] = stick_x_; hsy_[k] = stick_y_;
            ++hn_;
            if (std::hypot(o.pos[0] - prog_x_, o.pos[2] - prog_z_) > 60 || std::fabs(o.pos[1] - prog_y_) > 60) {
                prog_x_ = o.pos[0]; prog_y_ = o.pos[1]; prog_z_ = o.pos[2];
                progress(now);
            }
            if (hn_ > 45) {
                float ymax = -1e30f;
                for (uint32_t i = 0; i < 45; ++i) ymax = std::max(ymax, hy_[(hn_ - 1 - i) % 64]);
                if (ymax - o.pos[1] > 700 && now - fall_ns_ > 10'000 * kMs) {
                    fall_ns_ = now;
                    ++st_.falls;
                    char b[160];
                    std::snprintf(b, sizeof b, "FALL: dropped %.0f units in 1.5 s at (%.0f,%.0f,%.0f) act=%s action=%d",
                                  ymax - o.pos[1], o.pos[0], o.pos[1], o.pos[2], act_name(act_), o.current_action);
                    log_line(b);
                }
            }
        }
        if (!home_valid_ && o.link_present && !event_running(o)) set_home(o);
        if (o.link_present) note_cell(o);
        // The route belongs to the first stage in which Link is controllable.
        if (route_n_ && !route_stage_[0] && o.link_present && o.stage[0])
            std::memcpy(route_stage_, o.stage, sizeof(route_stage_));
        if (now - minute_ns_ >= 60'000 * kMs) minute_summary(o, now);
    }

    void progress(uint64_t now) { progress_ns_ = now; stuck_move_frames_ = 0; }
    // Coverage: distinct 1000 x 1000 unit cells (per stage name) Link has been in; each new one is logged. The frames
    // spent per cell steer explore mode's outward runs toward the least visited neighbourhood (frontier_heading).
    static uint64_t cell_key(const char* stage, float x, float z) {
        const int cx = int(std::floor(x / 1000.0f)), cz = int(std::floor(z / 1000.0f));
        uint32_t h = 2166136261u;
        for (int k = 0; k < 8 && stage[k]; ++k) h = (h ^ uint8_t(stage[k])) * 16777619u;
        return (uint64_t(h) << 32) ^ (uint64_t(uint16_t(cx)) << 16) ^ uint16_t(cz);
    }
    uint32_t cell_frames(uint64_t key) const {
        for (uint32_t i = 0; i < cells_n_; ++i) if (cells_[i] == key) return cells_frames_[i];
        return 0;
    }
    void note_cell(const GameObservation& o) {
        const int cx = int(std::floor(o.pos[0] / 1000.0f)), cz = int(std::floor(o.pos[2] / 1000.0f));
        const uint64_t key = cell_key(o.stage, o.pos[0], o.pos[2]);
        for (uint32_t i = 0; i < cells_n_; ++i) if (cells_[i] == key) { ++cells_frames_[i]; return; }
        if (cells_n_ >= 512) return;
        cells_frames_[cells_n_] = 1;
        cells_[cells_n_++] = key;
        char b[160];
        std::snprintf(b, sizeof b, "new area cell %u: %s (%d,%d) pos=(%.0f,%.0f,%.0f)", cells_n_, o.stage, cx, cz,
                      o.pos[0], o.pos[1], o.pos[2]);
        log_line(b);
    }

    void minute_summary(const GameObservation& o, uint64_t now) {
        const double secs = (now - minute_ns_) / 1e9;
        const uint32_t frames = o.frame - minute_frame_;
        char acts[300] = {};
        size_t n = 0;
        for (int a = 0; a < kActCount; ++a)
            if (st_.acts[a] && n + 24 < sizeof acts)
                n += std::snprintf(acts + n, sizeof acts - n, "%s%s:%u", n ? "," : "", act_name(a), st_.acts[a]);
        std::printf("[Soak] minute %u t=%.0f stage=%s r%d l%d pos=(%.0f,%.0f,%.0f) fps=%.1f dist=%.0f acts=%s events=%u "
                    "long=%u talks=%u/%u prompts=%u menus=%u open=%u closed=%u stuck=%u strokes=%u/%u ok=%u err_max=%.3f "
                    "stalls=%u falls=%u stages=%u softlocks=%u nan=%u gestures=%u cells=%u\n",
                    ++minute_n_, t_s(), o.stage, o.room, o.layer, o.pos[0], o.pos[1], o.pos[2],
                    secs > 0 ? frames / secs : 0.0, st_.dist, acts, st_.events, st_.long_events, st_.talk_events,
                    st_.talks, st_.prompts_pressed, st_.menus, st_.menus_opened, st_.menus_closed, st_.menus_stuck,
                    st_.menu_strokes, st_.strokes, st_.strokes_ok, st_.stroke_err_max, st_.stalls, st_.falls, st_.stage_changes,
                    st_.softlocks, st_.nan, st_.gestures, cells_n_);
        std::fflush(stdout);
        minute_ns_ = now;
        minute_frame_ = o.frame;
    }

    // ---------------------------------------------------------------- steering (camera-relative stick)
    // Same model as the PROG11 director: Link moves along F*(sy + i*sx) (complex x + i*z), F = camera forward,
    // initialised from Link's facing and re-measured from his own displacement while the command is steady.
    void steer_world(const GameObservation& o, float dx, float dz, float mag, bool fresh) {
        if (!f_init_) {
            const float a = o.rot_y * (3.14159265f / 32768.0f);
            fx_ = std::sin(a); fz_ = std::cos(a); f_init_ = true;
        }
        constexpr uint32_t K = 5;
        if (fresh && hn_ > K + 1) {
            const uint32_t a = (hn_ - 1) % 64, b = (hn_ - 1 - K) % 64;
            const float ddx = hx_[a] - hx_[b], ddz = hz_[a] - hz_[b], dl = std::hypot(ddx, ddz);
            float sx = 0, sy = 0;
            for (uint32_t i = 0; i < K; ++i) { sx += hsx_[(hn_ - 1 - i) % 64]; sy += hsy_[(hn_ - 1 - i) % 64]; }
            const float sl = std::hypot(sx, sy);
            bool steady = sl > 0;
            for (uint32_t i = 0; i < K && steady; ++i) {
                const uint32_t j = (hn_ - 1 - i) % 64;
                const float cl = std::hypot(hsx_[j], hsy_[j]);
                steady = cl > 0.28f && (hsx_[j] * sx + hsy_[j] * sy) / (cl * sl) > 0.87f;
            }
            if (steady && dl > 12.0f && dl < 200.0f) {
                const float hx = ddx / dl, hz = ddz / dl, ux = sx / sl, uy = sy / sl;
                const float mx = hx * uy + hz * ux, mz = hz * uy - hx * ux;
                fx_ += 0.3f * (mx - fx_); fz_ += 0.3f * (mz - fz_);
                const float nn = std::hypot(fx_, fz_);
                if (nn > 1e-3f) { fx_ /= nn; fz_ /= nn; } else f_init_ = false;
            }
        }
        const float d = std::hypot(dx, dz);
        if (!(d > 1e-3f)) { stick_x_ = stick_y_ = 0; return; }
        dx /= d; dz /= d;
        stick_x_ = mag * (dz * fx_ - dx * fz_);
        stick_y_ = mag * (dx * fx_ + dz * fz_);
    }
    float moved(uint32_t n) const {
        if (hn_ <= n) return 1e9f;
        const uint32_t a = (hn_ - 1) % 64, b = (hn_ - 1 - n) % 64;
        return std::hypot(hx_[a] - hx_[b], hz_[a] - hz_[b]);
    }
    // Walk along heading_ (world angle: direction (sin, cos)), with drift, tether and stall side-steps.
    void wander(const GameObservation& o, float mag, bool fresh, bool tethered = true) {
        if (fresh) { heading_ += (frand() - 0.5f) * drift_; ++stuck_move_frames_; }
        if (tethered && home_valid_ && tether_ > 0) {
            const float hx = home_x_ - o.pos[0], hz = home_z_ - o.pos[2];
            if (std::hypot(hx, hz) > tether_) heading_ = std::atan2(hx, hz) + (frand() - 0.5f) * 0.4f;
        }
        if (recover_ > 0) {
            if (fresh) --recover_;
            const float a = heading_ + side_ * 1.5708f + 3.14159265f * 0.15f * side_;
            steer_world(o, std::sin(a), std::cos(a), 1.0f, fresh);
            if (recover_ == 0) { move_hn_ = hn_; heading_ += side_ * (1.2f + 1.5f * frand()); }
            return;
        }
        if (fresh && hn_ - move_hn_ >= 45 && moved(40) < 25.0f) {
            ++st_.stalls;
            ++act_stalls_;
            if (explore_ && act_ == kOutward && act_stalls_ == 3) mark_blocked(o);
            side_ = frand() < 0.5f ? -1.0f : 1.0f;
            recover_ = 18;
            if (verbose_ || act_stalls_ == 1) {
                char b[160];
                std::snprintf(b, sizeof b, "stall in %s at (%.0f,%.0f,%.0f): moved %.0f in 40 frames; side step", act_name(act_),
                              o.pos[0], o.pos[1], o.pos[2], moved(40));
                log_line(b);
            }
        }
        steer_world(o, std::sin(heading_), std::cos(heading_), mag, fresh);
    }

    // ---------------------------------------------------------------- activity selection
    void start(int a, const GameObservation& o, const char* why = nullptr) {
        act_ = a;
        act_ns_ = now_;
        act_frames_ = 0;
        act_stalls_ = 0;
        sub_ = 0;
        sub_frames_ = 0;
        count_ = 0;
        n_ = 0;
        hold_ = 0;
        rs_x_ = rs_y_ = 0;
        stick_x_ = stick_y_ = 0;
        recover_ = 0;
        point_mode_ = 0;
        move_hn_ = hn_;
        ++st_.acts[a];
        if (verbose_ || why) {
            char b[200];
            std::snprintf(b, sizeof b, "act %s%s%s stage=%s pos=(%.0f,%.0f,%.0f) prompt=%d action=%d", act_name(a),
                          why ? " - " : "", why ? why : "", o.stage, o.pos[0], o.pos[1], o.pos[2], o.do_action,
                          o.current_action);
            log_line(b);
        }
    }
    int weight(const int* weights, int a) const { return a == kOutward && explore_ ? weights[a] * 5 : weights[a]; }
    bool route_active(const GameObservation& o) const {
        return route_i_ < route_n_ && std::strcmp(o.stage, route_stage_) == 0;
    }
    // Explore mode: of 8 headings, the one whose cells 1200 and 2400 units ahead were visited least (frames), skipping
    // headings that stalled 3 times from this cell before (walls, fences); ties broken at random.
    float frontier_heading(const GameObservation& o) {
        const uint64_t here = cell_key(o.stage, o.pos[0], o.pos[2]);
        float best = 0, best_score = 1e30f;
        for (int k = 0; k < 8; ++k) {
            const float h = k * 0.78539816f;
            float score = frand() * 30.0f;
            for (float d : {1200.0f, 2400.0f})
                score += float(cell_frames(cell_key(o.stage, o.pos[0] + d * std::sin(h), o.pos[2] + d * std::cos(h))));
            for (const auto& b : blocked_) if (b.cell == here && b.dir == k && b.cell) score += 1e6f;
            if (score < best_score) { best_score = score; best = h; frontier_k_ = k; }
        }
        return best + (frand() - 0.5f) * 0.3f;
    }
    void mark_blocked(const GameObservation& o) {
        const int k = frontier_k_; // the heading chosen for this outward run (side steps turn heading_)
        blocked_[blocked_i_++ % 64] = {cell_key(o.stage, o.pos[0], o.pos[2]), k};
        char b[120];
        std::snprintf(b, sizeof b, "explore: heading %d blocked at (%.0f,%.0f,%.0f)", k, o.pos[0], o.pos[1], o.pos[2]);
        log_line(b);
    }
    void pick_next(const GameObservation& o) {
        static const int weights[kActCount] = {
            /*idle*/ 3, /*walk*/ 9, /*run*/ 14, /*dash*/ 5, /*roll*/ 5, /*swings*/ 10, /*ztarget*/ 6, /*spin*/ 3,
            /*shield*/ 3, /*camera*/ 5, /*ztap*/ 3, /*pause*/ 6, /*map*/ 5, /*wheel*/ 5, /*pouch*/ 3, /*outward*/ 5,
            /*prompt*/ 0, /*event*/ 0, /*unstick*/ 0, /*home*/ 0};
        int total = 0;
        for (int a = 0; a < kActCount; ++a)
            if (allowed_mask_ & (1u << a)) total += weight(weights, a);
        int a = kIdle;
        if (total > 0) {
            int r = int(rnd() % uint32_t(total));
            for (a = 0; a < kActCount; ++a) {
                if (!(allowed_mask_ & (1u << a))) continue;
                if (r < weight(weights, a)) break;
                r -= weight(weights, a);
            }
            if (a >= kActCount) a = kIdle;
        }
        // Too far from home in this stage: walk back first.
        if (home_valid_ && tether_ > 0 && std::hypot(home_x_ - o.pos[0], home_z_ - o.pos[2]) > tether_ * 1.2f &&
            (allowed_mask_ & (1u << kRun)))
            a = kHome;
        if (a == kWalk || a == kRun || a == kDash || a == kRoll || a == kOutward) {
            heading_ = frand() * 6.2831853f;
            if (a == kOutward && route_active(o)) {
                heading_ = std::atan2(route_[route_i_][0] - o.pos[0], route_[route_i_][1] - o.pos[2]);
            } else if (a == kOutward && explore_) {
                heading_ = frontier_heading(o);
            } else if (a == kOutward && home_valid_) {
                const float ox = o.pos[0] - home_x_, oz = o.pos[2] - home_z_;
                if (std::hypot(ox, oz) > 100) heading_ = std::atan2(ox, oz) + (frand() - 0.5f) * 0.8f;
            }
        }
        dur_ = 0;
        switch (a) {
        case kIdle: dur_ = irand(45, 120); break;
        case kWalk: dur_ = irand(60, 180); break;
        case kRun: dur_ = irand(60, 210); break;
        case kDash: dur_ = irand(60, 120); break;
        case kRoll: dur_ = irand(90, 150); break;
        case kOutward: dur_ = route_active(o) ? 1800 : explore_ ? irand(600, 1500) : irand(300, 750); break;
        case kHome: dur_ = 900; break;
        default: break;
        }
        drift_ = a == kOutward ? 0.01f : 0.06f;
        start(a, o);
    }

    // ---------------------------------------------------------------- A-button prompts
    struct PromptRule { int prompt; float prob; uint32_t cooldown_s; uint32_t hold_frames; const char* label; };
    static const PromptRule* prompt_rule(int p) {
        static const PromptRule rules[] = {
            {kPTalk, 1.0f, 20, 6, "talk"},      {kPExamine, 0.7f, 30, 6, "examine"}, {kPRead, 0.7f, 30, 6, "read"},
            {kPOpen, 0.4f, 60, 6, "open"},      {kPClimbUp, 0.6f, 5, 6, "climb up"}, {kPJump, 0.5f, 10, 6, "jump"},
            {kPLeap, 0.5f, 10, 6, "leap"},      {kPJumpDown, 0.5f, 10, 6, "jump down"}, {kPDive, 0.5f, 120, 6, "dive"},
            {kPDropDown, 0.3f, 4, 6, "drop down"}, {kPLetGo, 0.3f, 4, 6, "let go"}, {kPPickUp, 0.4f, 20, 6, "pick up"},
            {kPGrab, 0.3f, 20, 6, "grab"},      {kPThrow, 1.0f, 3, 6, "throw"},       {kPDrop, 1.0f, 3, 6, "put down"},
            {kPDrop2, 1.0f, 3, 6, "drop"},      {kPSit, 0.2f, 60, 6, "sit"},         {kPStand, 1.0f, 3, 6, "stand"},
            {kPGetOut, 1.0f, 3, 6, "get out"},  {kPDash, 0.3f, 6, 45, "dash"},       {kPEtcDash, 0.3f, 6, 45, "dash"},
            {kPRoll, 0.3f, 6, 6, "roll"},       {kPEtcRoll, 0.3f, 6, 6, "roll"},
        };
        for (const auto& r : rules) if (r.prompt == p) return &r;
        return nullptr;
    }
    bool consider_prompt(const GameObservation& o, bool fresh) {
        if (!fresh || !o.link_present || tap_busy()) return false;
        const int p = o.do_action;
        if (p < 0 || p >= kPNone) { prompt_eval_frames_ = 0; last_prompt_eval_ = p; return false; }
        if (p == last_prompt_eval_ && ++prompt_eval_frames_ < 30) return false;
        last_prompt_eval_ = p;
        prompt_eval_frames_ = 0;
        const PromptRule* r = prompt_rule(p);
        if (!r) return false;
        if (now_ - prompt_used_ns_[p] < uint64_t(r->cooldown_s) * 1000 * kMs && prompt_used_ns_[p] != 0) return false;
        if (p == kPTalk) {
            for (const auto& t : talk_spots_)
                if (t.ns && now_ - t.ns < 120'000 * kMs && std::hypot(t.x - o.pos[0], t.z - o.pos[2]) < 250) return false;
        }
        const bool edge = p == kPDive || p == kPJump || p == kPJumpDown || p == kPLeap;
        if (frand() >= (explore_ && edge ? 1.0f : r->prob)) return false;
        prompt_used_ns_[p] = now_;
        prompt_ = p;
        prompt_hold_ = r->hold_frames;
        char b[120];
        std::snprintf(b, sizeof b, "A prompt %d '%s'", p, r->label);
        start(kPrompt, o, b);
        return true;
    }

    // ---------------------------------------------------------------- the activity machine
    void step(const GameObservation& o, uint64_t now, bool fresh) {
        tap_tick(fresh);
        if (fresh) ++sub_frames_;
        // Events (dialogs, cutscenes, also those without Link) get A pulses (do_event).
        if (event_running(o) && act_ != kEvent && !menu_act()) {
            start(kEvent, o);
            pulse_ns_ = now; pulse_frames_ = 0; pulses_ = 0;
        }
        // Loading / no player outside an event: neutral, keep any menu activity's own logic out of the way.
        if (!o.link_present && act_ != kEvent && !menu_act()) {
            stick_x_ = stick_y_ = 0; hold_ = 0; point_mode_ = 0;
            return;
        }
        // Soft-lock watchdog: no movement, event, stage or cursor change for 60 s outside an event, although the
        // script tried to walk for >= 600 game frames (~20 s) of that time.
        // Not inside a menu activity: it has its own MENU STUCK detection, and interrupting it left a menu counted as
        // opened but never as closed.
        if (act_ != kEvent && act_ != kUnstick && !menu_act() && now - progress_ns_ > 60'000 * kMs &&
            stuck_move_frames_ >= 600) {
            ++st_.softlocks;
            char b[200];
            std::snprintf(b, sizeof b, "SOFTLOCK? no progress for 60 s (act %s, cursor %d, action %d, prompt %d) at "
                          "(%.0f,%.0f,%.0f); unstick", act_name(act_), int(cursor()), o.current_action, o.do_action,
                          o.pos[0], o.pos[1], o.pos[2]);
            log_line(b);
            progress(now);
            start(kUnstick, o);
        }
        switch (act_) {
        case kEvent: return do_event(o, now, fresh);
        case kPrompt: return do_prompt(o, fresh);
        case kUnstick: return do_unstick(o, fresh);
        case kPause: return do_menu(o, fresh, kBtnPlus, false, kBtnPlus, kBtnB);
        case kMap: return do_menu(o, fresh, kBtnOne, false, kBtnB, kBtnOne);
        case kWheel: return do_menu(o, fresh, kBtnB, true, 0, kBtnB);
        case kPouch: return do_menu(o, fresh, kBtnMinus, true, 0, kBtnB);
        default: break;
        }
        // A menu that is open outside a menu activity (e.g. opened by a stray button): close it.
        if (cursor() && fresh && ++stray_cursor_frames_ > 90) {
            stray_cursor_frames_ = 0;
            log_line("menu pointer shown outside a menu activity; unstick");
            start(kUnstick, o);
            return;
        }
        if (!cursor()) stray_cursor_frames_ = 0;
        if (act_ != kSwings && act_ != kZTarget && act_ != kSpin && act_ != kShield && act_ != kCamera &&
            consider_prompt(o, fresh))
            return;
        point_mode_ = 0;
        switch (act_) {
        case kIdle:
            stick_x_ = stick_y_ = 0; hold_ = 0;
            if (act_frames_ >= dur_) pick_next(o);
            break;
        case kWalk: case kRun: case kOutward: case kHome: {
            const float mag = act_ == kWalk ? 0.45f : 1.0f;
            if (act_ == kHome && home_valid_) {
                heading_ = std::atan2(home_x_ - o.pos[0], home_z_ - o.pos[2]);
                if (std::hypot(home_x_ - o.pos[0], home_z_ - o.pos[2]) < 300) { pick_next(o); break; }
            }
            if (act_ == kOutward && route_active(o)) {
                const float rx = route_[route_i_][0] - o.pos[0], rz = route_[route_i_][1] - o.pos[2];
                if (std::hypot(rx, rz) < 300.0f) {
                    char b[120];
                    std::snprintf(b, sizeof b, "route waypoint %d/%d reached at (%.0f,%.0f,%.0f)", route_i_ + 1, route_n_,
                                  o.pos[0], o.pos[1], o.pos[2]);
                    log_line(b);
                    ++route_i_; route_fail_ = 0; ++route_reached_;
                    if (!route_active(o)) { pick_next(o); break; }
                } else if (recover_ == 0) {
                    heading_ = std::atan2(rx, rz);
                }
            }
            wander(o, mag, fresh, act_ != kOutward || (home_valid_ && std::hypot(home_x_ - o.pos[0], home_z_ - o.pos[2]) >
                                                                          2.0f * tether_));
            if (act_frames_ >= dur_ || act_stalls_ >= 5) {
                if (act_ == kOutward && route_active(o) && act_stalls_ >= 5 && ++route_fail_ >= 2) {
                    char b[120];
                    std::snprintf(b, sizeof b, "route waypoint %d/%d skipped (stalled twice) at (%.0f,%.0f,%.0f)",
                                  route_i_ + 1, route_n_, o.pos[0], o.pos[1], o.pos[2]);
                    log_line(b);
                    ++route_i_; route_fail_ = 0;
                }
                pick_next(o);
            }
            break;
        }
        case kDash:
            wander(o, 1.0f, fresh);
            hold_ = act_frames_ > 20 && act_frames_ < dur_ - 10 ? kBtnA : 0;
            if (act_frames_ >= dur_) pick_next(o);
            break;
        case kRoll:
            wander(o, 1.0f, fresh);
            if (fresh && (act_frames_ == 35 || act_frames_ == 95)) {
                auto& g = g_virtual_gesture[0];
                if (!g.active()) { (n_++ % 2 ? g.nunchuk_shake(false) : g.nunchuk_flick()); ++st_.gestures; }
            }
            if (act_frames_ >= dur_) pick_next(o);
            break;
        case kSwings: do_swings(o, fresh); break;
        case kZTarget: do_ztarget(o, fresh); break;
        case kSpin: case kShield: {
            stick_x_ = stick_y_ = 0;
            auto& g = g_virtual_gesture[0];
            if (fresh && !g.active() && sub_frames_ > 15) {
                if (count_ >= (act_ == kSpin ? 1 : 3)) { pick_next(o); break; }
                if (act_ == kSpin) g.spin(frand() < 0.5f); else g.nunchuk_flick();
                ++count_; ++st_.gestures; sub_frames_ = 0;
            }
            break;
        }
        case kCamera: do_camera(o, fresh); break;
        case kZTap:
            stick_x_ = stick_y_ = 0;
            if (sub_ == 0 && !tap_busy()) { tap(kNunZ); sub_ = 1; sub_frames_ = 0; }
            else if (sub_ == 1 && !tap_busy() && sub_frames_ > 20) pick_next(o);
            break;
        default: pick_next(o); break;
        }
    }

    void do_event(const GameObservation& o, uint64_t now, bool fresh) {
        stick_x_ = stick_y_ = 0; hold_ = 0; point_mode_ = 0;
        if (fresh) ++pulse_frames_;
        // A held 1.5 s of every 3 s (>= 2 game frames per edge), as the new-game replay; in a long event every 4th
        // pulse is B instead (a choice prompt or a menu that A does not close).
        const uint64_t phase = (now - pulse_ns_) % (3000 * kMs);
        const bool on = phase >= 1500 * kMs && pulse_frames_ >= 2;
        const uint64_t idx = (now - pulse_ns_) / (3000 * kMs);
        if (on) hold_ = (long_event_logged_ && idx % 4 == 3) ? kBtnB : kBtnA;
        // SOAK14, NWII_SS_SOAK_CHOICE=1: a full Nunchuk stick flick (0.3 s, 0.9-0.6 s before the A pulse; random
        // direction per flick). A dialog choice is selected by an FS-stick direction trigger (stick distance >= 0.8,
        // d_pad.cpp calcFSStickDirMask -> d_lyt_msg_window_select_btn.cpp handleButtonInput); with nothing selected
        // A does not confirm, so a choice dialog (the bird statue's 'Save') ran until the long-event B fallback
        // (155 s in G3). Text boxes and cutscenes ignore the stick, as for a player. (A per-pulse fixed direction
        // made every statue visit pick the same answers: save -> quit to title -> reload, forever, in G3b.)
        const bool flick = choice_ && pulse_frames_ >= 2 && phase >= 600 * kMs && phase < 900 * kMs;
        if (flick) {
            static const float dirs[4][2] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}};
            if (!flick_on_) { ++choice_flicks_; choice_salt_ = rnd() % 4u; }
            const float* d = dirs[choice_salt_ % 4u];
            stick_x_ = d[0]; stick_y_ = d[1];
        }
        flick_on_ = flick;
        if (event_running(o)) { idle_frames_ = 0; return; }
        if (fresh && ++idle_frames_ >= 20) {
            idle_frames_ = 0;
            char b[120];
            std::snprintf(b, sizeof b, "event over after %.1f s", (now - act_ns_) / 1e9);
            if (verbose_) log_line(b);
            if (choice_ && !verbose_) {
                std::snprintf(b, sizeof b, "event over after %.1f s (choice flicks so far %u)", (now - act_ns_) / 1e9,
                              choice_flicks_);
                log_line(b);
            }
            f_init_ = false; // the camera is placed anew after an event
            pick_next(o);
        }
    }

    void do_prompt(const GameObservation& o, bool fresh) {
        const bool dash = prompt_ == kPDash || prompt_ == kPEtcDash;
        if (!dash) { stick_x_ = stick_y_ = 0; } else wander(o, 1.0f, fresh);
        switch (sub_) {
        case 0:
            if (sub_frames_ >= (dash ? 1u : 4u)) {
                tap(kBtnA, prompt_hold_, dash ? 1500 : 170);
                ++st_.prompts_pressed;
                if (prompt_ == kPTalk) {
                    ++st_.talks;
                    talk_press_ns_ = now_;
                    talk_spots_[talk_i_++ % 4] = {o.pos[0], o.pos[2], now_};
                }
                sub_ = 1; sub_frames_ = 0;
            }
            break;
        case 1:
            if (!tap_busy() && sub_frames_ >= 30) pick_next(o);
            break;
        }
    }

    void do_swings(const GameObservation& o, bool fresh) {
        auto& g = g_virtual_gesture[0];
        if (sub_ == 0) { n_ = irand(4, 8); sub_ = 1; walk_swing_ = frand() < 0.4f; heading_ = frand() * 6.2831853f; }
        if (walk_swing_) wander(o, 0.4f, fresh); else stick_x_ = stick_y_ = 0;
        if (!fresh || g.active() || sub_frames_ < gap_) return;
        if (count_ >= n_) { pick_next(o); return; }
        const int d = irand(0, 8);
        static const float dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
        if (d == 8) g.thrust(); else g.swing(dirs[d][0], dirs[d][1]);
        // Swing evidence: requested direction and the virtual remote's roll, matched with the game's attack angle
        // (LINK +0x444C, stored at every swing start) in observe().
        swing_req_ns_ = now_; swing_req_d_ = d; swing_angle0_ = last_attack_angle_;
        {
            double mm[3][3];
            g_virtual_orientation[0].matrix(mm);
            swing_roll_ = float(std::atan2(mm[1][0], mm[1][1]) * 57.2958);
        }
        ++count_; ++st_.gestures;
        sub_frames_ = 0;
        gap_ = uint32_t(irand(6, 18));
    }

    void do_ztarget(const GameObservation& o, bool fresh) {
        (void)o;
        if (sub_ == 0) { dur_z_ = uint32_t(irand(90, 160)); sub_ = 1; }
        hold_ = act_frames_ < dur_z_ ? kNunZ : 0;
        const float s = ((act_frames_ / 30) % 2) ? 0.6f : -0.6f;
        stick_x_ = act_frames_ > 15 && act_frames_ < dur_z_ ? s : 0;
        stick_y_ = 0;
        auto& g = g_virtual_gesture[0];
        if (fresh && (act_frames_ == 40 || act_frames_ == 85) && !g.active()) {
            const float dx = frand() < 0.5f ? 1.0f : -1.0f, dy = frand() < 0.3f ? -1.0f : 0.0f;
            g.swing(dx, dy);
            ++st_.gestures;
        }
        if (act_frames_ >= dur_z_ + 20) pick_next(o);
    }

    void do_camera(const GameObservation& o, bool fresh) {
        stick_x_ = stick_y_ = 0;
        switch (sub_) {
        case 0:
            if (!tap_busy()) { cam_action0_ = o.current_action; tap(kNunC); sub_ = 1; sub_frames_ = 0; }
            break;
        case 1: // look around: the pointer target moves in a slow circle
            if (sub_frames_ == 25) cam_action1_ = o.current_action;
            if (sub_frames_ > 20) {
                point_mode_ = 2;
                const float a = (sub_frames_ - 20) * 0.06f;
                rs_x_ = 0.8f * std::cos(a) - 2.0f * cursor_x_ * 0.3f;
                rs_y_ = 0.8f * std::sin(a) - 2.0f * cursor_y_ * 0.3f;
            }
            if (sub_frames_ > 130) { rs_x_ = -cursor_x_ * 2.0f; rs_y_ = -cursor_y_ * 2.0f; }
            if (sub_frames_ > 160 && !tap_busy()) { tap(kNunC); sub_ = 2; sub_frames_ = 0; }
            break;
        case 2:
            rs_x_ = rs_y_ = 0;
            if (sub_frames_ > 30) {
                if (o.current_action == cam_action1_ && cam_action1_ != cam_action0_ && count_ == 0) {
                    ++count_; tap(kBtnB); // still in first person: B leaves it too
                    sub_frames_ = 0;
                    break;
                }
                if (verbose_) {
                    char b[120];
                    std::snprintf(b, sizeof b, "camera: action %d -> %d (C) -> %d", cam_action0_, cam_action1_,
                                  o.current_action);
                    log_line(b);
                }
                point_mode_ = 0;
                pick_next(o);
            }
            break;
        }
        (void)fresh;
    }

    // Menus: open (tap or hold), wait for the game's pointer, 3-5 pointer strokes through the INPUT13 target-cursor
    // model with the game's pointer read back after each, close (close button, then the alternative), check closed.
    void do_menu(const GameObservation& o, bool fresh, uint32_t open, bool held, uint32_t close, uint32_t alt) {
        stick_x_ = stick_y_ = 0;
        if (held) hold_ = sub_ <= 3 ? open : 0;
        switch (sub_) {
        case 0: // open
            ++st_.menus;
            opened_ = false;
            close_tries_ = 0;
            if (!held) tap(open);
            sub_ = 1; sub_frames_ = 0;
            break;
        case 1: // wait for the pointer (the item wheel and pouch may not use dCsGame_c's cursor)
            if (cursor()) {
                opened_ = true;
                ++st_.menus_opened;
                // INPUT13: the cursor starts where the game's pointer is (centre if off-screen).
                const float px = gptr_x_, py = gptr_y_;
                cursor_x_ = std::fabs(px) <= 1.0f ? px : 0.0f;
                cursor_y_ = std::fabs(py) <= 1.0f ? py : 0.0f;
                sub_ = 2; sub_frames_ = 0; n_ = irand(3, 5); count_ = 0;
            } else if (sub_frames_ > 75) {
                if (!held) {
                    char b[160];
                    std::snprintf(b, sizeof b, "%s: no menu pointer 2.5 s after the button (event %d, action %d)",
                                  act_name(act_), o.event_state, o.current_action);
                    log_line(b);
                }
                cursor_x_ = cursor_y_ = 0;
                sub_ = 2; sub_frames_ = 0; n_ = held ? irand(2, 3) : 1; count_ = 0;
            }
            break;
        case 2: // stroke: pick a target
            point_mode_ = 1;
            tx_ = (frand() * 2 - 1) * 0.75f;
            ty_ = (frand() * 2 - 1) * 0.65f;
            sub_ = 3; sub_frames_ = 0; settle_ = 0;
            break;
        case 3: { // move the virtual right stick toward the target, then settle and compare with the game pointer
            point_mode_ = 1;
            const float ex = tx_ - cursor_x_, ey = ty_ - cursor_y_, e = std::hypot(ex, ey);
            if (e > 0.03f && sub_frames_ < 60) {
                const float m = std::clamp(std::sqrt(e * 2.0f), 0.35f, 1.0f); // squared response: sqrt for linear feel
                rs_x_ = m * ex / e; rs_y_ = m * ey / e;
            } else {
                rs_x_ = rs_y_ = 0;
                if (fresh) ++settle_;
                if (settle_ >= 12) {
                    settle_ = 0;
                    ++st_.strokes;
                    const float gx = gptr_x_, gy = gptr_y_;
                    const float err = std::hypot(gx - cursor_x_, gy - cursor_y_);
                    const bool ok = opened_ && std::isfinite(err) && err < 0.15f;
                    if (opened_) {
                        ++st_.menu_strokes;
                        st_.stroke_err_max = std::max(st_.stroke_err_max, std::isfinite(err) ? err : 9.0f);
                        st_.stroke_err_sum += std::isfinite(err) ? err : 9.0f;
                        if (ok) ++st_.strokes_ok;
                    }
                    if (verbose_ || (opened_ && !ok)) {
                        char b[200];
                        std::snprintf(b, sizeof b, "%s pointer stroke %d: target (%.2f,%.2f) game pointer (%.2f,%.2f) "
                                      "err %.3f%s", act_name(act_), count_ + 1, cursor_x_, cursor_y_, gx, gy, err,
                                      opened_ ? (ok ? "" : " (MISS)") : " (no menu cursor)");
                        log_line(b);
                    }
                    if (++count_ >= n_) { sub_ = 4; sub_frames_ = 0; }
                    else sub_ = 2;
                }
            }
            break;
        }
        case 4: // close: release the held button / press the close button (alternating with the other one)
            point_mode_ = 1; rs_x_ = rs_y_ = 0;
            hold_ = 0;
            if (!opened_ && !cursor() && close_tries_ == 0) {
                // Never saw the menu: nothing to close (a close press could open it).
                if (held) { sub_ = 5; sub_frames_ = 0; close_tries_ = 1; break; }
                pick_next(o);
                break;
            }
            if (tap_busy()) break;
            if (!held || close_tries_ > 0) tap(close_tries_ % 2 == 0 && close ? close : alt);
            ++close_tries_;
            sub_ = 5; sub_frames_ = 0;
            break;
        case 5: // check closed; the pointer target goes back to the centre (the next gestures start level)
            point_mode_ = 1; rs_x_ = rs_y_ = 0;
            cursor_x_ = cursor_y_ = 0;
            if (!cursor() && sub_frames_ > 20) {
                if (opened_) ++st_.menus_closed;
                pick_next(o);
                break;
            }
            if (sub_frames_ > 45 && !tap_busy()) {
                if (close_tries_ >= 6) {
                    ++st_.menus_stuck;
                    char b[160];
                    std::snprintf(b, sizeof b, "MENU STUCK: %s pointer still shown after %d close presses; unstick",
                                  act_name(act_), close_tries_);
                    log_line(b);
                    start(kUnstick, o);
                    break;
                }
                sub_ = 4;
            }
            break;
        }
    }

    // Escalating recovery: close menus (B, +, 1, 2), advance dialogs (A), then walk off in 4 directions.
    void do_unstick(const GameObservation& o, bool fresh) {
        stick_x_ = stick_y_ = 0; hold_ = 0; point_mode_ = 0;
        static const uint32_t seq[] = {kBtnB, kBtnA, kBtnB, kBtnPlus, kBtnB, kBtnOne, kBtnB, kBtnTwo, kBtnB, kNunC, kBtnB};
        constexpr int nseq = int(sizeof(seq) / sizeof(seq[0]));
        if (cursor() || count_ < 2) {
            if (!tap_busy() && sub_frames_ > 30 && count_ < nseq) { tap(seq[count_++]); sub_frames_ = 0; }
            if (count_ >= nseq && sub_frames_ > 60) { pick_next(o); }
            return;
        }
        // No menu: four 1.5 s runs in different directions.
        if (n_ < 4) {
            if (sub_ == 0) { heading_ = frand() * 6.2831853f; sub_ = 1; sub_frames_ = 0; }
            steer_world(o, std::sin(heading_ + n_ * 1.5708f), std::cos(heading_ + n_ * 1.5708f), 1.0f, fresh);
            if (sub_frames_ > 45) { ++n_; sub_frames_ = 0; }
            return;
        }
        pick_next(o);
    }

    // ---------------------------------------------------------------- state
    bool active_ = false, verbose_ = false, explore_ = false;
    bool choice_ = false, flick_on_ = false; // NWII_SS_SOAK_CHOICE=1: stick flick before each event A pulse
    uint32_t choice_salt_ = 0, choice_flicks_ = 0;
    uint64_t cells_[512] = {};
    uint32_t cells_frames_[512] = {};
    uint32_t cells_n_ = 0;
    struct Blocked { uint64_t cell; int dir; };
    Blocked blocked_[64] = {};
    uint32_t blocked_i_ = 0;
    int frontier_k_ = 0;
    float route_[32][2] = {};
    int route_n_ = 0, route_i_ = 0, route_fail_ = 0, route_reached_ = 0;
    char route_stage_[16] = {};
    uint32_t seed_ = 14u, allowed_mask_ = ~0u;
    float tether_ = 2500.0f;
    uint64_t now_ = 0, start_ns_ = 0, minute_ns_ = 0, progress_ns_ = 0, stage_ns_ = 0, event_ns_ = 0, fall_ns_ = 0;
    uint64_t act_ns_ = 0, pulse_ns_ = 0, talk_press_ns_ = 0, ptrlog_ns_ = 0;
    uint32_t minute_frame_ = 0, minute_n_ = 0;
    Stats st_{};
    // observation memory
    char stage_[16] = {}, last_event_name_[32] = {};
    int room_ = -2, layer_ = -2, last_event_ = -3, last_prompt_logged_ = -1, last_prompt_eval_ = -1;
    uint32_t seen_prompt_[5] = {}, prompt_eval_frames_ = 0;
    bool last_cursor_ = false, long_event_logged_ = false;
    float prog_x_ = 0, prog_y_ = 0, prog_z_ = 0;
    float hx_[64] = {}, hy_[64] = {}, hz_[64] = {}, hsx_[64] = {}, hsy_[64] = {};
    uint32_t hn_ = 0;
    // home / steering
    bool home_valid_ = false, f_init_ = false;
    float home_x_ = 0, home_y_ = 0, home_z_ = 0, fx_ = 0, fz_ = 1, heading_ = 0, drift_ = 0.06f, side_ = 1;
    uint32_t recover_ = 0, move_hn_ = 0;
    // activity
    int act_ = kIdle, sub_ = 0, count_ = 0, n_ = 0, prompt_ = -1, close_tries_ = 0;
    uint32_t act_frames_ = 0, sub_frames_ = 0, dur_ = 0, dur_z_ = 0, act_stalls_ = 0, gap_ = 10, prompt_hold_ = 6;
    uint32_t idle_frames_ = 0, pulse_frames_ = 0, pulses_ = 0, stray_cursor_frames_ = 0, settle_ = 0, stuck_move_frames_ = 0;
    int cam_action0_ = -1, cam_action1_ = -1;
    bool walk_swing_ = false, opened_ = false;
    float tx_ = 0, ty_ = 0, gptr_x_ = 0, gptr_y_ = 0, swing_roll_ = 0;
    int swing_req_d_ = -1;
    int16_t swing_angle0_ = 0, last_attack_angle_ = 0;
    uint64_t swing_req_ns_ = 0;
    uint64_t prompt_used_ns_[160] = {};
    struct TalkSpot { float x, z; uint64_t ns; };
    TalkSpot talk_spots_[4] = {};
    uint32_t talk_i_ = 0;
    // outputs
    float stick_x_ = 0, stick_y_ = 0, rs_x_ = 0, rs_y_ = 0, cursor_x_ = 0, cursor_y_ = 0;
    int point_mode_ = 0; // 0 none, 1 menu cursor, 2 first-person look
    uint32_t hold_ = 0, cur_buttons_ = 0;
    uint32_t tap_mask_ = 0, tap_frames_ = 0, tap_gap_ = 0;
    uint64_t tap_until_ = 0;
    bool tap_on_ = false;
};

}
