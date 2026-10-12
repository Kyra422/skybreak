#pragma once
// CTRL18-INPUT (work/claude-notes/CTRL18-design.md sections 3.1-3.3 and 4; tracks T3/T4/T5/T6): intent -> context ->
// Wii signals. The "switch-hd" pad preset and the "kbm" keyboard+mouse preset produce INTENTS (input_bindings.h
// HdAction / KbmAction); this translator turns them, together with the read-only game context (game_context.h), into
// ordinary Wii Remote + Nunchuk signals: buttons, the Nunchuk stick, gesture requests (motion_gesture.h) and virtual
// remote rates (virtual_motion.h). The game keeps receiving only Wii signals; nothing is written to guest memory.
//
// Remote modes (design 3.1), chosen by situation:
//   REST   play: the remote is held level at the screen centre; no stick aim at all, so it cannot drift. Gestures start
//          and end there; after any other situation the remote is turned back (gesture recenter()).
//   RATE   aim (first person, item aim): the right stick / mouse turns the remote (+ the gyro in the hybrid).
//   TARGET the game's menu pointer (PointerAimLoop, SOAK14); the item / pouch / dowsing wheels (the game picks the slot
//          from its MotionPlus pointing direction mZ, calcItemWheelSelection 0x801E3E70: closed loop on the game's mZ);
//          Loftwing / swimming / skydive / tightrope / beetle: an absolute pose from the sticks.
// What the game tests for each action (research 2026-10-09, work/claude-notes/CTRL18-INPUT.md section "Wii triggers"):
//   items fire with A (bow/slingshot: hold A, release; beetle/clawshots: press; bellows: hold); B takes an item out;
//   Loftwing flap = any remote swing (|mpls_rot.xy| >= 2 rev/s), Spiral Charge = A, brake = B, yaw = remote roll,
//   pitch = pointing; ledge/vine leap = stick sideways + remote shake; carry: remote 45 deg up/down + any swing = throw /
//   roll; fatal blow = swing + Nunchuk shake (the spin entry) at a downed enemy; cutscene skip = hold 2 (4 frames).
// Every rule has an A/B env switch NWII_CTRL18_<RULE>=0 (ctrl18_rule_on).
// Header-only on purpose: no build-list change is needed.
#include "game_context.h"
#include "game_observation.h"
#include "camera_stick.h" // CTRL18-CAM: free-camera channel
#include "input_intents.h" // SDL-free (the sources include input_bindings.h)
#include "motion_gesture.h"
#include "stick_sword.h"
#include "virtual_motion.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace nwii::runtime::input {

// Wii Remote KPAD button masks and Nunchuk masks (as the sources write them).
inline constexpr uint32_t kWiiLeft = 0x0001, kWiiRight = 0x0002, kWiiDown = 0x0004, kWiiUp = 0x0008,
                          kWiiPlus = 0x0010, kWiiTwo = 0x0100, kWiiOne = 0x0200, kWiiB = 0x0400, kWiiA = 0x0800,
                          kWiiMinus = 0x1000, kWiiHome = 0x8000, kNunZ = 0x2000, kNunC = 0x4000;

// A/B switches: NWII_CTRL18_<NAME>=0 turns a rule off (live bisection, tests). Read once per name.
inline bool ctrl18_rule_on(const char* name) {
    struct Entry { const char* name; bool on; };
    static Entry cache[64];
    static int used = 0;
    static std::mutex m;
    std::lock_guard lock(m);
    for (int k = 0; k < used; ++k)
        if (std::strcmp(cache[k].name, name) == 0) return cache[k].on;
    char env[96];
    std::snprintf(env, sizeof env, "NWII_CTRL18_%s", name);
    const char* e = std::getenv(env);
    const bool on = !(e && e[0] == '0');
    if (used < 64) cache[used++] = {name, on};
    return on;
}

// Live flags the sources read from the game adapter (virtual_motion.h / game_observation.h atomics).
struct LiveFlags {
    bool player = false;          // g_guest_player_present
    bool pointer_cursor = false;  // g_guest_pointer_cursor (the game's menu pointer in play)
    bool point_center = false;    // g_guest_point_center
    bool motionplus_pointer = false;
    static LiveFlags read(int channel) {
        LiveFlags f;
        f.player = g_guest_player_present.load();
        f.pointer_cursor = g_guest_pointer_cursor.load();
        f.point_center = g_guest_point_center.load();
        f.motionplus_pointer = channel >= 0 && channel < 4 && g_guest_motionplus_pointer[channel].load();
        return f;
    }
};

enum class Situation : uint8_t { Menu, Event, Play, Aim, Wheel, Flight, Swim, Balance, Climb, Carry, Beetle, Skydive, Puzzle };
inline const char* situation_name_nl(Situation s) {
    switch (s) {
    case Situation::Puzzle: return "Sleutel draaien";
    case Situation::Menu: return "Menu";
    case Situation::Event: return "Tussenfilmpje/gesprek";
    case Situation::Play: return "Lopen";
    case Situation::Aim: return "Richten";
    case Situation::Wheel: return "Wiel";
    case Situation::Flight: return "Vliegen (Loftwing)";
    case Situation::Swim: return "Zwemmen";
    case Situation::Balance: return "Balanceren";
    case Situation::Climb: return "Klimmen/hangen";
    case Situation::Carry: return "Iets dragen";
    case Situation::Beetle: return "Kever vliegt";
    case Situation::Skydive: return "Vrije val";
    }
    return "?";
}

// The aim states (LINK +0x368 & 0x5D7: 0x2 bow, 0x4 clawshots, 0x10 beetle, 0x40 first person / dowsing, 0x80
// slingshot, 0x100 gust bellows; camera "Subject" modes 2-5/8). mSwordAndMoreStates FIRST_PERSON is kept for safety.
inline bool context_aiming(const GameContext& c) {
    if (c.sword_states & kSwordFirstPerson) return true;
    if (c.aim_flags & 0x5D7) return true;
    return c.cam_mode == 2 || c.cam_mode == 3 || c.cam_mode == 4 || c.cam_mode == 5 || c.cam_mode == 8;
}
inline bool context_wheel(const GameContext& c) {
    return (c.sword_states & (kSwordBWheelOpen | kSwordPouchOpen | kSwordCMenuOpen)) != 0;
}
inline bool context_lockon(const GameContext& c) { return c.cam_mode == 1 || c.cam_mode == 6 || c.cam_mode == 9; }

inline Situation classify(const GameContext& c, const LiveFlags& l) {
    // SOUE01 Act_IE 129 = View lock, 130 = Reset position. These two HUD prompts
    // identify the boss-key manipulation screen, even though its cursor is visible.
    if (c.valid && (c.prompt_slot[2] == 130 || c.prompt_slot[3] == 129) && !l.point_center)
        return Situation::Puzzle;
    if (!l.player || l.pointer_cursor || l.point_center) return Situation::Menu;
    if (!c.valid) return Situation::Play; // no context published (other games, unit fixtures): plain play
    if (c.event()) return Situation::Event;
    if (context_wheel(c)) return Situation::Wheel;
    if (c.riding == 2 && ctrl18_rule_on("FLIGHT")) return Situation::Flight;
    if (c.item_state == kItemBeetle && c.cam_handler == 6 && ctrl18_rule_on("BEETLE")) return Situation::Beetle;
    if (context_aiming(c)) return Situation::Aim;
    if ((c.action_flags & kActInWater) && ctrl18_rule_on("SWIM")) return Situation::Swim;
    if ((c.action == 0x12 || c.action == 0x13) && ctrl18_rule_on("SKYDIVE")) return Situation::Skydive;
    if ((c.action_flags & kActTightRope) && ctrl18_rule_on("BALANCE")) return Situation::Balance;
    if ((c.action_flags & (kActOnVines | kActHangingLedge | 0x20000u)) && ctrl18_rule_on("CLIMB")) return Situation::Climb;
    if (c.carrying() && ctrl18_rule_on("CARRY")) return Situation::Carry;
    return Situation::Play;
}

