#pragma once
#include "input/input_source.h"
#include "../virtual_motion.h"
#include "../motion_gesture.h"
#include "../game_observation.h"
#include "soak_script.h" // SOAK14: long soak/exploration script (NWII_SS_REPLAY_SOAK=1)
#include "ctrl18_script.h" // CTRL18-INPUT: controls live script (NWII_SS_REPLAY_CTRL18=1)
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace nwii::runtime::input {

// Read-only observations published by the SOUE01 adapter (ss_boot.cpp,
// SS_KPADReadEx, once per game frame) and used only by the explicit
// new-game test replay below: the dTitleMgr_c phase and the game's own
// pointer, dPad::ex_c::m_current_ex->pos (the value the keyboard REL feeds
// to textinput, fn_2_BB0: layout x = pos.x*406, y = -pos.y*228 on 812x456).
enum ReplayTitlePhase : int { kPhaseUnknown = 0, kPhaseOther = 1, kPhaseFileSelect = 2, kPhaseInput = 3 };
inline std::atomic<int> g_replay_title_phase{kPhaseUnknown};
inline std::atomic<float> g_replay_pointer_x{0.0f};
inline std::atomic<float> g_replay_pointer_y{0.0f};
inline std::atomic<uint32_t> g_replay_pointer_frames{0};

// Explicit test input, selected only by NWII_SS_INPUT_REPLAY=boot, boot-title or new-game. It
// generates ordinary controller samples, never guest calibration or hardware flags.
//
// new-game: the boot-title A pulses until the title manager reaches StateID_Input (name entry).
// Then the virtual MotionPlus remote is turned (gyro rates only; the sensor bar is reported
// as not visible) so that the game's OWN MotionPlus pointer (dPad::ex_c, read back above)
// lands on on-screen keys, and A is pressed there: NWII_SS_REPLAY_CLEAR backspaces (default 8),
// then the characters of NWII_SS_REPLAY_NAME (default "Link"; Shift before an upper-case
// letter), then OK, then A pulses every 3 s for the following dialogs. Moves use a
// raised-cosine rate profile (never a steady rate, which the original KMPLS zero-drift
// compensation would absorb), sized open-loop from the pointer model of virtual_motion.h and
// corrected closed-loop from the read-back pointer. Key centres are 640x480 screenshot
// coordinates (pos = ((x-320)/320, (y-240)/240)); NWII_SS_REPLAY_TARGET=x,y overrides OK.
//
// new-game-long (PROG11, roadmap M3.9): the same title/name-entry/intro script, then instead of the
// controls test a progression director (long_script below) walks Link out of his room and the Knight
// Academy into Skyloft (F000), advancing dialogs/cutscenes with A pulses whenever an event runs, and
// logs every stage/event/route step, stalls and fps per stage as "[Prog]" lines. Selected by
// NWII_SS_REPLAY_LONG=1 together with NWII_SS_INPUT_REPLAY=new-game (probe.py --input-replay
// new-game-long sets both), or Mode::NewGameLong / NWII_SS_INPUT_REPLAY=new-game-long once the input
// manager and the ss_boot observation publishers accept that name. 'new-game' alone is unchanged.
//
// load-save (LOAD14, roadmap M6.3; NWII_SS_INPUT_REPLAY=load-save): the boot-title A pulses until the title manager
// reaches StateID_FileSelect, then (A released) the three save slots are read from the game's own FileManager
// (game_observation.h, read-only) and the USED slot (new_file == 0) with the most recent saved_time is chosen; it
// is never a "New Adventure" slot, and with no used slot the replay idles. The MotionPlus pointer is turned onto
// that slot's feather (same closed-loop pointer moves as the keyboard script) and A is pressed, then onto each
// confirmation target in turn (NWII_SS_REPLAY_LOAD_TARGETS="x,y;x,y", 640x480 screenshot coordinates; the slot
// feathers can be overridden with NWII_SS_REPLAY_SLOT_TARGETS). A pulses follow until the title is gone and Link is
// controllable; then "[Load] LOADED" reports stage/room/layer/entrance, position, hearts, rupees and sword next to
// the values in the chosen slot ("MATCH"/"MISMATCH"). With NWII_SS_REPLAY_SAVE=1 Link then walks to the nearest
// bird statue (saveObj placements from the stage data), faces it, presses A and advances the statue dialog with A
// pulses; "[Load] SAVED" is logged when the game itself rewrites the slot (saved_time changes in its save buffer).
class BootReplaySource final : public IInputSource {
public:
    enum class Mode { Boot, BootTitle, NewGame, NewGameLong, LoadSave };

    explicit BootReplaySource(Mode replay_mode = Mode::Boot)
        : horizon_ns(replay_mode == Mode::Boot ? 120'000'000'000ull : 300'000'000'000ull),
          title_mode(replay_mode != Mode::Boot),
          mode(replay_mode == Mode::NewGameLong ? Mode::NewGame : replay_mode),
          long_mode(replay_mode == Mode::NewGameLong || (replay_mode == Mode::NewGame && long_requested())) {
        if (mode == Mode::LoadSave) {
            const char* e = std::getenv("NWII_SS_REPLAY_SAVE");
            ld.save_requested = e && e[0] && e[0] != '0';
            std::cout << "[InputReplay] load-save: boot-title A pulses until the file select, then the most recently "
                         "saved slot is loaded" << (ld.save_requested ? "; afterwards a save at the nearest bird statue"
                                                                       : "") << "; see [Load] lines.\n";
            return;
        }
        if (mode != Mode::NewGame) return;
        if (long_mode)
            std::cout << "[InputReplay] new-game-long: after name entry a progression director walks Link out of "
                         "the Knight Academy (route + A pulses during events); see [Prog] lines.\n";
        int clear = 8;
        std::string name = "Link";
        float ok_x = 525.0f, ok_y = 397.0f;
        if (const char* e = std::getenv("NWII_SS_REPLAY_CLEAR")) clear = std::clamp(std::atoi(e), 0, 16);
        if (const char* e = std::getenv("NWII_SS_REPLAY_NAME")) name = e;
        if (const char* e = std::getenv("NWII_SS_REPLAY_TARGET")) {
            float x = 0, y = 0;
            if (std::sscanf(e, "%f,%f", &x, &y) == 2) { ok_x = x; ok_y = y; }
        }
        for (int i = 0; i < clear; ++i) actions.push_back({568.0f, 153.0f, "backspace"});
        for (char ch : name) {
            float x = 0, y = 0;
            if (!key_position(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))), x, y)) {
                std::cout << "[InputReplay] new-game: no key for '" << ch << "', skipped\n";
                continue;
            }
            if (std::isupper(static_cast<unsigned char>(ch))) actions.push_back({80.0f, 265.0f, "Shift"});
            actions.push_back({x, y, std::string(1, ch)});
        }
        actions.push_back({ok_x, ok_y, "OK"});
    }
    // Compatibility with the original boolean constructor.
    explicit BootReplaySource(bool title_test) : BootReplaySource(title_test ? Mode::BootTitle : Mode::Boot) {}

    void update(GameCubePadState pads[4], WiimoteState motes[4]) override {
        auto& mote = motes[0];
        const uint64_t now = mote.motion.monotonic_ns;
        if (!started || now < started_ns) {
            started = true;
            started_ns = now;
        }
        const uint64_t elapsed = now - started_ns;
        float pitch = 0.0f, yaw = 0.0f;
        bool a_pressed;
        if ((mode == Mode::NewGame && (input_started || g_replay_title_phase.load() == kPhaseInput)) ||
            (mode == Mode::LoadSave && (input_started || g_replay_title_phase.load() == kPhaseFileSelect))) {
            if (!input_started) {
                input_started = true;
                stage_ns = now;
                if (mode == Mode::LoadSave)
                    std::cout << "[InputReplay] load-save: file select reached at elapsed_ms=" << elapsed / 1'000'000
                              << "; A released, slot choice after the settle time\n";
                else
                    std::cout << "[InputReplay] new-game: name entry reached at elapsed_ms="
                              << elapsed / 1'000'000 << "; keyboard script starts (" << actions.size()
                              << " pointer actions)\n";
            }
            a_pressed = name_entry_script(now, mote.motion.dt_seconds, pitch, yaw);
        } else {
            const uint64_t phase = elapsed % 3'000'000'000;
            // Preserve the original boot/calibration timeline. Later title-test
            // pulses last a second so low guest frame rates can observe the edge.
            const uint64_t pulse_end = title_mode && elapsed >= 120'000'000'000
                ? 3'000'000'000 : 2'400'000'000;
            a_pressed = elapsed < horizon_ns && phase >= 2'000'000'000 && phase < pulse_end;
            if (!horizon_logged && elapsed >= horizon_ns) {
                horizon_logged = true;
                std::cout << "[InputReplay] A pulses stopped after " << horizon_ns / 1'000'000'000
                          << " seconds; neutral samples continue.\n";
            }
        }
        if (a_pressed != previous_a_pressed) {
            std::cout << "[InputReplay] A " << (a_pressed ? "press" : "release")
                      << " sample=" << std::dec << mote.motion.sequence << " elapsed_ms=" << elapsed / 1'000'000 << '\n';
            previous_a_pressed = a_pressed;
        }
        mote.err = 0;
        mote.buttons = a_pressed ? 0x0800 : 0;
        // While the script steers the MotionPlus pointer the sensor bar is reported as not
        // visible (DPD invalid), so KPAD's DPD revise cannot pull the direction back to a
        // fixed IR centre; boot/boot-title keep the centred, valid IR pointer.
        mote.ir_x = mote.ir_y = input_started ? -1.0f : 0.5f;
        mote.gyro_pitch = mote.gyro_yaw = mote.gyro_roll = 0.0f;
        mote.nunchuk = {};
        mote.nunchuk.available = true;
        mote.nunchuk.stick_x = stick_x;
        mote.nunchuk.stick_y = stick_y;
        // Gravity comes from the virtual orientation (InputManager, virtual_motion.h).
        merge_virtual_motion(mote.motion, true, pitch, yaw, 0.0f);
        soak_.apply(mote, g_replay_pointer_x.load(), g_replay_pointer_y.load()); // SOAK14 (no-op until the soak runs)
        ctrl18_.apply(mote); // CTRL18-INPUT (no-op until the controls script runs)
        pads[0].err = 0;
        pads[0].buttons = a_pressed ? 0x0100 : 0;
    }