// What Wii A does right now (Act_IE_e): the action popup on A, else Link's own A request, else the game's default
// (fn_80242C60: DASH 95, DODGE 101 while Z-targeting). kPromptNone without a context.
inline int32_t a_action(const GameContext& c) {
    if (!c.valid) return kPromptNone;
    if (c.prompt_a >= 0 && c.prompt_a != kPromptNone) return c.prompt_a;
    if (c.link_request[0] >= 0) return c.link_request[0];
    return context_lockon(c) ? kPromptEtcDodge : kPromptEtcDash;
}
inline bool a_is_dash(const GameContext& c) {
    const int32_t a = a_action(c);
    return a == kPromptDash || a == kPromptEtcDash;
}
inline bool prompt_is_cancel(int32_t p) {
    return p == kPromptPutAway || p == kPromptLetGo || p == kPromptGetOut || p == kPromptRelease ||
           p == 104 /* ETC_PUT_AWAY */;
}
// "Back / put away / get off" prompts the game shows on the B button (Act_IE ETC_*: 104 PUT_AWAY, 108/116/137 BACK,
// 125 GET_OFF, 126 STORE, 127/146 RETURN, 131 CANCEL, 144 STOW_CANNON). HD B presses Wii B for them.
inline bool prompt_is_b_cancel(int32_t p) {
    return p == 104 || p == 108 || p == 116 || p == 137 || p == 125 || p == 126 || p == 127 || p == 146 || p == 131 ||
           p == 144;
}
// HD B with nothing on A: the B button's own back / put-away prompt (Link's B request or the meter's latched B slot),
// or an item in hand (Wii B puts it away again).
inline bool b_puts_away(const GameContext& c) {
    if (!c.valid) return false;
    if (prompt_is_b_cancel(c.link_request[4]) || prompt_is_b_cancel(c.prompt_slot[1])) return true;
    return c.item_state != kItemNone && c.item_state != kItemSword && c.item_state != 0x0A /* sailcloth */ &&
           (c.aim_flags & 0x5D7) != 0;
}
// The fatal-blow popup (fn_801C8720: icon 0x26 + ACT_DO_FATAL_BLOW 0x43).
inline bool fatal_blow_shown(const GameContext& c) {
    return c.valid && (c.dob_act == kPromptFatalBlow || c.prompt_a == kPromptFatalBlow);
}
// Items that fire / draw / blow / launch with Wii A once they are out (shootBow, shootSlingshotMaybe, shootBeetle,
// shootClawsMaybe, blowBellows).
inline bool item_fires_with_a(uint16_t item) {
    return item == kItemBow || item == kItemSlingshot || item == kItemBeetle || item == kItemClaw || item == kItemBellows;
}

// ---- live status for the launcher's test panel (CTRL18-UI) ----
struct Ctrl18Status {
    char preset[16] = "";
    char situation[32] = "";
    char last_move[48] = "";
    uint32_t moves = 0;
    bool gyro_aim = false;
    bool gyro_hint = false; // a gyro pad is connected while gyro aiming is off: show the one-time hint
};
inline std::mutex& ctrl18_status_mutex() { static std::mutex m; return m; }
inline Ctrl18Status& ctrl18_status_storage() { static Ctrl18Status s; return s; }
inline Ctrl18Status ctrl18_live_status() {
    std::lock_guard lock(ctrl18_status_mutex());
    return ctrl18_status_storage();
}
inline void ctrl18_set_status(const char* preset, Situation s, bool gyro_aim) {
    std::lock_guard lock(ctrl18_status_mutex());
    auto& st = ctrl18_status_storage();
    std::snprintf(st.preset, sizeof st.preset, "%s", preset);
    std::snprintf(st.situation, sizeof st.situation, "%s", situation_name_nl(s));
    st.gyro_aim = gyro_aim;
}
inline void ctrl18_note_move(const char* text) {
    std::lock_guard lock(ctrl18_status_mutex());
    auto& st = ctrl18_status_storage();
    std::snprintf(st.last_move, sizeof st.last_move, "%s", text);
    ++st.moves;
}
inline void ctrl18_set_gyro_hint(bool on) {
    std::lock_guard lock(ctrl18_status_mutex());
    ctrl18_status_storage().gyro_hint = on;
}
inline void ctrl18_note_sword_event(const SwordEvent& e) {
    char text[48];
    if (e.kind == SwordEvent::Swing)
        std::snprintf(text, sizeof text, "%s %s", e.strike ? "Hemelslag" : "Slag",
                      sword_sector_name_nl(SwordRecognizer::sector8(e.dx, e.dy)));
    else if (e.kind == SwordEvent::Spin)
        std::snprintf(text, sizeof text, "Draaiaanval (%s)", e.vertical ? "verticaal" : "horizontaal");
    else if (e.kind == SwordEvent::RaiseOn)
        std::snprintf(text, sizeof text, "Zwaard omhoog (hemelslag laden)");
    else
        return;
    ctrl18_note_move(text);
}

// ---- camera stick (free camera): CTRL18-CAM's channel (camera_stick.h) ----
// switch-hd: the right stick while "camera" (L) is held; kbm: the mouse in play. Only while the game camera may orbit
// (camera_free_ok(): the normal follow camera; otherwise 0 / nothing). wii-motion: no free camera (user decision).
inline void ctrl18_feed_camera(float stick_x, float stick_y) {
    if (!camera_free_ok()) stick_x = stick_y = 0.0f;
    camera_stick_set(stick_x, stick_y);
}
inline void ctrl18_feed_camera_mouse(float dx, float dy) {
    if ((dx != 0.0f || dy != 0.0f) && camera_free_ok()) camera_mouse_pixels(dx, dy);
}
// [camera] flight_invert_y (Loftwing / swimming / skydive: stick up = climb instead of dive). Stored by the sources.
inline std::atomic<bool> g_ctrl18_flight_invert_y{false};