private:
    enum class Stage { Settle, Plan, Move, Wait, Press, Release, After };
    struct Action { float x, y; std::string label; };
    static constexpr uint64_t kMs = 1'000'000ull;
    static constexpr int kMaxMoves = 8;
    static constexpr float kTolX = 0.03f, kTolY = 0.035f; // ~10 px / 8 px on 640x480

    static bool key_position(char ch, float& x, float& y) {
        static const char* rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
        static const float row_x[] = {52.0f, 75.0f, 143.0f, 155.0f};
        static const float row_y[] = {153.0f, 190.0f, 228.0f, 265.0f};
        for (int r = 0; r < 4; ++r)
            for (int i = 0; rows[r][i]; ++i)
                if (rows[r][i] == ch) { x = row_x[r] + 45.6f * i; y = row_y[r]; return true; }
        if (ch == ' ') { x = 342.0f; y = 302.0f; return true; }
        return false;
    }

    void next(Stage s, uint64_t now) { stage = s; stage_ns = now; }

    static void pointer(float& x, float& y, uint32_t& frames) {
        x = g_replay_pointer_x.load();
        y = g_replay_pointer_y.load();
        frames = g_replay_pointer_frames.load();
    }

    const char* tag() const { return mode == Mode::LoadSave ? "load-save" : "new-game"; }

    void log_stage(const char* what) {
        float x, y; uint32_t f;
        pointer(x, y, f);
        std::printf("[InputReplay] %s %s pointer=(%.3f,%.3f) screen=(%.0f,%.0f) frame#%u\n", tag(), what, x, y,
                    320.0f + 320.0f * x, 240.0f + 240.0f * y, f);
        std::fflush(stdout);
    }

    // At least `ns` and `frames` fresh game frames (pointer publications) since the stage began.
    bool waited(uint64_t now, uint64_t ns, uint32_t frames) const {
        return now - stage_ns >= ns && g_replay_pointer_frames.load() >= stage_frames + frames;
    }
    void next_counted(Stage s, uint64_t now) { next(s, now); stage_frames = g_replay_pointer_frames.load(); }

    // Raised-cosine rotation of (move_pitch, move_yaw) radians over move_duration seconds,
    // tracking the integrated angle exactly (only producer updates with a valid dt count).
    bool move_step(float dt, float& pitch, float& yaw) {
        if (!(dt > 0.0f) || dt > 0.1f) return false; // not integrated by the producer
        const double t0 = move_t, t1 = std::min<double>(move_t + dt, move_duration);
        move_t += dt;
        // Integral of (1 - cos(2*pi*t/D)) / D over [t0, t1].
        const double k = 2.0 * 3.14159265358979323846 / move_duration;
        const double frac = ((t1 - t0) - (std::sin(k * t1) - std::sin(k * t0)) / k) / move_duration;
        double want_pitch = move_pitch * frac, want_yaw = move_yaw * frac;
        if (move_t >= move_duration) { // land exactly on the planned angle
            want_pitch = move_pitch - done_pitch;
            want_yaw = move_yaw - done_yaw;
        }
        const double cap = kPointerMotionMaxRateRadS * dt;
        want_pitch = std::clamp(want_pitch, -cap, cap);
        want_yaw = std::clamp(want_yaw, -cap, cap);
        pitch = static_cast<float>(want_pitch / dt);
        yaw = static_cast<float>(want_yaw / dt);
        done_pitch += pitch * dt;
        done_yaw += yaw * dt;
        return move_t >= move_duration &&
               std::abs(move_pitch - done_pitch) < 1e-4 && std::abs(move_yaw - done_yaw) < 1e-4;
    }

    bool name_entry_script(uint64_t now, float dt, float& pitch, float& yaw) {
        switch (stage) {
        case Stage::Settle:
            if (now - stage_ns >= 4000 * kMs) {
                if (mode == Mode::LoadSave && !build_load_actions(now)) {
                    if (!ld.nothing) return false; // FileManager not read yet: keep waiting
                    next_counted(Stage::After, now);
                    return false;
                }
                log_stage("start"); next(Stage::Plan, now); moves = 0;
            }
            return false;
        case Stage::Plan: {
            if (action >= actions.size()) { log_stage("all actions done"); next_counted(Stage::After, now); return false; }
            const Action& a = actions[action];
            const float tx = (a.x - 320.0f) / 320.0f, ty = (a.y - 240.0f) / 240.0f;
            float x, y; uint32_t f;
            pointer(x, y, f);
            // Refine the gains from the previous move (sign and scale), within sane bounds.
            if (measuring) {
                measuring = false;
                const float dx = x - measure_x, dy = y - measure_y;
                if (std::abs(measure_yaw) > 0.03f && std::abs(dx) > 0.02f && std::abs(x) < 0.97f) {
                    const float g = dx / measure_yaw;
                    if (std::abs(g) > 0.3f * kModelGain && std::abs(g) < 3.0f * kModelGain) gain_x = g;
                }
                if (std::abs(measure_pitch) > 0.03f && std::abs(dy) > 0.02f && std::abs(y) < 0.97f) {
                    const float g = dy / measure_pitch;
                    if (std::abs(g) > 0.3f * kModelGain && std::abs(g) < 3.0f * kModelGain) gain_y = g;
                }
                std::printf("[InputReplay] %s measured: d=(%.3f,%.3f) for yaw %.3f pitch %.3f rad -> "
                            "gain x/yaw %.3f, y/pitch %.3f (model %.3f, %.3f)\n", tag(), dx, dy, measure_yaw,
                            measure_pitch, gain_x, gain_y, -kModelGain, kModelGain);
            }
            const float ex = tx - x, ey = ty - y;
            if ((std::abs(ex) < kTolX && std::abs(ey) < kTolY) || moves >= kMaxMoves) {
                std::printf("[InputReplay] %s key '%s' at (%.0f,%.0f): %s\n", tag(), a.label.c_str(), a.x, a.y,
                            moves >= kMaxMoves && !(std::abs(ex) < kTolX && std::abs(ey) < kTolY)
                                ? "move budget used, pressing A anyway" : "on target, pressing A");
                log_stage("press");
                next_counted(Stage::Press, now);
                return true;
            }
            ++moves;
            move_yaw = ex / gain_x;
            move_pitch = ey / gain_y;
            const float angle = std::sqrt(move_yaw * move_yaw + move_pitch * move_pitch);
            move_duration = std::clamp(0.25f + 0.8f * angle, 0.3f, 0.75f);
            move_t = 0; done_pitch = done_yaw = 0;
            measuring = true; measure_x = x; measure_y = y; measure_yaw = move_yaw; measure_pitch = move_pitch;
            std::printf("[InputReplay] %s move %d to '%s' (%.0f,%.0f): pointer (%.3f,%.3f) target (%.3f,%.3f) "
                        "turn yaw %.4f pitch %.4f rad over %.2f s\n", tag(), moves, a.label.c_str(), a.x, a.y, x, y, tx,
                        ty,
                        move_yaw, move_pitch, move_duration);
            std::fflush(stdout);
            next(Stage::Move, now);
            return false;
        }
        case Stage::Move:
            if (move_step(dt, pitch, yaw)) next_counted(Stage::Wait, now);
            return false;
        case Stage::Wait:
            // The KMPLS consumes the move on the next KPAD read; dPad publishes the pointer
            // computed from it one read later.
            if (waited(now, 300 * kMs, 3)) next(Stage::Plan, now);
            return false;
        case Stage::Press:
            if (waited(now, 400 * kMs, 3)) next_counted(Stage::Release, now);
            return true;
        case Stage::Release:
            if (waited(now, 400 * kMs, 3)) { ++action; moves = 0; next(Stage::Plan, now); }
            return false;
        case Stage::After: {
            // Following dialogs and cutscenes: A pulses (>= 3 game frames each), pointer left in
            // place, until the player is controllable in Link's room (gameplay_script).
            const int phase = g_replay_title_phase.load();
            if (phase != last_phase) {
                last_phase = phase;
                std::printf("[InputReplay] %s title phase now %d (2 = StateID_FileSelect, 3 = StateID_Input)\n", tag(),
                            phase);
                std::fflush(stdout);
            }
            return gameplay_script(now);
        }
        }
        return false;
    }

    // ---- after name entry: progression to a controllable Link, then the controls test ----
    // Progression: A pulses every 3 s (1.5 s held, >= 2 game frames) advance the intro, the save
    // prompt and the Skyloft / Link's room cutscene dialogs. "Controllable" is read from the game's
    // own state (game_observation.h, read-only): stage NWII_SS_REPLAY_GOAL_STAGE (default F001r),
    // dAcPy_c::LINK present and EventManager idle (mState 0) for 45 consecutive game frames. Then the
    // A pulses stop. With NWII_SS_REPLAY_CONTROLS != 0 (default on) a scripted controls test follows:
    // idle negative-control window, walking with the Nunchuk stick, sword gestures (motion_gesture.h)
    // and a Nunchuk flick, each logged as "[CtrlTest]" with the observed player state.
    struct CtrlStep { const char* name; double seconds; float sx, sy; int gesture; double gx, gy; };
    enum Gesture { kNone, kSwing, kThrust, kFlick, kSpin, kRaise, kLower, kRecenter };
    static const std::vector<CtrlStep>& ctrl_steps() {
        static const std::vector<CtrlStep> steps = {
            {"settle + point forward", 3.0, 0, 0, kRecenter, 0, 0},
            {"negative-control idle", 4.0, 0, 0, kNone, 0, 0},
            {"walk forward", 2.5, 0, 1, kNone, 0, 0},
            {"stop", 1.5, 0, 0, kNone, 0, 0},
            {"walk right", 1.5, 1, 0, kNone, 0, 0},
            {"stop", 1.5, 0, 0, kNone, 0, 0},
            {"swing right (draw)", 3.0, 0, 0, kSwing, 1, 0},
            {"swing right", 3.0, 0, 0, kSwing, 1, 0},
            {"swing left", 3.0, 0, 0, kSwing, -1, 0},
            {"swing down", 3.0, 0, 0, kSwing, 0, -1},
            {"swing up", 3.0, 0, 0, kSwing, 0, 1},
            {"swing down-right", 3.0, 0, 0, kSwing, 1, -1},
            {"swing up-left", 3.0, 0, 0, kSwing, -1, 1},
            {"thrust", 3.0, 0, 0, kThrust, 0, 0},
            {"nunchuk flick", 3.0, 0, 0, kFlick, 0, 0},
            {"spin", 3.5, 0, 0, kSpin, 0, 0},
            {"raise", 2.5, 0, 0, kRaise, 0, 0},
            {"swing down from raised", 3.0, 0, 0, kSwing, 0, -1},
            {"lower", 2.0, 0, 0, kLower, 0, 0},
            {"walk forward 2", 2.0, 0, 1, kNone, 0, 0},
            {"negative-control idle 2", 4.0, 0, 0, kNone, 0, 0},
        };
        return steps;
    }
    struct CtrlRecord {
        uint32_t frames = 0, attack_dirs = 0, specific = 0, remote_motion = 0, nunchuk_motion = 0, sword_or = 0;
        int actions[8] = {};
        int n_actions = 0;
        float pos0[3] = {0, 0, 0}, pos1[3] = {0, 0, 0};
        bool have_pos = false;
        float swing_mag = 0, swing_deg = 0; // strongest player swing vector (+0x4608/+0x460C) and its angle
        bool angle_init = false, angle_changed = false; // attack angle +0x444C (applyAttackDirection)
        int16_t angle_start = 0, angle_new = 0;
    };
    void ctrl_observe(const GameObservation& o) {
        if (o.frame == ctrl_last_frame) return;
        ctrl_last_frame = o.frame;
        CtrlRecord& r = ctrl_record;
        ++r.frames;
        if (o.attack_direction) r.attack_dirs |= 1u << (o.attack_direction & 31);
        r.specific |= o.specific_attack_direction;
        r.remote_motion |= o.remote_motion;
        r.nunchuk_motion |= o.nunchuk_motion;
        r.sword_or |= o.sword_states;
        bool seen = false;
        for (int i = 0; i < r.n_actions; ++i) seen = seen || r.actions[i] == o.current_action;
        if (!seen && r.n_actions < 8) r.actions[r.n_actions++] = o.current_action;
        // Player swing vector angle as fn_8021C0B0 forms it, atan2s(v.y, -v.x): 0 up, 90 left, -90 right, 180 down.
        const float mag = std::sqrt(o.swing[0] * o.swing[0] + o.swing[1] * o.swing[1]);
        if (mag > r.swing_mag) {
            r.swing_mag = mag;
            r.swing_deg = std::atan2(o.swing[1], -o.swing[0]) * 57.2957795f;
        }
        if (!r.angle_init) { r.angle_init = true; r.angle_start = o.attack_angle; }
        else if (o.attack_angle != r.angle_start && !r.angle_changed) { r.angle_changed = true; r.angle_new = o.attack_angle; }
        if (!r.have_pos) { std::copy(o.pos, o.pos + 3, r.pos0); r.have_pos = true; }
        std::copy(o.pos, o.pos + 3, r.pos1);
    }
    void ctrl_report(const CtrlStep& st) {
        const CtrlRecord& r = ctrl_record;
        const float dx = r.pos1[0] - r.pos0[0], dy = r.pos1[1] - r.pos0[1], dz = r.pos1[2] - r.pos0[2];
        std::printf("[CtrlTest] %-24s frames=%u attack_dirs=0x%X specific=0x%X remote=0x%02X nunchuk=0x%02X "
                    "sword_or=0x%X actions=", st.name, r.frames, r.attack_dirs, r.specific, r.remote_motion,
                    r.nunchuk_motion, r.sword_or);
        for (int i = 0; i < r.n_actions; ++i) std::printf("%s%d", i ? "," : "", r.actions[i]);
        if (r.angle_changed) std::printf(" attack_angle=%.0fdeg", r.angle_new * (360.0 / 65536.0));
        else std::printf(" attack_angle=none");
        std::printf(" swing_vec=%.2f@%.0fdeg move=(%.1f,%.1f,%.1f) dist=%.1f\n", r.swing_mag, r.swing_deg, dx, dy, dz,
                    std::sqrt(dx * dx + dz * dz));
        std::fflush(stdout);
    }
    bool gameplay_script(uint64_t now) {
        if (mode == Mode::LoadSave) return load_after_script(now);
        if (long_mode) return long_script(now);
        const GameObservation o = g_game_observation.get();
        if (!controllable) {
            static const std::string goal = [] {
                const char* e = std::getenv("NWII_SS_REPLAY_GOAL_STAGE");
                return std::string(e && e[0] ? e : "F001r");
            }();
            if (o.frame != ctrl_last_frame) {
                ctrl_last_frame = o.frame;
                const bool idle = o.link_present && o.event_state == 0 && goal == o.stage;
                idle_frames = idle ? idle_frames + 1 : 0;
                if (idle_frames >= 45) {
                    controllable = true;
                    ctrl_ns = now;
                    std::printf("[InputReplay] new-game: Link controllable in %s room=%d layer=%d at elapsed_ms=%llu "
                                "frame=%u pos=(%.1f,%.1f,%.1f) action=%d; A pulses stop\n", o.stage, o.room, o.layer,
                                static_cast<unsigned long long>((now - started_ns) / kMs), o.frame, o.pos[0], o.pos[1],
                                o.pos[2], o.current_action);
                    std::fflush(stdout);
                }
            }
            if (!controllable) {
                const uint32_t f = g_replay_pointer_frames.load() - stage_frames;
                return (now - stage_ns) % (3000 * kMs) >= 1500 * kMs && f >= 2;
            }
        }
        static const bool run_controls = [] {
            const char* e = std::getenv("NWII_SS_REPLAY_CONTROLS");
            return !(e && e[0] == '0');
        }();
        if (!run_controls || ctrl_index >= ctrl_steps().size()) { stick_x = stick_y = 0; return false; }
        const CtrlStep& st = ctrl_steps()[ctrl_index];
        if (!ctrl_step_started) {
            ctrl_step_started = true;
            ctrl_ns = now;
            ctrl_record = CtrlRecord{};
            auto& g = g_virtual_gesture[0];
            switch (st.gesture) {
            case kSwing: g.swing(st.gx, st.gy); break;
            case kThrust: g.thrust(); break;
            case kFlick: g.nunchuk_flick(); break;
            case kSpin: g.spin(false); break;
            case kRaise: g.set_raised(true); break;
            case kLower: g.set_raised(false); break;
            case kRecenter: g.recenter(); break;
            default: break;
            }
            std::printf("[CtrlTest] begin %-24s at elapsed_ms=%llu frame=%u\n", st.name,
                        static_cast<unsigned long long>((now - started_ns) / kMs), o.frame);
            std::fflush(stdout);
        }
        ctrl_observe(o);
        stick_x = st.sx;
        stick_y = st.sy;
        if (now - ctrl_ns >= static_cast<uint64_t>(st.seconds * 1e9) && ctrl_record.frames >= 10) {
            ctrl_report(st);
            ctrl_step_started = false;
            if (++ctrl_index >= ctrl_steps().size()) {
                stick_x = stick_y = 0;
                std::printf("[CtrlTest] done\n");
                std::fflush(stdout);
            }
        }
        return false;
    }

    // ---- new-game-long: progression director (PROG11, roadmap M3.9) ----
    // Reads only the read-only observation (game_observation.h): stage/room/layer/entrance, Link's position and
    // facing, EventManager state/name. Output: Nunchuk stick + A, like a player. Nothing is written to the guest.
    static bool long_requested() {
        const char* e = std::getenv("NWII_SS_REPLAY_LONG");
        const char* r = std::getenv("NWII_SS_INPUT_REPLAY");
        return (e && e[0] && e[0] != '0') || (r && std::strcmp(r, "new-game-long") == 0);
    }