// ---- common remote helpers ----
// Rates that steer the remote so the GAME's pointer lands on (tx, ty) (closed loop, SOAK14), level roll included.
inline void steer_pointer(const VirtualRemoteOrientation& o, const PointerAimLoop& aim, double tx, double ty, double dt,
                          float& pitch, float& yaw, float& roll) {
    double target[3];
    aim.aim(tx, ty, target);
    float r[3];
    o.rates_toward(target, dt, kPointerMotionMaxRateRadS, r);
    pitch = r[0]; yaw = r[1]; roll = r[2];
}
// Pointing direction (yaw right +, pitch up +) of the remote, radians.
inline void pointing_angles(const VirtualRemoteOrientation& o, double& yaw_right, double& pitch_up) {
    double m[3][3];
    o.matrix(m);
    yaw_right = std::atan2(-m[0][2], m[2][2]);
    pitch_up = std::asin(std::clamp(m[1][2], -1.0, 1.0));
}
// Rates toward an absolute pose: pointing (yaw right +, pitch up +) and roll (right +), radians.
inline void steer_pose(const VirtualRemoteOrientation& o, double yaw_right, double pitch_up, double roll_right,
                       double dt, float max_rate, float& pitch, float& yaw, float& roll) {
    const double t[3] = {-std::sin(yaw_right) * std::cos(pitch_up), std::sin(pitch_up),
                         std::cos(yaw_right) * std::cos(pitch_up)};
    float r[3];
    o.rates_toward(t, dt, max_rate, r);
    pitch = r[0]; yaw = r[1];
    // Roll: the angle phi from the remote's X (left) axis to "level" about the pointing axis (as level_roll_rate);
    // a body +z rate rolls the remote to the right, which makes phi negative: the wanted roll is phi = -roll_right.
    double m[3][3];
    o.matrix(m);
    double lx = m[2][2], lz = -m[0][2];
    const double n = std::sqrt(lx * lx + lz * lz);
    float rr = r[2];
    if (n > 0.1) {
        lx /= n; lz /= n;
        const double cx = m[1][0] * lz, cy = m[2][0] * lx - m[0][0] * lz, cz = -m[1][0] * lx;
        const double phi = std::atan2(cx * m[0][2] + cy * m[1][2] + cz * m[2][2], m[0][0] * lx + m[2][0] * lz);
        double err = phi + roll_right;
        err = std::atan2(std::sin(err), std::cos(err));
        double rate = std::clamp(err * 8.0, -double(max_rate), double(max_rate));
        if (dt > 0 && std::fabs(rate * dt) > std::fabs(err)) rate = err / dt;
        rr = float(rate);
    }
    roll = rr;
}
// Pointing / roll error of the remote from REST (level, screen centre), radians.
inline double rest_error(const VirtualRemoteOrientation& o) {
    double m[3][3];
    o.matrix(m);
    const double point = std::acos(std::clamp(m[2][2], -1.0, 1.0));
    const double roll = std::fabs(m[1][0]); // X axis tilt out of the horizontal plane
    return std::max(point, roll);
}
// Item / pouch / dowsing wheel: the game selects by its MotionPlus pointing direction mZ rotated rotX(+11 deg)
// (fn_801E3C80): tilt >= 12 deg selects (< 7 deg cancels), direction alpha = atan2(x = left, y = up), slot k centred at
// k * 45 deg toward the left from up (fn_8023ED40). dir_left / dir_up: wanted direction on screen (left / up > 0).
// The remote is steered so that the GAME's mZ (read back, read-only) points there.
inline void steer_wheel(const VirtualRemoteOrientation& o, const GameContext& c, double dir_left, double dir_up,
                        double dt, float& pitch, float& yaw, float& roll) {
    const double n = std::sqrt(dir_left * dir_left + dir_up * dir_up);
    if (!(n > 1e-6) || !(dt > 0)) return;
    const double tilt = 28.0 * 3.14159265358979323846 / 180.0;
    const double s11 = std::sin(11.0 * 3.14159265358979323846 / 180.0), c11 = std::cos(11.0 * 3.14159265358979323846 / 180.0);
    const double vx = std::sin(tilt) * dir_left / n, vy = std::sin(tilt) * dir_up / n, vz = std::cos(tilt);
    double t[3] = {vx, c11 * vy + s11 * vz, -s11 * vy + c11 * vz}; // undo the game's rotX(+11 deg)
    if (c.have_mz && ctrl18_rule_on("WHEEL_MZ_LOOP")) {
        double m[3][3];
        o.matrix(m);
        double d[3] = {m[0][2] - c.game_mz[0], m[1][2] - c.game_mz[1], m[2][2] - c.game_mz[2]};
        const double dn = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dn < 0.35)
            for (int k = 0; k < 3; ++k) t[k] += d[k]; // our direction = the game's + the (small) estimate difference
    }
    float r[3];
    o.rates_toward(t, dt, kPointerMotionMaxRateRadS, r);
    pitch = r[0]; yaw = r[1]; roll = r[2];
}
// Wheel slot index (as the game counts, LINK +0x43B1) of an on-screen direction; and the direction of a slot.
inline int wheel_index_of(double dir_left, double dir_up) {
    const double a = std::atan2(dir_left, dir_up);
    return int(std::lround(a / (3.14159265358979323846 / 4.0)) + 8) % 8;
}
inline void wheel_dir_of(int index, double& dir_left, double& dir_up) {
    const double a = index * (3.14159265358979323846 / 4.0);
    dir_left = std::sin(a);
    dir_up = std::cos(a);
}

// Aim stick response (the classic right-stick aim: deadzone, squared, 4 rad/s x speed).
inline float aim_axis(float v, float dead) {
    dead = std::min(dead, 0.99f);
    v = std::clamp(v, -1.0f, 1.0f);
    if (std::fabs(v) <= dead) return 0.0f;
    const float s = (std::fabs(v) - dead) / (1.0f - dead);
    return std::copysign(s * s, v);
}

// Output of one translator step for one channel.
struct WiiOut {
    uint32_t buttons = 0;         // Wii Remote buttons
    uint32_t nunchuk_buttons = 0; // C / Z
    float stick_x = 0, stick_y = 0;
    float pitch = 0, yaw = 0, roll = 0; // virtual remote rates (body, rad/s)
    bool claim_aim = false;              // the device was used (g_aim_owner = this device)
    bool steering = false;               // rates were set by a pose / wheel / aim rule this step
    Situation situation = Situation::Play;
};

// ---- the context rules shared by the pad and the keyboard+mouse translators ----
struct CoreIn {
    // held intents (HD names)
    bool action = false, dash = false, item = false, pouch = false, harp = false, recentre = false, shield = false,
         thrust = false, lock_on = false, map = false, gear = false, first_person = false, fi = false, help = false,
         dpad_left = false, dpad_up = false, dpad_right = false, home = false, skip = false;
    float mx = 0, my = 0;       // move (Nunchuk stick), length <= 1
    float steer_x = 0, steer_y = 0; // extra steering (pad right stick) for swim / beetle
    bool gyro_aim = false;
    double dt = 0;
};

class Ctrl18Core {
public:
    bool a_does_not_dash = true;
    static constexpr int kIn = 19;

    // Situation bookkeeping; call first every step.
    Situation begin(const GameContext& ctx, const LiveFlags& live, const VirtualRemoteOrientation& o) {
        const Situation s = classify(ctx, live);
        if (s != last_) {
            const auto playish = [](Situation x) { return x == Situation::Play || x == Situation::Climb; };
            if (playish(s) && !playish(last_) && ctrl18_rule_on("REST")) rest_pending_ = true;
            if (s == Situation::Beetle || s == Situation::Skydive) pointing_angles(o, entry_yaw_, entry_pitch_);
            if (s != Situation::Carry) carry_ = CarryStep::Idle;
            last_ = s;
        }
        return s;
    }
    Situation situation() const { return last_; }

    // Buttons and button-driven gestures of the common intents.
    void buttons(const CoreIn& in, const GameContext& ctx, Situation s, VirtualGestureController& g, WiiOut& out) {
        const bool e_action = in.action && !prev_.action, e_dash = in.dash && !prev_.dash, e_harp = in.harp && !prev_.harp;
        const bool e_shield = in.shield && !prev_.shield, e_thrust = in.thrust && !prev_.thrust;
        const bool e_recentre = in.recentre && !prev_.recentre;
        if (s == Situation::Puzzle) {
            // The original game owns grab/release/reset and rotation. No sword impulses,
            // camera recenter taps or automatic leveling may override those controls.
            if (in.action) out.buttons |= kWiiA;
            if (in.dash) out.buttons |= kWiiB;
            if (in.recentre || in.first_person) out.nunchuk_buttons |= kNunC;
            if (in.lock_on) out.nunchuk_buttons |= kNunZ;
            prev_ = in;
            return;
        }
        const bool menu = s == Situation::Menu || s == Situation::Event;
        const bool moving = in.mx * in.mx + in.my * in.my > 0.04f;
        const bool dt_ok = in.dt > 0.0 && in.dt <= 0.1;
        // A
        if (s == Situation::Flight && ctrl18_rule_on("FLAP")) {
            // Loftwing flap: any remote swing of >= 2 rev/s (fn_129_A0E0); a sideways out-and-back keeps the pointing
            // pitch and the roll (= the steering) unchanged. A itself would be the Spiral Charge.
            if (e_action) { g.swing(flap_right_ ? 1.0 : -1.0, 0.0); flap_right_ = !flap_right_; ctrl18_note_move("Klappen"); }
        } else if (s == Situation::Climb && std::fabs(in.mx) > 0.3f && ctrl18_rule_on("LEAP")) {
            // Ledge / vine leap: stick sideways + a remote shake (> 3 g) in the same frame (fn_801F4230, fn_801FDEB0);
            // A here would let go of the ledge.
            if (e_action) { g.thrust(); ctrl18_note_move("Zijsprong"); }
        } else if (in.action) {
            const bool suppress = a_does_not_dash && s == Situation::Play && a_is_dash(ctx) && ctrl18_rule_on("A_NO_DASH");
            if (!suppress) out.buttons |= kWiiA;
        }
        // HD B: menus / events / Loftwing (brake) / beetle (recall) -> Wii B; a cancel prompt on A (put away, let go,
        // get out, release) -> Wii A; held while moving when A would dash (or already sprinting) -> Wii A held
        // (latched until B is let go); otherwise nothing (Wii B in play would take out the B item).
        if (in.dash) {
            if (menu || s == Situation::Flight || s == Situation::Beetle || !ctrl18_rule_on("B_RULES")) {
                out.buttons |= kWiiB;
            } else if (s == Situation::Skydive || s == Situation::Wheel) {
                // nothing: the sailcloth is ZR (Wii B), the wheel closes with ZR / R
            } else if (dash_latched_ || cancel_latched_) {
                out.buttons |= kWiiA;
            } else if (e_dash && prompt_is_cancel(a_action(ctx))) {
                cancel_latched_ = true;
                out.buttons |= kWiiA;
            } else if (moving && (a_is_dash(ctx) || (ctx.sword_states & kSwordSprinting))) {
                dash_latched_ = true;
                out.buttons |= kWiiA;
            } else if ((e_dash || b_cancel_latched_) && b_puts_away(ctx) && ctrl18_rule_on("B_ITEM")) {
                b_cancel_latched_ = true; // an aimed item / a back prompt on B: Wii B (put away / back)
                out.buttons |= kWiiB;
            }
        } else {
            dash_latched_ = cancel_latched_ = b_cancel_latched_ = false;
        }
        // ZR: with a firing item out (aiming) = Wii A (draw / fire / launch / blow); beetle in flight = A (boost);
        // carrying = nothing (the stick throws); otherwise Wii B (take out, item wheel, sailcloth).
        if (in.item) {
            if (s == Situation::Carry) {
            } else if ((s == Situation::Aim && item_fires_with_a(ctx.item_state) && ctrl18_rule_on("ITEM_A")) ||
                       s == Situation::Beetle) {
                out.buttons |= kWiiA;
            } else {
                out.buttons |= kWiiB;
            }
        }
        if (in.pouch) out.buttons |= kWiiMinus;
        if (s == Situation::Flight) {
            if (in.harp) out.buttons |= kWiiA; // Spiral Charge (getDownTrigA)
            if (e_harp) ctrl18_note_move("Spiraalaanval");
        } else if (in.harp) {
            out.buttons |= kWiiUp;
        }
        if (in.map) {
            if (s == Situation::Event) { if (ctx.skip_shown && ctrl18_rule_on("SKIP")) out.buttons |= kWiiTwo; }
            else out.buttons |= kWiiPlus;
        }
        if (in.skip && s == Situation::Event) out.buttons |= kWiiTwo;
        if (in.gear) out.buttons |= kWiiOne;
        if (in.first_person) out.nunchuk_buttons |= kNunC;
        if (in.fi) out.buttons |= kWiiDown;
        if (in.help) out.buttons |= kWiiTwo;
        if (in.dpad_left) out.buttons |= kWiiLeft;
        if (in.dpad_up) out.buttons |= kWiiUp;
        if (in.dpad_right) out.buttons |= kWiiRight;
        if (in.home) out.buttons |= kWiiHome;
        if (in.lock_on) out.nunchuk_buttons |= kNunZ;
        // Y (gyro off): the camera behind Link = a short Nunchuk Z tap (the game's own re-centre, camera mode 7).
        if (e_recentre && !in.gyro_aim && !menu && s != Situation::Aim && s != Situation::Flight) z_tap_left_ = 0.12;
        if (z_tap_left_ > 0.0) {
            out.nunchuk_buttons |= kNunZ;
            if (dt_ok) z_tap_left_ -= in.dt;
        }
        // L3: Nunchuk shake = shield / bash / roll while dashing / swim spin dash (+0x4364 & 0x10).
        if (e_shield && !menu && s != Situation::Wheel) g.nunchuk_flick();
        // R3: thrust; at a downed enemy the fatal blow (swing + Nunchuk shake = doSpinAttack_orFinalBlow).
        if (e_thrust) {
            if (fatal_blow_shown(ctx) && ctrl18_rule_on("FATAL_BLOW")) { spin(g, true); ctrl18_note_move("Genadeslag"); }
            else if (s == Situation::Play || s == Situation::Climb || s == Situation::Balance || s == Situation::Swim) {
                g.thrust();
                ctrl18_note_move("Steek");
            }
        }
        out.stick_x = in.mx;
        out.stick_y = in.my;
        prev_ = in;
    }

    // A sword event of a recogniser (stick / mouse). At a downed enemy any swing is the fatal blow.
    void sword_event(const SwordEvent& e, const GameContext& ctx, VirtualGestureController& g) {
        if (e.kind == SwordEvent::Swing && fatal_blow_shown(ctx) && ctrl18_rule_on("FATAL_BLOW")) {
            spin(g, std::fabs(e.dy) >= std::fabs(e.dx));
            ctrl18_note_move("Genadeslag");
            return;
        }
        switch (e.kind) {
        case SwordEvent::Swing: g.swing(e.dx, e.dy); break;
        case SwordEvent::RaiseOn: g.set_raised(true); break;
        case SwordEvent::RaiseOff: g.set_raised(false); break;
        case SwordEvent::Spin: spin(g, e.vertical); break;
        default: break;
        }
        ctrl18_note_sword_event(e);
    }
    // Spin: preempt queued swings so the remote's out-and-back and the Nunchuk shake run together; the return goes to
    // REST (recenter sets the home), whatever pose earlier flicks left.
    static void spin(VirtualGestureController& g, bool vertical) {
        g.reset();
        g.recenter();
        g.spin(vertical);
    }