public:
    enum StepKind { kGoto, kDoor };
    // Goto: walk to (x,z) until within r. Door: stand at (x,z) + 70*(nx,nz), n = unit vector from the door actor
    // toward the approach side (inside dAcOdoor_c's interaction box |local x| <= 80, |local z| <= 100,
    // d_a_obj_door.cpp isPlayerInteractable), face the door, press A (OPEN interaction -> DoorPull/DoorPush event)
    // and require Link on the far side afterwards, or a stage change.
    struct RouteStep { const char* stage; StepKind kind; float x, z, r, nx, nz; const char* label; };
    static const std::vector<RouteStep>& route() {
        // F001r (Knight Academy), from Link's room (room 1, entrance 5 at (216,-2744)). Doors: Stage/F001r room 0
        // room.bzs layer 0 OBJS "Door": (791,0,-2183) angY -158 deg = Link's room; the exit leaf (2910,51,-2010)
        // angY 0, p1 0x00000003 -> SCEN 0 = F000 entrance 4. Waypoints: straight segments free with 30 units of
        // clearance on the room 0/1 KCL raster (work/prog11/kcl.py + astar.py, 25-unit steps allowed).
        static const std::vector<RouteStep> steps = {
            {"F001r", kGoto, 750.0f, -2290.0f, 45.0f, 0, 0, "Link's room, before the door"},
            {"F001r", kDoor, 791.0f, -2183.0f, 0, -0.3746f, -0.9272f, "Link's room door"},
            {"F001r", kGoto, 1000.0f, -2110.0f, 70.0f, 0, 0, "hallway"},
            {"F001r", kGoto, 2590.0f, -2250.0f, 70.0f, 0, 0, "hallway east, foot of the steps"},
            {"F001r", kGoto, 2910.0f, -2095.0f, 35.0f, 0, 0, "before the academy exit"},
            {"F001r", kDoor, 2910.0f, -2010.0f, 0, 0.0f, -1.0f, "Knight Academy exit door (to Skyloft F000)"},
            // F000 (Skyloft) after the stage intro, from the academy's ground-floor exit (entrance 4, (-4573,1238,-7802)):
            // one loop over the academy plateau (y 1238) for soak coverage. The plateau's south slope is closed by the
            // DmtGate actor at (-5787,1238,-5786) at this point of the story (run3: 8 stalls there), and the next story
            // step (ZeldaMeet2, NpcZld on the statue island) has no walkable connection from here on the collision
            // raster. Waypoints: 2.5D A* over the room KCL + F000Megami.dzb restricted to the plateau north of the gate
            // (work/prog11/nav25.py, 50 units clearance, steps <= 32), merged when < 150 apart.
            {"F000", kGoto, -5850.0f, -6275.0f, 90.0f, 0, 0, "Skyloft plateau loop 1/21"},
            {"F000", kGoto, -7350.0f, -6875.0f, 90.0f, 0, 0, "Skyloft plateau loop 2/21"},
            {"F000", kGoto, -7550.0f, -6925.0f, 90.0f, 0, 0, "Skyloft plateau loop 3/21"},
            {"F000", kGoto, -8100.0f, -7225.0f, 90.0f, 0, 0, "Skyloft plateau loop 4/21"},
            {"F000", kGoto, -6975.0f, -7250.0f, 90.0f, 0, 0, "Skyloft plateau loop 5/21"},
            {"F000", kGoto, -5475.0f, -7375.0f, 90.0f, 0, 0, "Skyloft plateau loop 6/21"},
            {"F000", kGoto, -4075.0f, -7375.0f, 90.0f, 0, 0, "Skyloft plateau loop 7/21"},
            {"F000", kGoto, -3775.0f, -7550.0f, 90.0f, 0, 0, "Skyloft plateau loop 8/21"},
            {"F000", kGoto, -3200.0f, -9050.0f, 90.0f, 0, 0, "Skyloft plateau loop 9/21"},
            {"F000", kGoto, -3150.0f, -9325.0f, 90.0f, 0, 0, "Skyloft plateau loop 10/21"},
            {"F000", kGoto, -3150.0f, -9775.0f, 90.0f, 0, 0, "Skyloft plateau loop 11/21"},
            {"F000", kGoto, -4050.0f, -11275.0f, 90.0f, 0, 0, "Skyloft plateau loop 12/21"},
            {"F000", kGoto, -4625.0f, -12525.0f, 90.0f, 0, 0, "Skyloft plateau loop 13/21"},
            {"F000", kGoto, -3125.0f, -11675.0f, 90.0f, 0, 0, "Skyloft plateau loop 14/21"},
            {"F000", kGoto, -1600.0f, -10975.0f, 90.0f, 0, 0, "Skyloft plateau loop 15/21"},
            {"F000", kGoto, -2800.0f, -9475.0f, 90.0f, 0, 0, "Skyloft plateau loop 16/21"},
            {"F000", kGoto, -3625.0f, -7925.0f, 90.0f, 0, 0, "Skyloft plateau loop 17/21"},
            {"F000", kGoto, -3900.0f, -7450.0f, 90.0f, 0, 0, "Skyloft plateau loop 18/21"},
            {"F000", kGoto, -4075.0f, -7375.0f, 90.0f, 0, 0, "Skyloft plateau loop 19/21"},
            {"F000", kGoto, -4450.0f, -7450.0f, 90.0f, 0, 0, "Skyloft plateau loop 20/21"},
            {"F000", kGoto, -4600.0f, -7525.0f, 90.0f, 0, 0, "Skyloft plateau loop 21/21"},
        };
        return steps;
    }