    // Poses of the vehicle-like situations from the move stick (+ extra steering). Returns true when it set rates.
    bool steer(const CoreIn& in, Situation s, const VirtualRemoteOrientation& o, WiiOut& out) {
        const double dt = in.dt;
        if (!(dt > 0.0 && dt <= 0.1)) return false;
        const double inv = g_ctrl18_flight_invert_y.load() ? -1.0 : 1.0;
        const double sx = std::clamp(in.mx + in.steer_x, -1.0f, 1.0f), sy = std::clamp(in.my + in.steer_y, -1.0f, 1.0f);
        switch (s) {
        case Situation::Flight:
            // Loftwing (d_a_birdNP fn_129_5BF0): yaw rate from the remote's roll (Y'.x, clamped at 0.5 = 30 deg), pitch
            // from the pointing angle (-45..+60 deg; down = dive, faster). Stick up = dive (HD), flight_invert_y flips.
            if (!ctrl18_rule_on("FLIGHT_STEER")) return false;
            steer_pose(o, 0.0, -double(in.my) * inv * 0.70, double(in.mx) * 0.55, dt, 6.0f, out.pitch, out.yaw, out.roll);
            return true;
        case Situation::Swim:
            // Under water (fn_802177A0 / fn_80216060): pitch = pointing, yaw = pointing yaw or roll. Stick up = down.
            if (!ctrl18_rule_on("SWIM_STEER")) return false;
            steer_pose(o, sx * 0.75, -sy * inv * 0.75, 0.0, dt, 6.0f, out.pitch, out.yaw, out.roll);
            return true;
        case Situation::Skydive:
            // Free fall (ActorLink__doFreeFall): pitch -30..+50 deg, roll +-45 deg relative to the fall start.
            if (!ctrl18_rule_on("SKYDIVE_STEER")) return false;
            steer_pose(o, entry_yaw_, entry_pitch_ - double(in.my) * inv * 0.70, double(in.mx) * 0.70, dt, 6.0f,
                       out.pitch, out.yaw, out.roll);
            return true;
        case Situation::Balance:
            // Tightrope (fn_80229F20): the pointing yaw past 16.9 deg leans Link; the stick points left / right.
            if (!ctrl18_rule_on("BALANCE_STEER")) return false;
            steer_pose(o, double(in.mx) * 0.50, 0.0, 0.0, dt, 6.0f, out.pitch, out.yaw, out.roll);
            return true;
        case Situation::Beetle:
            // Beetle flight (dAcBoomerang_c executeState_Move): turn = pointing yaw / roll vs the launch pose, pitch =
            // pointing. Stick up = up.
            if (!ctrl18_rule_on("BEETLE_STEER")) return false;
            steer_pose(o, entry_yaw_ + sx * 0.6, entry_pitch_ + sy * 0.5, 0.0, dt, 6.0f, out.pitch, out.yaw, out.roll);
            return true;
        default:
            return false;
        }
    }

    // Carrying (a bomb, a pot, ...): stance +1 = up (throw), -1 = down (roll), 0 = none; `release` fires the stance.
    // The game (fn_80203C20 / fn_80203EA0) throws overhand with the remote >= 45 deg up and rolls with it >= 45 deg
    // down when any swing bit comes; the pose is held 0.25 s first (its height state follows at 0.1 per frame).
    bool carry(int stance, bool release, double dt, const VirtualRemoteOrientation& o, VirtualGestureController& g,
               WiiOut& out) {
        if (!ctrl18_rule_on("CARRY_THROW") || !(dt > 0.0 && dt <= 0.1)) return false;
        switch (carry_) {
        case CarryStep::Idle:
            if (stance != 0) { carry_ = CarryStep::Stance; carry_dir_ = stance; carry_t_ = 0; }
            break;
        case CarryStep::Stance:
            if (stance != 0 && stance != carry_dir_) { carry_dir_ = stance; carry_t_ = 0; }
            if (release || stance == 0) carry_ = CarryStep::Fire;
            break;
        case CarryStep::Fire:
        case CarryStep::Done:
            break;
        }
        if (carry_ == CarryStep::Stance || carry_ == CarryStep::Fire) {
            carry_t_ += dt;
            if (carry_ == CarryStep::Fire && carry_t_ >= 0.25 && !g.remote_active()) {
                g.swing(0.0, -carry_dir_); // from up: down (overhand); from down: up (underhand)
                ctrl18_note_move(carry_dir_ > 0 ? "Gooien" : "Rollen");
                carry_ = CarryStep::Done;
                rest_pending_ = true;
                return false;
            }
            steer_pose(o, 0.0, carry_dir_ * 1.05, 0.0, dt, 6.0f, out.pitch, out.yaw, out.roll);
            return true;
        }
        if (carry_ == CarryStep::Done && stance == 0 && !release && !g.remote_active()) carry_ = CarryStep::Idle;
        return false;
    }

    // REST after another situation (and after a throw): turn the remote back once nothing else moves it.
    void rest(Situation s, const VirtualRemoteOrientation& o, VirtualGestureController& g) {
        if (rest_pending_ && !g.remote_active() && (s == Situation::Play || s == Situation::Climb)) {
            rest_pending_ = false;
            if (rest_error(o) > 0.01) g.recenter();
        }
    }
    void request_rest() { rest_pending_ = true; }

private:
    enum class CarryStep { Idle, Stance, Fire, Done };
    CoreIn prev_{};
    Situation last_ = Situation::Menu;
    bool dash_latched_ = false, cancel_latched_ = false, b_cancel_latched_ = false, rest_pending_ = false,
         flap_right_ = true;
    double z_tap_left_ = 0.0, entry_yaw_ = 0.0, entry_pitch_ = 0.0;
    CarryStep carry_ = CarryStep::Idle;
    int carry_dir_ = 0;
    double carry_t_ = 0;
};

// ---- switch-hd pad translator ----
struct PadIntents {
    bool held[kHdActionCount] = {};
    float lx = 0, ly = 0;       // left stick, deadzone applied, y up, length <= 1
    float rx = 0, ry = 0;       // right stick, raw -1..1, y up
    float right_dead = 8000.0f / 32767.0f;
    float aim_speed = 1.0f;     // [input] aim_stick_speed
    bool gyro_aim = false;      // HD "motion aiming" hybrid active
    bool gyro_valid = false;
    float gyro_rate[3] = {0, 0, 0}; // measured angular integral / host interval (pitch, yaw, roll)
    bool owns_pointer = true; // menu correction belongs to exactly one live device
    bool gyro_recentre = false;     // the source recentred the gyro this update (Y / touchpad with gyro on)
    double dt = 0;
};

class HdPadTranslator {
public:
    bool a_does_not_dash = true;

    void reset() { *this = HdPadTranslator{}; }

    void step(const PadIntents& in, const GameContext& ctx, const LiveFlags& live, VirtualGestureController& g,
              const VirtualRemoteOrientation& o, const PointerAimLoop& aim, WiiOut& out) {
        out = WiiOut{};
        const double dt = in.dt;
        const bool dt_ok = dt > 0.0 && dt <= 0.1;
        const Situation s = core_.begin(ctx, live, o);
        out.situation = s;
        core_.a_does_not_dash = a_does_not_dash;
        const bool camera_held = in.held[kHdCamera] && s != Situation::Menu && s != Situation::Puzzle;
        if (s == Situation::Puzzle && last_s_ != Situation::Puzzle) g.reset();
        last_s_ = s;

        CoreIn ci;
        ci.action = in.held[kHdAction]; ci.dash = in.held[kHdDash]; ci.item = in.held[kHdItem];
        ci.pouch = in.held[kHdPouch]; ci.harp = in.held[kHdHarp]; ci.recentre = in.held[kHdRecentre];
        ci.shield = in.held[kHdShield]; ci.thrust = in.held[kHdThrust]; ci.lock_on = in.held[kHdLockOn];
        ci.map = in.held[kHdMap]; ci.gear = in.held[kHdGear]; ci.first_person = in.held[kHdFirstPerson];
        ci.fi = in.held[kHdFi]; ci.help = in.held[kHdHelp]; ci.dpad_left = in.held[kHdDpadLeft]; ci.home = in.held[kHdHome];
        ci.mx = in.lx; ci.my = in.ly;
        ci.gyro_aim = in.gyro_aim;
        ci.dt = dt_ok ? dt : 0.0;
        const bool vehicle = s == Situation::Swim || s == Situation::Beetle;
        if (vehicle && !camera_held) { ci.steer_x = in.rx; ci.steer_y = in.ry; }
        core_.buttons(ci, ctx, s, g, out);

        // Right stick: sword (play-like situations, not while the camera button is held).
        const bool sword_enabled = (s == Situation::Play || s == Situation::Climb || s == Situation::Balance) &&
                                   !camera_held && ctrl18_rule_on("STICK_SWORD");
        SwordEvent ev[3];
        const int nev = sword_.update(in.rx, in.ry, dt_ok ? dt : 0.0, sword_enabled, ev);
        for (int k = 0; k < nev; ++k) core_.sword_event(ev[k], ctx, g);
        if (sword_.busy() && sword_enabled) out.claim_aim = true;
        if (s != Situation::Menu && s != Situation::Event) menu_synced_ = false;

        const float rdx = aim_axis(in.rx, in.right_dead), rdy = aim_axis(in.ry, in.right_dead);
        const bool right_active = rdx != 0.0f || rdy != 0.0f;
        float pitch = 0, yaw = 0, roll = 0;
        if (s == Situation::Menu || s == Situation::Event) {
            // The game's pointer: the stick moves a screen target, the remote is steered onto it (INPUT13 menu cursor).
            if (!in.owns_pointer) menu_synced_ = false;
            if (live.motionplus_pointer && dt_ok && !camera_held && in.owns_pointer) {
                if (!menu_synced_) {
                    float px = 0, py = 0;
                    if (aim.game_pointer(o, px, py) && std::fabs(px) <= 1.0f && std::fabs(py) <= 1.0f) {
                        cursor_x_ = px; cursor_y_ = py;
                    } else {
                        cursor_x_ = cursor_y_ = 0.0f;
                    }
                    menu_synced_ = true;
                }
                if (live.point_center || (in.held[kHdRecentre] && in.gyro_aim)) cursor_x_ = cursor_y_ = 0.0f;
                if (in.gyro_aim && in.gyro_valid && !live.point_center && !in.held[kHdRecentre]) {
                    cursor_x_ -= float(in.gyro_rate[1] * dt / kPointerHalfRangeRad);
                    cursor_y_ += float(in.gyro_rate[0] * dt / kPointerHalfRangeRad);
                }
                const float speed = 1.6f * in.aim_speed;
                cursor_x_ = std::clamp(cursor_x_ + rdx * speed * float(dt), -1.0f, 1.0f);
                cursor_y_ = std::clamp(cursor_y_ - rdy * speed * float(dt), -1.0f, 1.0f);
                steer_pointer(o, aim, cursor_x_, cursor_y_, dt, pitch, yaw, roll);
                out.steering = true;
            }
            if (right_active) out.claim_aim = true;
        } else if (s == Situation::Puzzle) {
            // Fine incremental 3-axis rotation: release the sticks to hold the pose.
            // L + right-stick horizontal twists about the pointing axis; left-stick
            // horizontal is an additional twist input. No screen-range clamp or leveling.
            const float rate = 1.4f * in.aim_speed;
            pitch = -rdy * rate;
            yaw = in.held[kHdCamera] ? 0.0f : -rdx * rate;
            roll = -(in.held[kHdCamera] ? rdx : aim_axis(in.lx, 0.0f)) * rate;
            if (in.gyro_aim && in.gyro_valid && in.owns_pointer) {
                pitch += in.gyro_rate[0]; yaw += in.gyro_rate[1]; roll += in.gyro_rate[2];
            }
            out.stick_x = out.stick_y = 0.0f;
            out.steering = true;
            if (right_active || in.lx != 0.0f) out.claim_aim = true;
        } else if (s == Situation::Wheel) {
            // Radial pick: the stick direction tilts the game's pointing direction toward that slot; a released stick
            // keeps the last pick (release ZR / R afterwards; no pick yet = no selection).
            const float m = std::sqrt(in.rx * in.rx + in.ry * in.ry);
            if (m > 0.5f) { wheel_left_ = -in.rx / m; wheel_up_ = in.ry / m; wheel_set_ = true; }
            if (dt_ok && wheel_set_ && ctrl18_rule_on("WHEEL")) {
                steer_wheel(o, ctx, wheel_left_, wheel_up_, dt, pitch, yaw, roll);
                out.steering = true;
            }
            out.claim_aim = true;
        } else if (s == Situation::Aim && !camera_held) {
            const float rate = 4.0f * in.aim_speed;
            pitch = -rdy * rate; // stick up = nose up (+pitch = nose down)
            yaw = -rdx * rate;   // stick right = turn right (+yaw = turn left)
            if (in.gyro_aim && in.gyro_valid && in.owns_pointer && ctrl18_rule_on("GYRO_AIM")) {
                pitch += in.gyro_rate[0];
                yaw += in.gyro_rate[1];
            }
            if (in.held[kHdRecentre] && in.gyro_aim && dt_ok) {
                steer_pointer(o, aim, 0.0, 0.0, dt, pitch, yaw, roll);
            } else if (dt_ok && (pitch != 0.0f || yaw != 0.0f)) {
                double mm[3][3];
                o.matrix(mm);
                roll = VirtualRemoteOrientation::level_roll_rate(mm, dt, kPointerMotionMaxRateRadS);
            }
            out.steering = true;
            if (right_active) out.claim_aim = true;
        } else if (s == Situation::Carry) {
            const float m = std::sqrt(in.rx * in.rx + in.ry * in.ry);
            const int stance = m > 0.6f && std::fabs(in.ry) > std::fabs(in.rx) ? (in.ry > 0 ? 1 : -1) : 0;
            const bool release = item_prev_ && !in.held[kHdItem];
            WiiOut pose;
            if (core_.carry(stance, release, dt, o, g, pose)) { pitch = pose.pitch; yaw = pose.yaw; roll = pose.roll; out.steering = true; }
        } else {
            WiiOut pose;
            if (core_.steer(ci, s, o, pose)) { pitch = pose.pitch; yaw = pose.yaw; roll = pose.roll; out.steering = true; }
        }
        if (s != Situation::Wheel) wheel_set_ = false;
        item_prev_ = in.held[kHdItem];
        core_.rest(s, o, g);
        out.pitch = pitch;
        out.yaw = yaw;
        out.roll = roll;
        if (out.buttons || out.nunchuk_buttons) out.claim_aim = true;
    }

    const SwordRecognizer& sword() const { return sword_; }
    SwordRecognizer& sword() { return sword_; }
    Ctrl18Core& core() { return core_; }

private:
    Ctrl18Core core_;
    SwordRecognizer sword_;
    Situation last_s_ = Situation::Menu;
    bool menu_synced_ = false, wheel_set_ = false, item_prev_ = false;
    float cursor_x_ = 0, cursor_y_ = 0, wheel_left_ = 0, wheel_up_ = 0;
};

// ---- kbm (keyboard + mouse) translator (design section 4, track T5) ----
// Modes follow the situation: Menu/Event = the mouse is free and absolute (the game's pointer, closed loop; left
// button = A, right = B); Play = the mouse is captured (relative) and turns the camera, the left button is the sword
// (click = slash, hold + drag = directed slash / raise / spin; MouseSwordStick + SwordRecognizer); Aim = the captured
// mouse aims (remote rates), left button = fire; Wheel = the mouse direction picks a slot; Flight/Swim/Skydive/
// Balance/Beetle = WASD steer the remote; Carry = left button + drag up / down, release = throw / roll.
struct KbmIntents {
    bool held[kKbmActionCount] = {};
    float mouse_dx = 0, mouse_dy = 0; // relative motion this update (px, x right / y down); valid while captured
    bool captured = false;            // the mouse was captured (relative) during this update
    float abs_x = 0, abs_y = 0;       // absolute mouse in the game picture (-1..1, x right / y down)
    bool mouse_owns_pointer = false;  // the mouse was used last (g_aim_owner == mouse)
    double dt = 0;
};
struct KbmOut : WiiOut {
    bool want_capture = false; // capture (relative mode) for the next update
    float cam_dx = 0, cam_dy = 0; // mouse pixels for the free camera
};