private:
    enum DoorSub { kApproach, kFace, kPress, kThrough };
    struct LongState {
        bool init = false, entered = false, goal_logged = false, paused = false, nan_logged = false, f_init = false;
        uint32_t last_frame = 0, idle = 0;
        char stage[16] = {}, ev_name[32] = {};
        int room = -2, layer = -2, entrance = -2, event = -3;
        uint64_t stage_ns = 0, beat_ns = 0, pulse_ns = 0;
        uint32_t stage_frame = 0, beat_frame = 0, pulse_frame = 0;
        // per fresh game frame: Link (x,z) and the stick command in effect
        float hx[64] = {}, hz[64] = {}, csx[64] = {}, csy[64] = {};
        uint32_t hn = 0;
        float fx = 0, fz = 1; // camera forward estimate (unit, world x/z): stick (sx,sy) moves Link along F*(sy + i*sx)
        size_t step = 0;
        int sub = kApproach, stuck = 0, attempts = 0, presses = 0, recover = 0;
        float side = 1, lateral = 0, face_cmd = 0;
        int face_ok = 0;
        uint32_t step_frames = 0, move_hn = 0, sub_frame = 0;
        uint64_t step_ns = 0, sub_ns = 0;
        bool a_held = false;
    } lg;

    double long_t(uint64_t now) const { return (now - started_ns) / 1e9; }

    void long_observe(const GameObservation& o, uint64_t now) {
        LongState& L = lg;
        const double t = long_t(now);
        if (!L.init) {
            L.init = true;
            L.stage_ns = L.beat_ns = L.pulse_ns = now;
            L.stage_frame = L.beat_frame = L.pulse_frame = o.frame;
            std::memcpy(L.stage, o.stage, sizeof(L.stage));
            L.room = o.room; L.layer = o.layer; L.entrance = o.entrance;
            std::printf("[Prog] t=%.1f director start: stage=%s room=%d layer=%d entrance=%d frame=%u\n", t, o.stage, o.room,
                        o.layer, o.entrance, o.frame);
        }
        if (std::strcmp(o.stage, L.stage) != 0 || o.room != L.room || o.layer != L.layer || o.entrance != L.entrance) {
            const double secs = (now - L.stage_ns) / 1e9;
            const uint32_t frames = o.frame - L.stage_frame;
            std::printf("[Prog] t=%.1f stage %s r%d l%d e%d -> %s r%d l%d e%d (frame %u); previous: %u frames in %.1f s = "
                        "%.1f fps\n", t, L.stage, L.room, L.layer, L.entrance, o.stage, o.room, o.layer, o.entrance, o.frame,
                        frames, secs, secs > 0 ? frames / secs : 0.0);
            std::memcpy(L.stage, o.stage, sizeof(L.stage));
            L.room = o.room; L.layer = o.layer; L.entrance = o.entrance;
            L.stage_ns = now; L.stage_frame = o.frame;
        }
        if (o.event_state != L.event || std::strcmp(o.event_name, L.ev_name) != 0) {
            std::printf("[Prog] t=%.1f event %d '%s' -> %d '%s' stage=%s link=%d pos=(%.0f,%.0f,%.0f)\n", t, L.event,
                        L.ev_name, o.event_state, o.event_name, o.stage, o.link_present ? 1 : 0, o.pos[0], o.pos[1], o.pos[2]);
            L.event = o.event_state;
            std::memcpy(L.ev_name, o.event_name, sizeof(L.ev_name));
        }
        if (now - L.beat_ns >= 15'000 * kMs) {
            const double secs = (now - L.beat_ns) / 1e9;
            const auto& R = route();
            if (mode == Mode::LoadSave) // LOAD14: no route; hearts/rupees instead
                std::printf("[Prog] t=%.1f beat stage=%s r%d ev=%d '%s' pos=(%.0f,%.0f,%.0f) roty=%.0f hearts=%d/%d "
                            "rupees=%d fps=%.1f\n", t, o.stage, o.room, o.event_state, o.event_name, o.pos[0], o.pos[1],
                            o.pos[2], o.rot_y * (180.0 / 32768.0), o.health, o.health_capacity, o.rupees,
                            (o.frame - L.beat_frame) / secs);
            else
                std::printf("[Prog] t=%.1f beat stage=%s r%d ev=%d '%s' step=%zu/%zu '%s' pos=(%.0f,%.0f,%.0f) roty=%.0f "
                            "fps=%.1f\n", t, o.stage, o.room, o.event_state, o.event_name, L.step, R.size(),
                            L.step < R.size() ? R[L.step].label : "(route done)", o.pos[0], o.pos[1], o.pos[2],
                            o.rot_y * (180.0 / 32768.0), (o.frame - L.beat_frame) / secs);
            L.beat_ns = now; L.beat_frame = o.frame;
        }
        if (o.link_present && !L.nan_logged && !(std::isfinite(o.pos[0]) && std::isfinite(o.pos[1]) && std::isfinite(o.pos[2]))) {
            L.nan_logged = true;
            std::printf("[Prog] t=%.1f ANOMALY: Link position not finite (frame %u, action %d)\n", t, o.frame, o.current_action);
        }
        const uint32_t k = L.hn % 64;
        L.hx[k] = o.pos[0]; L.hz[k] = o.pos[2]; L.csx[k] = stick_x; L.csy[k] = stick_y;
        ++L.hn;
        L.idle = o.link_present && o.event_state == 0 ? L.idle + 1 : 0;
        std::fflush(stdout);
    }

    // Distance moved over the last n fresh frames.
    float moved(uint32_t n) const {
        if (lg.hn <= n) return 1e9f;
        const uint32_t a = (lg.hn - 1) % 64, b = (lg.hn - 1 - n) % 64;
        return std::hypot(lg.hx[a] - lg.hx[b], lg.hz[a] - lg.hz[b]);
    }

    // Stick toward world direction (dx,dz). The stick is camera relative: Link moves along F*(sy + i*sx) in complex
    // x + i*z, F = camera forward (controls track: stick up = camera forward, stick right = camera right). F starts
    // as Link's facing (the camera trails Link) and is re-measured from Link's own displacement while the command
    // is steady: F = d * conj(s).
    void steer(const GameObservation& o, float dx, float dz, float mag, bool fresh) {
        LongState& L = lg;
        if (!L.f_init) {
            const float a = o.rot_y * (3.14159265f / 32768.0f);
            L.fx = std::sin(a); L.fz = std::cos(a); L.f_init = true;
        }
        constexpr uint32_t K = 5;
        if (fresh && L.hn > K + 1) {
            const uint32_t a = (L.hn - 1) % 64, b = (L.hn - 1 - K) % 64;
            const float ddx = L.hx[a] - L.hx[b], ddz = L.hz[a] - L.hz[b], dl = std::hypot(ddx, ddz);
            float sx = 0, sy = 0;
            bool steady = true;
            for (uint32_t i = 0; i < K; ++i) {
                const uint32_t j = (L.hn - 1 - i) % 64;
                sx += L.csx[j]; sy += L.csy[j];
            }
            const float sl = std::hypot(sx, sy);
            for (uint32_t i = 0; i < K && steady; ++i) {
                const uint32_t j = (L.hn - 1 - i) % 64;
                const float cl = std::hypot(L.csx[j], L.csy[j]);
                steady = cl > 0.28f && sl > 0 && (L.csx[j] * sx + L.csy[j] * sy) / (cl * sl) > 0.87f; // within 30 deg
            }
            if (steady && dl > 12.0f && std::isfinite(dl)) {
                const float hx = ddx / dl, hz = ddz / dl, ux = sx / sl, uy = sy / sl;
                const float mx = hx * uy + hz * ux, mz = hz * uy - hx * ux; // d * conj(s)
                L.fx += 0.3f * (mx - L.fx); L.fz += 0.3f * (mz - L.fz);
                const float n = std::hypot(L.fx, L.fz);
                if (n > 1e-3f) { L.fx /= n; L.fz /= n; }
            }
        }
        const float d = std::hypot(dx, dz);
        if (!(d > 1e-3f)) { stick_x = stick_y = 0; return; }
        dx /= d; dz /= d;
        stick_x = mag * (dz * L.fx - dx * L.fz);
        stick_y = mag * (dx * L.fx + dz * L.fz);
    }

    void step_done(uint64_t now, const GameObservation& o, const char* how) {
        LongState& L = lg;
        const auto& R = route();
        std::printf("[Prog] t=%.1f step %zu/%zu '%s' %s after %.1f s (%u controllable frames, %d stalls, %d door tries) "
                    "pos=(%.0f,%.0f,%.0f)\n", long_t(now), L.step, R.size(), R[L.step].label, how,
                    (now - L.step_ns) / 1e9, L.step_frames, L.stuck, L.attempts, o.pos[0], o.pos[1], o.pos[2]);
        ++L.step;
        L.sub = kApproach; L.stuck = L.attempts = L.presses = L.recover = 0; L.lateral = 0;
        L.step_frames = 0; L.step_ns = now; L.move_hn = L.hn; L.a_held = false;
        if (L.step < R.size())
            std::printf("[Prog] t=%.1f next step %zu '%s' target=(%.0f,%.0f)\n", long_t(now), L.step, R[L.step].label,
                        R[L.step].x, R[L.step].z);
        std::fflush(stdout);
    }

    // A step that fails (door that does not open, waypoint not reached) blocks the rest of this stage's route:
    // skip it, A pulses from here on.
    void route_failed(uint64_t now, const GameObservation& o, const char* how) {
        const std::string stage = route()[lg.step].stage;
        step_done(now, o, how);
        while (lg.step < route().size() && stage == route()[lg.step].stage) {
            std::printf("[Prog] t=%.1f step %zu '%s' skipped (route for %s aborted)\n", long_t(now), lg.step,
                        route()[lg.step].label, stage.c_str());
            ++lg.step;
        }
        std::fflush(stdout);
    }

    // Walk toward (tx,tz); stall recovery (side steps); returns the distance.
    float walk_to(const GameObservation& o, uint64_t now, float tx, float tz, float slow_radius, bool fresh) {
        LongState& L = lg;
        float dx = tx - o.pos[0], dz = tz - o.pos[2];
        const float dist = std::hypot(dx, dz);
        if (L.recover > 0) {
            if (fresh) --L.recover;
            const float c = 0.0f, s = L.side; // side step: 90 deg off the goal direction, slightly backwards
            const float rx = dx * c - dz * s - 0.3f * dx, rz = dx * s + dz * c - 0.3f * dz;
            steer(o, rx, rz, 1.0f, fresh);
            if (L.recover == 0) L.move_hn = L.hn;
            return dist;
        }
        if (fresh && L.hn - L.move_hn >= 45 && moved(40) < 25.0f) {
            ++L.stuck;
            L.side = -L.side;
            L.recover = 18;
            std::printf("[Prog] t=%.1f STALL %d in step %zu '%s': moved %.0f in 40 frames at (%.0f,%.0f), target "
                        "(%.0f,%.0f); side step %s\n", long_t(now), L.stuck, L.step,
                        walk_label_ ? walk_label_ : route()[L.step].label, moved(40),
                        o.pos[0], o.pos[2], tx, tz, L.side > 0 ? "left" : "right");
            std::fflush(stdout);
        }
        const float mag = slow_radius > 0 ? std::clamp(dist / slow_radius, 0.35f, 1.0f) : 1.0f;
        steer(o, dx, dz, mag, fresh);
        return dist;
    }

    bool long_script(uint64_t now) {
        LongState& L = lg;
        const GameObservation o = g_game_observation.get();
        const bool fresh = o.frame != L.last_frame;
        if (fresh) { L.last_frame = o.frame; long_observe(o, now); }
        if (!L.init) return false;
        const auto& R = route();
        // SOAK14: once the route is over in Skyloft, the soak script (soak_script.h) owns the controls.
        if (L.step >= R.size() && L.goal_logged && SoakScript::requested()) return soak_.update(o, now, fresh, stick_x, stick_y);
        // Leaving the current step's stage after acting in it ends that stage's part of the route.
        if (L.step < R.size() && L.entered && o.stage[0] && std::strcmp(o.stage, R[L.step].stage) != 0) {
            const std::string left = R[L.step].stage;
            std::printf("[Prog] t=%.1f left %s for %s (entrance %d) during step %zu '%s'\n", long_t(now), left.c_str(),
                        o.stage, o.entrance, L.step, R[L.step].label);
            if (R[L.step].kind == kDoor && L.sub == kThrough) step_done(now, o, "done (stage change)");
            while (L.step < R.size() && left == R[L.step].stage) {
                std::printf("[Prog] t=%.1f step %zu '%s' skipped (stage left)\n", long_t(now), L.step, R[L.step].label);
                ++L.step;
            }
            L.entered = false; L.sub = kApproach; L.step_frames = 0; L.step_ns = now;
            if (!L.goal_logged && left == "F001r" && std::strcmp(o.stage, "F000") == 0) {
                L.goal_logged = true;
                std::printf("[Prog] t=%.1f GOAL: left the Knight Academy (F001r) into Skyloft F000 entrance %d\n",
                            long_t(now), o.entrance);
            }
            std::fflush(stdout);
        }
        const bool on_route = L.step < R.size() && std::strcmp(o.stage, R[L.step].stage) == 0;
        const bool controllable = L.idle >= 20;
        if (!controllable || !on_route) {
            // Events, cutscenes, dialogs, or a stage without route: A pulses (1.5 s held of every 3 s, >= 2 game
            // frames) as in new-game, neutral stick. Route timers pause.
            if (on_route && R[L.step].kind == kDoor && L.sub == kPress && o.event_state > 0) {
                L.sub = kThrough;
                std::printf("[Prog] t=%.1f door event '%s' started (step %zu '%s')\n", long_t(now), o.event_name, L.step,
                            R[L.step].label);
                std::fflush(stdout);
            }
            if (!L.paused) { L.paused = true; L.pulse_ns = now; L.pulse_frame = g_game_observation.frames(); }
            stick_x = stick_y = 0;
            const uint32_t f = g_game_observation.frames() - L.pulse_frame;
            return (now - L.pulse_ns) % (3000 * kMs) >= 1500 * kMs && f >= 2;
        }
        const RouteStep& st = R[L.step];
        if (L.paused || !L.entered) {
            L.paused = false;
            L.move_hn = L.hn;
            L.recover = 0;
            L.f_init = false; // after an event the camera is re-placed behind Link: restart the estimate from his facing
            if (!L.entered) {
                L.entered = true;
                L.step_ns = now;
                std::printf("[Prog] t=%.1f Link controllable in %s room=%d layer=%d frame=%u pos=(%.1f,%.1f,%.1f) "
                            "roty=%.0f; route step %zu '%s' target=(%.0f,%.0f)\n", long_t(now), o.stage, o.room, o.layer,
                            o.frame, o.pos[0], o.pos[1], o.pos[2], o.rot_y * (180.0 / 32768.0), L.step, st.label, st.x,
                            st.z);
                std::fflush(stdout);
            }
        }
        if (fresh) ++L.step_frames;
        if (st.kind == kGoto) {
            const float dist = walk_to(o, now, st.x, st.z, 0, fresh);
            if (dist < st.r) step_done(now, o, "reached");
            else if (L.step_frames > 1200 || L.stuck >= 8) route_failed(now, o, "FAILED (timeout/stalls)");
            return false;
        }
        // Door
        const float px = st.x + st.nx * 70.0f - st.nz * L.lateral, pz = st.z + st.nz * 70.0f + st.nx * L.lateral;
        switch (L.sub) {
        case kApproach: {
            const float dist = walk_to(o, now, px, pz, 150.0f, fresh);
            if (dist < 22.0f) {
                L.sub = kFace; L.sub_frame = o.frame; L.face_ok = 0;
                L.face_cmd = std::atan2(st.x - o.pos[0], st.z - o.pos[2]);
                std::printf("[Prog] t=%.1f door '%s': at the stand point (%.0f,%.0f), turning to the door\n", long_t(now),
                            st.label, o.pos[0], o.pos[2]);
                std::fflush(stdout);
            } else if (L.step_frames > 1200 || L.stuck >= 8) {
                route_failed(now, o, "FAILED (door not reached)");
            }
            return false;
        }
        case kFace: {
            // Walk at the door until Link FACES it (closed loop on his own rot_y: the command angle integrates the
            // facing error, which removes any camera-estimate bias) and is close, then press A.
            const float dx = st.x - o.pos[0], dz = st.z - o.pos[2], dist = std::hypot(dx, dz);
            const float err = std::remainder(std::atan2(dx, dz) - o.rot_y * (3.14159265f / 32768.0f), 6.2831853f);
            if (fresh) {
                const float want = std::atan2(dx, dz);
                L.face_cmd = want + std::clamp(std::remainder(L.face_cmd + 0.2f * err - want, 6.2831853f), -1.57f, 1.57f);
                L.face_ok = std::fabs(err) < 0.17f ? L.face_ok + 1 : 0;
            }
            const uint32_t f = o.frame - L.sub_frame;
            if ((L.face_ok >= 3 && dist < 75.0f) || f >= 45) {
                std::printf("[Prog] t=%.1f door '%s': facing error %.0f deg, %.0f from the door after %u frames; A presses "
                            "(%s)\n", long_t(now), st.label, err * 57.2958f, dist, f,
                            L.attempts % 2 ? "stick pushed at the door" : "stick neutral");
                std::fflush(stdout);
                L.sub = kPress; L.sub_frame = o.frame; L.sub_ns = now; L.presses = 0; L.a_held = false;
                stick_x = stick_y = 0;
                return false;
            }
            steer(o, std::sin(L.face_cmd), std::cos(L.face_cmd), dist > 75.0f ? 0.5f : 0.3f, fresh);
            return false;
        }
        case kPress: {
            if (L.attempts % 2) steer(o, st.x - o.pos[0], st.z - o.pos[2], 0.35f, fresh);
            else stick_x = stick_y = 0;
            // A held >= 5 game frames and 170 ms, then released >= 10 frames; 6 presses per attempt.
            const uint32_t f = o.frame - L.sub_frame;
            if (L.a_held && f >= 5 && now - L.sub_ns >= 170 * kMs) { L.a_held = false; L.sub_frame = o.frame; L.sub_ns = now; }
            else if (!L.a_held && f >= 10) {
                if (L.presses >= 6) {
                    ++L.attempts;
                    // Link in the door's frame (dAcOdoor_c: v = (link - door).rotY(-door angle); box |x|<=80, |z|<=100).
                    const float vx = o.pos[0] - st.x, vz = o.pos[2] - st.z;
                    std::printf("[Prog] t=%.1f door '%s': no event after %d A presses at (%.0f,%.0f) roty=%.0f, door "
                                "frame x=%.0f z=%.0f, facing error %.0f deg (try %d)\n", long_t(now), st.label, L.presses,
                                o.pos[0], o.pos[2], o.rot_y * (180.0 / 32768.0), vx * st.nz - vz * st.nx,
                                vx * st.nx + vz * st.nz,
                                std::remainder(std::atan2(-vx, -vz) - o.rot_y * (3.14159265f / 32768.0f), 6.2831853f) *
                                    57.2958f,
                                L.attempts);
                    std::fflush(stdout);
                    if (L.attempts >= 4) { route_failed(now, o, "FAILED (door did not open)"); return false; }
                    L.lateral = (L.attempts % 2 ? 25.0f : -25.0f) * ((L.attempts + 1) / 2);
                    L.sub = kApproach; L.move_hn = L.hn;
                    return false;
                }
                ++L.presses; L.a_held = true; L.sub_frame = o.frame; L.sub_ns = now;
            }
            return L.a_held;
        }
        case kThrough: {
            const float side = (o.pos[0] - st.x) * st.nx + (o.pos[2] - st.z) * st.nz;
            if (side < -15.0f) { step_done(now, o, "passed"); return false; }
            ++L.attempts;
            std::printf("[Prog] t=%.1f door '%s': event over but Link still on the near side (%.0f) at (%.0f,%.0f) "
                        "(try %d)\n", long_t(now), st.label, side, o.pos[0], o.pos[2], L.attempts);
            std::fflush(stdout);
            if (L.attempts >= 4) route_failed(now, o, "FAILED (not through the door)");
            else { L.sub = kApproach; L.move_hn = L.hn; }
            return false;
        }
        }
        return false;
    }

    // ---- load-save (LOAD14, roadmap M6.3) ----
    // Bird statues (saveObj, d_a_obj_save.cpp): Stage/F000/F000_stg_l0.arc room 0 room.bzs layer 0 OBJS "saveObj"
    // (work/prog11 dump3.py F000 0); p1 low byte = statue id.
    struct Statue { const char* stage; float x, y, z; const char* label; };
    static const std::vector<Statue>& statues() {
        static const std::vector<Statue> v = {
            {"F000", -4699.0f, 1238.0f, -6364.0f, "F000 saveObj 0x2E (Knight Academy plateau)"},
            {"F000", 2187.0f, 394.0f, 804.0f, "F000 saveObj 0x2F"},
            {"F000", 4174.0f, -3157.0f, -5928.0f, "F000 saveObj 0x30"},
            {"F000", -2354.0f, 1881.0f, -9623.0f, "F000 saveObj 0x31"},
        };
        return v;
    }
    enum SaveSub { kSvChoose, kSvApproach, kSvFace, kSvPress, kSvTalk, kSvDone };
    struct LoadState {
        bool save_requested = false, nothing = false, title_left = false, loaded = false, after_init = false;
        bool pulsing = false, saved = false, a_held = false, menu_clicked = false;
        uint64_t talk_ns = 0;
        int slot = -1;
        SaveSlotObservation chosen{};
        uint32_t last_frame = 0, ctrl = 0;
        uint64_t after_ns = 0, pulse_ns = 0;
        uint32_t pulse_frame = 0;
        int sv = kSvChoose, statue = -1, attempts = 0, presses = 0, dialogs = 0, face_ok = 0;
        float stand = 95.0f, ux = 0, uz = 1, face_cmd = 0;
        uint32_t sub_frame = 0, step_frames = 0, idle = 0;
        uint64_t sub_ns = 0;
        int64_t saved_before = 0;
    } ld;
    const char* walk_label_ = nullptr;