// Quick items (keys 1-8, decision D5): hold Wii B until the item wheel is open, steer the game's pointing direction to
// the slot (keys clockwise from the top, as the wheel is drawn), release B once the game highlights that slot (LINK
// +0x43B1, read-only) or after 0.5 s. Timeout 1.2 s without the wheel.
struct QuickItemMacro {
    int key = -1;            // 0..7 active (clockwise from the top), -1 idle
    double t = 0, open_t = -1;
    void start(int k) { key = k; t = 0; open_t = -1; }
    bool active() const { return key >= 0; }
    int index() const { return key < 0 ? -1 : (8 - key) % 8; } // the game's wheel index (counted toward the left)
    // Returns true while B must be held; dir_left / dir_up = the wheel direction once the wheel is open.
    bool step(const GameContext& ctx, double dt, bool& aim, double& dir_left, double& dir_up) {
        aim = false;
        if (key < 0) return false;
        t += dt;
        const bool open = (ctx.sword_states & kSwordBWheelOpen) != 0;
        if (open && open_t < 0) open_t = t;
        if (open_t < 0) {
            if (t > 1.2) { key = -1; return false; }
            return true;
        }
        wheel_dir_of(index(), dir_left, dir_up);
        aim = true;
        if ((ctx.wheel_index == index() && t - open_t > 0.1) || t - open_t > 0.5 || !open) {
            key = -1; // release B: the game equips the highlighted slot
            return false;
        }
        return true;
    }
};

class KbmTranslator {
public:
    bool a_does_not_dash = true;
    bool quick_items = true;
    float aim_sensitivity = 1.0f;
    bool invert_y = false;

    void reset() { *this = KbmTranslator{}; }

    void step(const KbmIntents& in, const GameContext& ctx, const LiveFlags& live, VirtualGestureController& g,
              const VirtualRemoteOrientation& o, const PointerAimLoop& aim, KbmOut& out) {
        out = KbmOut{};
        const double dt = in.dt;
        const bool dt_ok = dt > 0.0 && dt <= 0.1;
        const Situation s = core_.begin(ctx, live, o);
        out.situation = s;
        core_.a_does_not_dash = a_does_not_dash;
        const auto held = [&](int a) { return in.held[a]; };
        const auto edge = [&](int a) { return in.held[a] && !prev_[a]; };
        const auto released = [&](int a) { return !in.held[a] && prev_[a]; };
        const bool menu = s == Situation::Menu || s == Situation::Event;
        out.want_capture = !menu && ctrl18_rule_on("KBM_CAPTURE");

        // Movement keys -> Nunchuk stick (normalised; Alt = walk slowly).
        float sx = float(held(kKbmRight)) - float(held(kKbmLeft));
        float sy = float(held(kKbmUp)) - float(held(kKbmDown));
        if (const float len = std::sqrt(sx * sx + sy * sy); len > 1.0f) { sx /= len; sy /= len; }
        if (held(kKbmWalk)) { sx *= 0.5f; sy *= 0.5f; }
        const bool moving = sx * sx + sy * sy > 0.04f;

        CoreIn ci;
        ci.action = held(kKbmAction);
        ci.dash = held(kKbmDash);
        ci.item = held(kKbmItem);
        ci.pouch = held(kKbmPouch);
        ci.harp = held(kKbmHarp);
        ci.thrust = held(kKbmThrust) && !menu;
        ci.lock_on = held(kKbmLockOn);
        ci.map = held(kKbmMap);
        ci.gear = held(kKbmGear);
        ci.first_person = held(kKbmFirstPerson);
        ci.fi = held(kKbmFi) || held(kKbmDpadDown);
        ci.help = held(kKbmHelp);
        ci.dpad_left = held(kKbmDpadLeft);
        ci.dpad_up = held(kKbmDpadUp);
        ci.dpad_right = held(kKbmDpadRight);
        ci.home = held(kKbmHome);
        ci.skip = held(kKbmSkip);
        ci.mx = sx;
        ci.my = sy;
        ci.dt = dt_ok ? dt : 0.0;
        // Mouse buttons by situation: menus A / B; aim: fire (A with a firing item, else B) / cancel (HD B rules);
        // Loftwing: left = Spiral Charge (A); beetle: left = boost (A), right = recall (B); play: sword / shield.
        if (menu || s == Situation::Puzzle) {
            if (held(kKbmSlash)) out.buttons |= kWiiA;
            if (held(kKbmShield)) out.buttons |= kWiiB;
        } else if (s == Situation::Aim) {
            if (held(kKbmSlash)) out.buttons |= item_fires_with_a(ctx.item_state) && ctrl18_rule_on("ITEM_A") ? kWiiA : kWiiB;
            ci.dash = ci.dash || held(kKbmShield);
        } else if (s == Situation::Flight) {
            ci.harp = ci.harp || held(kKbmSlash);
            ci.dash = ci.dash || held(kKbmShield);
        } else if (s == Situation::Beetle) {
            ci.item = ci.item || held(kKbmSlash);
            ci.dash = ci.dash || held(kKbmShield);
        } else if (s == Situation::Wheel) {
        } else {
            ci.shield = held(kKbmShield);
        }
        // Space: Loftwing = flap; running = roll (Nunchuk shake); otherwise A (jump at a ledge, hop with Z).
        if (held(kKbmDodge) && !menu) {
            if (s == Situation::Flight) {
                ci.action = true;
            } else if (edge(kKbmDodge) && moving && ((ctx.sword_states & kSwordSprinting) || a_is_dash(ctx) ||
                                                      a_action(ctx) == kPromptRoll)) {
                g.nunchuk_flick();
                ctrl18_note_move("Rol");
            } else if (!(ctx.sword_states & kSwordSprinting) && !(s == Situation::Play && a_is_dash(ctx))) {
                out.buttons |= kWiiA;
            }
        }
        if (s == Situation::Flight && held(kKbmAction)) ci.action = true; // E flaps too
        core_.buttons(ci, ctx, s, g, out);

        // ---- mouse sword (play-like) ----
        const bool slash = held(kKbmSlash) && !menu;
        const bool play_like = s == Situation::Play || s == Situation::Climb || s == Situation::Balance;
        const bool sword_on = play_like && ctrl18_rule_on("MOUSE_SWORD");
        if (s != last_) {
            if (s != Situation::Play) { stick_.reset(); slash_used_ = true; }
            last_ = s;
        }
        if (sword_on && slash) {
            if (edge(kKbmSlash)) {
                stick_.reset();
                slash_used_ = false;
                slash_t_ = 0;
                // A slash while the mouse is already moving fast: at once, in that direction.
                const float vx = recent_dx_, vy = recent_dy_;
                if (vx * vx + vy * vy > 30.0f * 30.0f && ctrl18_rule_on("MOUSE_SLASH_MOVING")) {
                    SwordEvent e;
                    e.kind = SwordEvent::Swing;
                    SwordRecognizer::sector_dir(SwordRecognizer::sector8(vx, -vy), e.dx, e.dy);
                    core_.sword_event(e, ctx, g);
                    slash_used_ = true;
                }
            }
            if (in.captured) stick_.add(in.mouse_dx, in.mouse_dy);
            slash_t_ += dt_ok ? dt : 0;
        } else {
            stick_.reset();
        }
        SwordEvent ev[3];
        const int nev = sword_.update(stick_.x(), stick_.y(), dt_ok ? dt : 0.0, sword_on, ev);
        for (int k = 0; k < nev; ++k) {
            core_.sword_event(ev[k], ctx, g);
            if (ev[k].kind == SwordEvent::Swing || ev[k].kind == SwordEvent::Spin) { stick_.reset(); slash_used_ = true; }
            if (ev[k].kind == SwordEvent::RaiseOn) slash_used_ = true;
        }
        // A plain click (released without a drag): alternate right / left horizontal slashes.
        if (sword_on && released(kKbmSlash) && !slash_used_ && slash_t_ < 0.5) {
            SwordEvent e;
            e.kind = SwordEvent::Swing;
            e.dx = click_right_ ? 1.0f : -1.0f;
            click_right_ = !click_right_;
            core_.sword_event(e, ctx, g);
        }
        if (sword_on) {
            if (edge(kKbmSpin)) {
                Ctrl18Core::spin(g, false);
                ctrl18_note_move(fatal_blow_shown(ctx) ? "Genadeslag" : "Draaiaanval (horizontaal)");
            }
            if (held(kKbmRaise) != raise_key_) {
                raise_key_ = held(kKbmRaise);
                g.set_raised(raise_key_);
                if (raise_key_) ctrl18_note_move("Zwaard omhoog (hemelslag laden)");
            }
        } else if (raise_key_) {
            raise_key_ = false;
            g.set_raised(false);
        }
        // Recent mouse velocity (px per ~80 ms) for the "slash while moving".
        if (dt_ok) {
            const float k = float(std::min(1.0, dt / 0.08));
            const float ix = in.captured ? in.mouse_dx : 0.0f, iy = in.captured ? in.mouse_dy : 0.0f;
            recent_dx_ = recent_dx_ * (1.0f - k) + ix * (0.08f / float(dt)) * k;
            recent_dy_ = recent_dy_ * (1.0f - k) + iy * (0.08f / float(dt)) * k;
        }

        // Quick items 1-8.
        if (quick_items && s == Situation::Play && !quick_.active()) {
            for (int k = 0; k < 8; ++k)
                if (edge(kKbmItem1 + k)) {
                    quick_.start(k);
                    char text[32];
                    std::snprintf(text, sizeof text, "Voorwerp %d", k + 1);
                    ctrl18_note_move(text);
                    break;
                }
        }
        bool quick_aim = false;
        double ql = 0, qu = 0;
        if (quick_.active() && ctrl18_rule_on("QUICK_ITEMS")) {
            if (quick_.step(ctx, dt_ok ? dt : 0.0, quick_aim, ql, qu)) out.buttons |= kWiiB;
        }

        float pitch = 0, yaw = 0, roll = 0;
        if (s == Situation::Puzzle) {
            if (in.captured && in.mouse_owns_pointer && dt_ok) {
                const float rpp = 0.004f * aim_sensitivity;
                pitch = in.mouse_dy * rpp / float(dt);
                yaw = -in.mouse_dx * rpp / float(dt);
            }
            out.stick_x = out.stick_y = 0.0f;
            out.steering = true;
        } else if (menu) {
            // The game's pointer under the mouse (closed loop, SOAK14), as the classic mouse does.
            if (live.motionplus_pointer && in.mouse_owns_pointer && dt_ok) {
                steer_pointer(o, aim, std::clamp(double(in.abs_x), -1.0, 1.0), std::clamp(double(in.abs_y), -1.0, 1.0), dt,
                              pitch, yaw, roll);
                out.steering = true;
            }
        } else if (s == Situation::Wheel || quick_aim) {
            if (quick_aim) {
                if (dt_ok) { steer_wheel(o, ctx, ql, qu, dt, pitch, yaw, roll); out.steering = true; }
            } else {
                if (in.captured) { wheel_ax_ += in.mouse_dx; wheel_ay_ += in.mouse_dy; }
                const float m = std::sqrt(wheel_ax_ * wheel_ax_ + wheel_ay_ * wheel_ay_);
                if (m > 25.0f) {
                    wheel_left_ = -wheel_ax_ / m;
                    wheel_up_ = -wheel_ay_ / m;
                    wheel_set_ = true;
                    if (m > 80.0f) { wheel_ax_ *= 80.0f / m; wheel_ay_ *= 80.0f / m; }
                }
                if (dt_ok && wheel_set_ && ctrl18_rule_on("WHEEL")) {
                    steer_wheel(o, ctx, wheel_left_, wheel_up_, dt, pitch, yaw, roll);
                    out.steering = true;
                }
            }
        } else if (s == Situation::Aim) {
            if (dt_ok && in.captured) {
                const float rpp = 0.008f * aim_sensitivity;
                pitch = std::clamp(in.mouse_dy * (invert_y ? -1.0f : 1.0f) * rpp / float(dt), -kPointerMotionMaxRateRadS,
                                   kPointerMotionMaxRateRadS);
                yaw = std::clamp(-in.mouse_dx * rpp / float(dt), -kPointerMotionMaxRateRadS, kPointerMotionMaxRateRadS);
                if (pitch != 0.0f || yaw != 0.0f) {
                    double mm[3][3];
                    o.matrix(mm);
                    roll = VirtualRemoteOrientation::level_roll_rate(mm, dt, kPointerMotionMaxRateRadS);
                }
            }
            out.steering = true;
        } else if (s == Situation::Carry) {
            // Left button held + drag up / down = the stance; let go = throw / roll.
            if (slash && in.captured) { carry_ay_ += in.mouse_dy; carry_ay_ = std::clamp(carry_ay_, -80.0f, 80.0f); }
            if (!slash) carry_ay_ = 0.0f;
            const int stance = carry_ay_ < -30.0f ? 1 : carry_ay_ > 30.0f ? -1 : 0;
            if (stance != 0) carry_stance_ = stance;
            const bool release = released(kKbmSlash) && carry_stance_ != 0;
            WiiOut pose;
            if (core_.carry(slash ? stance : (release ? carry_stance_ : 0), release, dt, o, g, pose)) {
                pitch = pose.pitch; yaw = pose.yaw; roll = pose.roll; out.steering = true;
            }
            if (!slash && !release) carry_stance_ = 0;
        } else {
            WiiOut pose;
            if (core_.steer(ci, s, o, pose)) { pitch = pose.pitch; yaw = pose.yaw; roll = pose.roll; out.steering = true; }
        }
        if (s != Situation::Wheel) { wheel_ax_ = wheel_ay_ = 0.0f; wheel_set_ = false; }
        // The camera: mouse motion in play, except while the sword drag uses it or Z is held.
        if ((s == Situation::Play || s == Situation::Climb) && in.captured && !slash && !held(kKbmLockOn)) {
            out.cam_dx = in.mouse_dx;
            out.cam_dy = in.mouse_dy;
        }
        core_.rest(s, o, g);
        out.pitch = pitch;
        out.yaw = yaw;
        out.roll = roll;
        for (int a = 0; a < kKbmActionCount; ++a) prev_[a] = in.held[a];
    }

    const SwordRecognizer& sword() const { return sword_; }
    SwordRecognizer& sword() { return sword_; }
    Ctrl18Core& core() { return core_; }

private:
    static SwordTuning mouse_tuning() {
        SwordTuning t;
        t.neutral = 0.15f;  // 9 px of 60
        t.rim = 0.5f;       // 30 px drag = a slash (design: hold + drag > 30 px)
        t.flick_time = 0.30;
        t.raise_time = 0.25;
        return t;
    }

    Ctrl18Core core_;
    SwordRecognizer sword_ = [] { SwordRecognizer r; r.tuning = mouse_tuning(); return r; }();
    MouseSwordStick stick_;
    QuickItemMacro quick_;
    bool prev_[kKbmActionCount] = {};
    bool raise_key_ = false, click_right_ = true, slash_used_ = true, wheel_set_ = false;
    double slash_t_ = 0;
    float recent_dx_ = 0, recent_dy_ = 0, carry_ay_ = 0;
    int carry_stance_ = 0;
    Situation last_ = Situation::Menu;
    float wheel_ax_ = 0, wheel_ay_ = 0, wheel_left_ = 0, wheel_up_ = 0;
};

} // namespace nwii::runtime::input