public:
    // LOAD14 -> SOAK14: true once the load-save replay has loaded the file and Link is controllable ("[Load] LOADED").
    bool load_controllable() const { return mode == Mode::LoadSave && ld.loaded; }

private:

    static bool parse_points(const char* env, std::vector<std::pair<float, float>>& out) {
        const char* e = std::getenv(env);
        if (!e || !e[0]) return false;
        out.clear();
        std::string text(e);
        size_t start = 0;
        while (start <= text.size()) {
            const size_t end = text.find(';', start);
            const std::string item = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            float x = 0, y = 0;
            if (std::sscanf(item.c_str(), "%f,%f", &x, &y) == 2) out.push_back({x, y});
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return true;
    }

    // At the end of the file-select settle time: choose the used slot with the most recent save and plan the clicks.
    bool build_load_actions(uint64_t now) {
        LoadState& D = ld;
        const GameObservation o = g_game_observation.get();
        if (!o.file_manager || !o.slots[0].read) {
            if (now - stage_ns < 20'000 * kMs) return false;
            std::printf("[Load] FileManager save buffer not readable 20 s into the file select: nothing to load; idle\n");
            std::fflush(stdout);
            D.nothing = true;
            return false;
        }
        int best = -1;
        for (int i = 0; i < 3; ++i) {
            const SaveSlotObservation& sl = o.slots[i];
            std::printf("[Load] slot %d: %s name='%s' saved_time=%lld play=%.0f s stage=%s room=%d layer=%d entrance=%d "
                        "pos=(%.1f,%.1f,%.1f) hearts=%d/%d\n", i, sl.new_file ? "empty (New Adventure)" : "used",
                        sl.name, static_cast<long long>(sl.saved_time), sl.play_time / 60750000.0, sl.stage, sl.room,
                        sl.layer, sl.entrance, sl.pos[0], sl.pos[1], sl.pos[2], sl.health, sl.health_capacity);
            if (sl.new_file) continue;
            if (best < 0 || sl.saved_time > o.slots[best].saved_time) best = i;
        }
        if (best < 0) {
            std::printf("[Load] no used save slot: nothing to load (this replay never starts a new file); idle\n");
            std::fflush(stdout);
            D.nothing = true;
            return false;
        }
        D.slot = best;
        D.chosen = o.slots[best];
        // Targets in 640x480 screenshot coordinates (pointer pos = ((x-320)/320, (y-240)/240), as for the keyboard
        // keys), measured on graphics run 20261003-080701 (16:9 SYSCONF): the slot feathers of "Select a file."
        // (StateID_FileSelect) and the "Start" button of "Do you want to start this quest?" (the content select;
        // Copy/Delete there are the +/- buttons and Back is B or (200,420), so A on the pointer cannot copy or
        // delete a file).
        std::vector<std::pair<float, float>> slots = {{355.0f, 140.0f}, {400.0f, 237.0f}, {445.0f, 330.0f}};
        std::vector<std::pair<float, float>> custom;
        if (parse_points("NWII_SS_REPLAY_SLOT_TARGETS", custom))
            for (size_t i = 0; i < custom.size() && i < 3; ++i) slots[i] = custom[i];
        std::vector<std::pair<float, float>> confirm = {{437.0f, 420.0f}};
        parse_points("NWII_SS_REPLAY_LOAD_TARGETS", confirm);
        actions.clear();
        action = 0;
        actions.push_back({slots[best].first, slots[best].second, "slot " + std::to_string(best)});
        for (size_t i = 0; i < confirm.size(); ++i)
            actions.push_back({confirm[i].first, confirm[i].second, "confirm " + std::to_string(i + 1)});
        std::printf("[Load] chosen slot %d '%s' (used, most recent save): saved stage=%s room=%d layer=%d entrance=%d "
                    "pos=(%.1f,%.1f,%.1f) angle=%.0f deg hearts=%d/%d; %zu pointer action(s)\n", best, D.chosen.name,
                    D.chosen.stage, D.chosen.room, D.chosen.layer, D.chosen.entrance, D.chosen.pos[0], D.chosen.pos[1],
                    D.chosen.pos[2], D.chosen.angle * (180.0 / 32768.0), D.chosen.health, D.chosen.health_capacity,
                    actions.size());
        std::fflush(stdout);
        return true;
    }

    // A pulses: 1.5 s released, 1.5 s held, >= 2 game frames per edge (the new-game pattern).
    bool load_pulse(uint64_t now) {
        LoadState& D = ld;
        if (!D.pulsing) { D.pulsing = true; D.pulse_ns = now; D.pulse_frame = g_replay_pointer_frames.load(); }
        const uint32_t f = g_replay_pointer_frames.load() - D.pulse_frame;
        return (now - D.pulse_ns) % (3000 * kMs) >= 1500 * kMs && f >= 2;
    }
    bool load_quiet() { ld.pulsing = false; stick_x = stick_y = 0; return false; }

    bool load_after_script(uint64_t now) {
        LoadState& D = ld;
        if (D.nothing) return load_quiet();
        const GameObservation o = g_game_observation.get();
        const bool fresh = o.frame != D.last_frame;
        const int phase = g_replay_title_phase.load();
        if (!D.after_init) {
            D.after_init = true;
            D.after_ns = now;
            std::printf("[Load] t=%.1f pointer actions done; waiting for the file to load\n", long_t(now));
            std::fflush(stdout);
        }
        if (fresh) {
            D.last_frame = o.frame;
            if (D.title_left) long_observe(o, now);
        }
        if (!D.title_left) {
            if (phase == kPhaseUnknown) {
                D.title_left = true;
                std::printf("[Load] t=%.1f title left (dTitleMgr_c gone): the game loads the file\n", long_t(now));
                std::fflush(stdout);
                return load_quiet();
            }
            // Prompts after the slot: quiet first (screenshots of the content select), then A pulses with the
            // pointer left on the last target.
            static const uint64_t quiet_ns = [] {
                const char* e = std::getenv("NWII_SS_REPLAY_LOAD_QUIET_MS");
                return uint64_t(e && e[0] ? std::clamp(std::atoi(e), 0, 120000) : 6000) * kMs;
            }();
            if (now - D.after_ns < quiet_ns) return load_quiet();
            return load_pulse(now);
        }
        if (!D.loaded) {
            // Loading / spawn events: A pulses only while an event runs or Link is absent, never while Link idles
            // (an A next to the bird statue would talk to it).
            if (fresh) D.ctrl = o.link_present && o.event_state == 0 ? D.ctrl + 1 : 0;
            if (D.ctrl >= 45) {
                D.loaded = true;
                report_loaded(o, now);
            } else if (!o.link_present || o.event_state != 0) {
                return load_pulse(now);
            } else {
                return load_quiet();
            }
        }
        // SOAK14: after the load the soak script (soak_script.h) owns the controls (NWII_SS_REPLAY_SOAK=1).
        if (!D.save_requested && Ctrl18Script::requested()) return ctrl18_.update(o, now, fresh, stick_x, stick_y); // CTRL18-INPUT
        if (!D.save_requested && SoakScript::requested()) return soak_.update(o, now, fresh, stick_x, stick_y);
        if (!D.save_requested) return load_quiet();
        return save_script(o, now, fresh);
    }

    void report_loaded(const GameObservation& o, uint64_t now) {
        const SaveSlotObservation& sl = ld.chosen;
        const float dx = o.pos[0] - sl.pos[0], dy = o.pos[1] - sl.pos[1], dz = o.pos[2] - sl.pos[2];
        const float dist = std::hypot(dx, dz);
        const bool stage_ok = std::strcmp(o.stage, sl.stage) == 0 && o.room == sl.room;
        const bool pos_ok = dist < 150.0f && std::fabs(dy) < 100.0f;
        const bool hearts_ok = o.health == sl.health;
        std::printf("[Load] t=%.1f LOADED slot %d '%s': stage=%s room=%d layer=%d entrance=%d pos=(%.1f,%.1f,%.1f) "
                    "roty=%.0f hearts=%d/%d rupees=%d sword=%u | saved: stage=%s room=%d layer=%d entrance=%d "
                    "pos=(%.1f,%.1f,%.1f) angle=%.0f hearts=%d/%d | distance %.1f (dy %.1f) stage=%s pos=%s hearts=%s: "
                    "%s\n", long_t(now), ld.slot, sl.name, o.stage, o.room, o.layer, o.entrance, o.pos[0], o.pos[1],
                    o.pos[2], o.rot_y * (180.0 / 32768.0), o.health, o.health_capacity, o.rupees,
                    unsigned(o.current_sword), sl.stage, sl.room, sl.layer, sl.entrance, sl.pos[0], sl.pos[1], sl.pos[2],
                    sl.angle * (180.0 / 32768.0), sl.health, sl.health_capacity, dist, dy, stage_ok ? "ok" : "DIFF",
                    pos_ok ? "ok" : "DIFF", hearts_ok ? "ok" : "DIFF",
                    stage_ok && pos_ok && hearts_ok ? "MATCH" : "MISMATCH");
        std::fflush(stdout);
    }

    // Save round trip (NWII_SS_REPLAY_SAVE=1): nearest bird statue, face it, A, A pulses through its dialog; the
    // save itself is observed in the game's save buffer (the slot's saved_time changes).
    bool save_script(const GameObservation& o, uint64_t now, bool fresh) {
        LoadState& D = ld;
        LongState& L = lg;
        const int64_t slot_saved = D.slot >= 0 ? o.slots[D.slot].saved_time : 0;
        if (D.sv == kSvChoose) {
            float best = 1e9f;
            for (size_t i = 0; i < statues().size(); ++i) {
                const Statue& st = statues()[i];
                if (std::strcmp(o.stage, st.stage) != 0) continue;
                const float d = std::hypot(st.x - o.pos[0], st.z - o.pos[2]);
                if (d < best) { best = d; D.statue = int(i); }
            }
            if (D.statue < 0 || best > 1500.0f) {
                std::printf("[Load] t=%.1f save: no bird statue known within 1500 of Link in %s (nearest %.0f); no save\n",
                            long_t(now), o.stage, best);
                std::fflush(stdout);
                D.sv = kSvDone;
                return load_quiet();
            }
            const Statue& st = statues()[D.statue];
            const float vx = o.pos[0] - st.x, vz = o.pos[2] - st.z, vl = std::hypot(vx, vz);
            if (vl > 1.0f) { D.ux = vx / vl; D.uz = vz / vl; }
            // Optional: stand elsewhere in front of the statue before saving, so the round trip saves a position that
            // differs from the loaded one (NWII_SS_REPLAY_SAVE_TURN = degrees to rotate the statue->Link direction,
            // NWII_SS_REPLAY_SAVE_STAND = distance from the statue; defaults 0 and 95).
            if (const char* e = std::getenv("NWII_SS_REPLAY_SAVE_TURN")) {
                const float a = std::clamp(float(std::atof(e)), -90.0f, 90.0f) * 3.14159265f / 180.0f;
                const float rx = D.ux * std::cos(a) + D.uz * std::sin(a), rz = -D.ux * std::sin(a) + D.uz * std::cos(a);
                D.ux = rx; D.uz = rz;
            }
            if (const char* e = std::getenv("NWII_SS_REPLAY_SAVE_STAND")) D.stand = std::clamp(float(std::atof(e)), 50.0f, 200.0f);
            D.saved_before = slot_saved;
            walk_label_ = st.label;
            std::printf("[Load] t=%.1f save round trip: statue '%s' at (%.0f,%.0f), Link %.0f away; stand point "
                        "(%.0f,%.0f); slot %d saved_time=%lld before\n", long_t(now), st.label, st.x, st.z, best,
                        st.x + D.ux * D.stand, st.z + D.uz * D.stand, D.slot, static_cast<long long>(D.saved_before));
            std::fflush(stdout);
            D.sv = kSvApproach; D.step_frames = 0; L.move_hn = L.hn; L.recover = 0; L.stuck = 0; L.f_init = false;
        }
        if (!D.saved && D.slot >= 0 && slot_saved != D.saved_before) {
            D.saved = true;
            const SaveSlotObservation& sl = o.slots[D.slot];
            std::printf("[Load] t=%.1f SAVED: the game rewrote slot %d: saved_time %lld -> %lld, play %.1f s, stage=%s "
                        "room=%d layer=%d entrance=%d pos=(%.1f,%.1f,%.1f) angle=%.0f hearts=%d/%d\n", long_t(now), D.slot,
                        static_cast<long long>(D.saved_before), static_cast<long long>(sl.saved_time),
                        sl.play_time / 60750000.0, sl.stage, sl.room, sl.layer, sl.entrance, sl.pos[0], sl.pos[1],
                        sl.pos[2], sl.angle * (180.0 / 32768.0), sl.health, sl.health_capacity);
            std::fflush(stdout);
        }
        if (D.sv == kSvDone) return load_quiet();
        const Statue& st = statues()[D.statue];
        if (fresh) D.idle = o.link_present && o.event_state == 0 ? D.idle + 1 : 0;
        if (D.sv != kSvTalk && D.sv != kSvPress && (!o.link_present || o.event_state != 0)) {
            stick_x = stick_y = 0;
            return load_pulse(now); // an unexpected event: advance it, then go on
        }
        switch (D.sv) {
        case kSvApproach: {
            if (fresh) ++D.step_frames;
            const float px = st.x + D.ux * D.stand, pz = st.z + D.uz * D.stand;
            const float dist = walk_to(o, now, px, pz, 120.0f, fresh);
            if (dist < 20.0f) {
                D.sv = kSvFace; D.sub_frame = o.frame; D.face_ok = 0;
                D.face_cmd = std::atan2(st.x - o.pos[0], st.z - o.pos[2]);
                std::printf("[Load] t=%.1f save: at the stand point (%.0f,%.0f), %.0f from the statue; turning to it\n",
                            long_t(now), o.pos[0], o.pos[2], std::hypot(st.x - o.pos[0], st.z - o.pos[2]));
                std::fflush(stdout);
            } else if (D.step_frames > 900 || L.stuck >= 8) {
                std::printf("[Load] t=%.1f save: FAILED (stand point not reached; %u frames, %d stalls) at (%.0f,%.0f)\n",
                            long_t(now), D.step_frames, L.stuck, o.pos[0], o.pos[2]);
                std::fflush(stdout);
                D.sv = kSvDone;
            }
            return false;
        }
        case kSvFace: {
            const float dx = st.x - o.pos[0], dz = st.z - o.pos[2], dist = std::hypot(dx, dz);
            const float err = std::remainder(std::atan2(dx, dz) - o.rot_y * (3.14159265f / 32768.0f), 6.2831853f);
            if (fresh) {
                const float want = std::atan2(dx, dz);
                D.face_cmd = want + std::clamp(std::remainder(D.face_cmd + 0.2f * err - want, 6.2831853f), -1.57f, 1.57f);
                D.face_ok = std::fabs(err) < 0.17f ? D.face_ok + 1 : 0;
            }
            const uint32_t f = o.frame - D.sub_frame;
            if (D.face_ok >= 3 || f >= 45) {
                std::printf("[Load] t=%.1f save: facing error %.0f deg, %.0f from the statue after %u frames; A presses\n",
                            long_t(now), err * 57.2958f, dist, f);
                std::fflush(stdout);
                D.sv = kSvPress; D.sub_frame = o.frame; D.sub_ns = now; D.presses = 0; D.a_held = false;
                return load_quiet();
            }
            steer(o, std::sin(D.face_cmd), std::cos(D.face_cmd), 0.3f, fresh);
            return false;
        }
        case kSvPress: {
            stick_x = stick_y = 0;
            if (o.event_state != 0) {
                std::printf("[Load] t=%.1f save: statue event '%s' started after %d A press(es)\n", long_t(now),
                            o.event_name, D.presses);
                std::fflush(stdout);
                D.sv = kSvTalk; D.pulsing = false; D.a_held = false; D.talk_ns = now; D.menu_clicked = false;
                return false;
            }
            const uint32_t f = o.frame - D.sub_frame;
            if (D.a_held && f >= 5 && now - D.sub_ns >= 170 * kMs) { D.a_held = false; D.sub_frame = o.frame; D.sub_ns = now; }
            else if (!D.a_held && f >= 15) {
                if (D.presses >= 4) {
                    ++D.attempts;
                    std::printf("[Load] t=%.1f save: no event after %d A presses at (%.0f,%.0f), %.0f from the statue "
                                "(try %d)\n", long_t(now), D.presses, o.pos[0], o.pos[2],
                                std::hypot(st.x - o.pos[0], st.z - o.pos[2]), D.attempts);
                    std::fflush(stdout);
                    if (D.attempts >= 4) { D.sv = kSvDone; return false; }
                    static const float stands[] = {70.0f, 120.0f, 55.0f};
                    D.stand = stands[(D.attempts - 1) % 3];
                    D.sv = kSvApproach; D.step_frames = 0; L.move_hn = L.hn;
                    return false;
                }
                ++D.presses; D.a_held = true; D.sub_frame = o.frame; D.sub_ns = now;
            }
            return D.a_held;
        }
        case kSvTalk: {
            stick_x = stick_y = 0;
            if (D.idle >= 30) {
                ++D.dialogs;
                std::printf("[Load] t=%.1f save: statue dialog over (%d); %s\n", long_t(now), D.dialogs,
                            D.saved ? "the game saved: SAVE DONE" : "no save observed");
                std::fflush(stdout);
                if (D.saved || D.dialogs >= 3) D.sv = kSvDone;
                else { D.sv = kSvFace; D.sub_frame = o.frame; D.face_ok = 0; }
                return load_quiet();
            }
            if (o.event_state == 0) return load_quiet();
            // The statue's menu ("Bird Statue": Cancel (B) / Save / Save and quit, "Select" with the remote) is a
            // pointer menu: A alone picks nothing (graphics run 20261003-083548: 165 s of A pulses, menu unchanged).
            // 2.5 s after the event starts the pointer is turned onto "Save" (640x480 screenshot coordinates,
            // NWII_SS_REPLAY_STATUE_TARGET overrides) with the file-select pointer moves and A is pressed there;
            // A pulses then advance the following messages. "Save and quit" (320,390) is never targeted.
            if (!D.menu_clicked && now - D.talk_ns >= 2500 * kMs) {
                D.menu_clicked = true;
                std::vector<std::pair<float, float>> target = {{487.0f, 272.0f}};
                parse_points("NWII_SS_REPLAY_STATUE_TARGET", target);
                if (target.empty()) target = {{487.0f, 272.0f}};
                actions.push_back({target[0].first, target[0].second, "bird statue menu: Save"});
                moves = 0;
                measuring = false;
                next(Stage::Plan, now);
                std::printf("[Load] t=%.1f save: statue menu, pointing at 'Save' (%.0f,%.0f) and pressing A\n",
                            long_t(now), target[0].first, target[0].second);
                std::fflush(stdout);
                return load_quiet();
            }
            if (!D.menu_clicked) return load_quiet();
            return load_pulse(now);
        }
        default:
            return load_quiet();
        }
    }

    // Pointer pos per radian from the pointer model (virtual_motion.h): pos.x = -yaw/range,
    // pos.y = +pitch/range. Refined from measurements during the run.
    static constexpr float kModelGain = static_cast<float>(1.0 / kPointerHalfRangeRad);

    const uint64_t horizon_ns;
    const bool title_mode;
    const Mode mode;
    const bool long_mode;
    bool started = false;
    bool previous_a_pressed = false;
    bool horizon_logged = false;
    bool input_started = false;
    uint64_t started_ns = 0;
    // new-game script state
    std::vector<Action> actions;
    size_t action = 0;
    Stage stage = Stage::Settle;
    uint64_t stage_ns = 0;
    uint32_t stage_frames = 0;
    int moves = 0;
    int last_phase = kPhaseInput;
    float gain_x = -kModelGain, gain_y = kModelGain;
    float move_yaw = 0, move_pitch = 0, move_duration = 0.5f;
    double move_t = 0, done_yaw = 0, done_pitch = 0;
    bool measuring = false;
    float measure_x = 0, measure_y = 0, measure_yaw = 0, measure_pitch = 0;
    // gameplay script state
    float stick_x = 0, stick_y = 0;
    bool controllable = false, ctrl_step_started = false;
    uint32_t idle_frames = 0, ctrl_last_frame = 0;
    uint64_t ctrl_ns = 0;
    size_t ctrl_index = 0;
    CtrlRecord ctrl_record{};
    SoakScript soak_; // SOAK14 (soak_script.h)
    Ctrl18Script ctrl18_; // CTRL18-INPUT (ctrl18_script.h)
};

}
