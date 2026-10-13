// CTRL18-INPUT: the "switch-hd" controller preset and the "kbm" keyboard+mouse preset through the PRODUCTION sources
// (gamepad_source.cpp, mouse_keyboard_source.cpp), the production InputManager and the context translator
// (hd_translator.h), with SDL mocked at the API boundary (pattern of controls/bindings-test.cpp) and the game context
// published like ss_boot.cpp publish_game_context does. The ORIGINAL-game-code half (stick sword, Loftwing, carry,
// wheel, ...) is controls/swing-test.cpp --hd (H1-H15).
// IDs (lines "FAIL: Un ..."): U1 face buttons by position, U2 every switch-hd intent -> its Wii signal, U3 HD B rules,
// U4 "A does not dash", U5 Y = camera behind Link (Z tap), U6 - = map / cutscene skip, U7 the right stick in play
// does not move the remote (no drift; the classic preset does), U8 L + right stick = free camera (CTRL18-CAM channel),
// U9 aim + items fire with A + back to REST, U10 wheel direction, U11 menu cursor, U12 gyro aiming hybrid,
// U13 kbm keys + mouse capture, U14 kbm mouse (camera, slash, drag, shield, thrust, aim), U15 quick items 1-8,
// U16 bindings ([input.gamepad_hd] rebind, preset swap at runtime), U17 config keys, U18 live status + device
// tracking (rumble routing, wii-motion = no free camera).
// Negative runs (build-ctrl18-test.cmd): --neg-context (no context published), NWII_CTRL18_B_RULES=0,
// NWII_CTRL18_KBM_CAPTURE=0 - expected failing IDs in outputs/pc-tests/run_all.py.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "input/input_manager.h"
#include "input/sources/gamepad_source.h"
#include "input/sources/mouse_keyboard_source.h"
#include "input/hd_translator.h"
#include "input/game_context.h"
#include "input/gyro_calibration.h"
#include "input/motion_gesture.h"
#include "input/virtual_motion.h"
#include "runtime/camera_stick.h"
#include "runtime/config.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace nwii::runtime;
using namespace nwii::runtime::input;

static std::array<Uint8, SDL_NUM_SCANCODES> keys{};
static std::array<Uint8, SDL_CONTROLLER_BUTTON_MAX> buttons{};
static std::array<Sint16, SDL_CONTROLLER_AXIS_MAX> axes{};
static bool connected = true, focused = true, has_sensors = false;
static int mouse_x = 320, mouse_y = 240, rel_x = 0, rel_y = 0;
static Uint32 mouse_buttons = 0;
static int relative_mode = -1, relative_calls = 0; // last SDL_SetRelativeMouseMode argument
static SDL_EventFilter watch_fn = nullptr;

extern "C" {
SDL_Window* SDLCALL SDL_GetKeyboardFocus() { return focused ? reinterpret_cast<SDL_Window*>(uintptr_t(1)) : nullptr; }
SDL_Window* SDLCALL SDL_GetMouseFocus() { return nullptr; }
void SDLCALL SDL_GetWindowSize(SDL_Window*, int*, int*) {}
int SDLCALL SDL_SetRelativeMouseMode(SDL_bool on) { relative_mode = on ? 1 : 0; ++relative_calls; return 0; }
Uint32 SDLCALL SDL_GetRelativeMouseState(int* x, int* y) { *x = rel_x; *y = rel_y; rel_x = rel_y = 0; return 0; }
SDL_bool SDLCALL SDL_IsGameController(int index) { return index == 0 && connected ? SDL_TRUE : SDL_FALSE; }
SDL_GameController* SDLCALL SDL_GameControllerOpen(int index) {
    return SDL_IsGameController(index) ? reinterpret_cast<SDL_GameController*>(uintptr_t(1)) : nullptr;
}
Uint8 SDLCALL SDL_GameControllerGetButton(SDL_GameController*, SDL_GameControllerButton b) { return buttons.at(b); }
Sint16 SDLCALL SDL_GameControllerGetAxis(SDL_GameController*, SDL_GameControllerAxis a) { return axes.at(a); }
SDL_bool SDLCALL SDL_GameControllerHasSensor(SDL_GameController*, SDL_SensorType) { return has_sensors ? SDL_TRUE : SDL_FALSE; }
int SDLCALL SDL_GameControllerSetSensorEnabled(SDL_GameController*, SDL_SensorType, SDL_bool) { return 0; }
int SDLCALL SDL_GameControllerGetSensorData(SDL_GameController*, SDL_SensorType, float*, int) { return -1; }
void SDLCALL SDL_AddEventWatch(SDL_EventFilter f, void*) { watch_fn = f; }
SDL_JoystickID SDLCALL SDL_JoystickGetDeviceInstanceID(int index) { return index; }
int SDLCALL SDL_GameControllerRumble(SDL_GameController*, Uint16, Uint16, Uint32) { return 0; }
const char* SDLCALL SDL_GameControllerName(SDL_GameController*) { return "mock controller"; }
Uint32 SDLCALL SDL_GetMouseState(int* x, int* y) { *x = mouse_x; *y = mouse_y; return mouse_buttons; }
const Uint8* SDLCALL SDL_GetKeyboardState(int* size) { if (size) *size = int(keys.size()); return keys.data(); }
}

static unsigned checks = 0, failures = 0, ufail[20] = {};
static void check(int id, bool v, const std::string& what) {
    ++checks;
    if (!v) { ++failures; ++ufail[id]; std::printf("  CHECK FAILED: U%d %s\n", id, what.c_str()); }
}

static bool publish_context = true;
static GameContext ctx_now;
static void context(GameContext c) {
    c.valid = true;
    c.player = true;
    static uint32_t frame = 0;
    c.frame = ++frame;
    ctx_now = c;
    g_guest_player_present.store(c.player);
    if (publish_context) g_guest_context.publish(c);
}
static uint64_t now_ns = 1'000'000'000ull;
static uint64_t sensor_us = 0;
static double sensor_rate = 1000, sensor_budget = 0;
static double gyro_yaw_rate = 0.0; // SDL gyro +Y = turn left (rad/s), pad flat
static void push_sensors(double seconds) {
    if (!has_sensors || !watch_fn) return;
    sensor_budget += seconds * sensor_rate;
    const int n = int(std::floor(sensor_budget + 1e-8));
    sensor_budget -= n;
    for (int k = 0; k < n; ++k) {
        sensor_us += uint64_t(std::llround(1e6 / sensor_rate));
        SDL_Event e{};
        e.type = SDL_CONTROLLERSENSORUPDATE;
        e.csensor.which = 0;
        e.csensor.timestamp_us = sensor_us;
        e.csensor.sensor = SDL_SENSOR_GYRO;
        e.csensor.data[0] = 0.0f; e.csensor.data[1] = float(gyro_yaw_rate); e.csensor.data[2] = 0.0f;
        watch_fn(nullptr, &e);
        e.csensor.sensor = SDL_SENSOR_ACCEL;
        e.csensor.data[0] = 0.0f; e.csensor.data[1] = 9.80665f; e.csensor.data[2] = 0.0f;
        watch_fn(nullptr, &e);
    }
}
static WiimoteState last;
static uint32_t buttons_seen = 0, nunchuk_seen = 0;
static void step(int n = 1) {
    for (int k = 0; k < n; ++k) {
        now_ns += 8'333'333ull; // 120 Hz
        push_sensors(1.0 / 120);
        if (publish_context) { ctx_now.frame++; g_guest_context.publish(ctx_now); }
        InputManager::get().update_at(now_ns);
        last = InputManager::get().get_wiimote_state(0);
        buttons_seen |= last.buttons;
        nunchuk_seen |= last.nunchuk.buttons;
    }
}
static double seconds(double s) { const int n = int(std::lround(s * 120)); step(n); return s; }
static double rest_err() { return rest_error(g_virtual_orientation[0]); }
static void release_all() {
    keys.fill(0); buttons.fill(0); axes.fill(0); mouse_buttons = 0; rel_x = rel_y = 0; gyro_yaw_rate = 0;
    step(4);
}
static void clean(GameContext c = {}) {
    release_all();
    context(c);
    step(2);
    g_virtual_gesture[0].reset();
    g_virtual_orientation[0].reset();
    step(2);
    buttons_seen = nunchuk_seen = 0;
}
static void set_presets(const char* pad, const char* kb) {
    auto& cfg = Config::get();
    cfg.preset_gamepad = pad;
    cfg.preset_keyboard = kb;
    request_input_bindings_reload();
    step(2);
}
static int face(int position) { return face_button_for(position, 0); } // the mock pad reports type 0 (Xbox-like)

int main(int argc, char** argv) {
    for (int a = 1; a < argc; ++a)
        if (std::string(argv[a]) == "--neg-context") publish_context = false;
    std::printf("CTRL18 production-source test%s\n", publish_context ? "" : " (NEGATIVE: no game context published)");
    auto& cfg = Config::get();
    cfg.input_mode = 6;
    cfg.preset_gamepad = "switch-hd";
    cfg.preset_keyboard = "kbm";
    reload_input_bindings(false);
    auto& im = InputManager::get();
    im.add_source(std::make_unique<MouseKeyboardSource>());
    im.add_source(std::make_unique<GamepadSource>());
    g_guest_motionplus_pointer[0].store(true);
    g_guest_free_camera_ok.store(true);
    const float rad2deg = 180.0f / 3.14159265f;

    // ---------------------------------------------------------------------------------------------- U1
    std::printf("U1 face buttons by position (user decision 3)\n");
    check(1, face_button_for(1, 1) == SDL_CONTROLLER_BUTTON_B && face_button_for(0, 7) == SDL_CONTROLLER_BUTTON_A,
          "Xbox / PS5: east = SDL b (Circle / Xbox B), south = SDL a (Cross / Xbox A)");
    check(1, face_button_for(1, 5) == SDL_CONTROLLER_BUTTON_A && face_button_for(0, 5) == SDL_CONTROLLER_BUTTON_B &&
          face_button_for(3, 5) == SDL_CONTROLLER_BUTTON_X && face_button_for(2, 5) == SDL_CONTROLLER_BUTTON_Y,
          "Switch Pro (SDL label mode): east = A, south = B, north = X, west = Y");
    GameContext talk_ctx{};
    talk_ctx.prompt_a = 16; // A = talk (A "dash" is not passed on in switch-hd, U4)
    clean(talk_ctx);
    buttons[SDL_CONTROLLER_BUTTON_B] = 1; // east on an Xbox/PS pad
    step(3);
    check(1, (last.buttons & kWiiA) != 0, "switch-hd: the RIGHT face button (PS5 Circle / Xbox B) = Wii A");
    clean();
    buttons[SDL_CONTROLLER_BUTTON_A] = 1; // south
    step(3);
    check(1, (last.buttons & (kWiiA | kWiiB)) == 0, "switch-hd: the BOTTOM face button alone (standing) is neither Wii A nor B");
    set_presets("classic", "kbm");
    clean();
    buttons[SDL_CONTROLLER_BUTTON_A] = 1;
    step(3);
    check(1, (last.buttons & kWiiA) != 0, "classic preset unchanged: SDL a = Wii A");
    set_presets("switch-hd", "kbm");

    // ---------------------------------------------------------------------------------------------- U2
    std::printf("U2 every switch-hd intent -> its Wii signal (context: play)\n");
    struct Map { const char* name; int button; int axis; uint32_t wii, nun; };
    const Map maps[] = {
        {"north (X) = harp = D-up", face(3), -1, kWiiUp, 0}, {"ZR = item = Wii B", -1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, kWiiB, 0},
        {"R = pouch = Wii -", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -1, kWiiMinus, 0},
        {"- = map = Wii +", SDL_CONTROLLER_BUTTON_BACK, -1, kWiiPlus, 0},
        {"+ = gear = Wii 1", SDL_CONTROLLER_BUTTON_START, -1, kWiiOne, 0},
        {"D-up = first person = C", SDL_CONTROLLER_BUTTON_DPAD_UP, -1, 0, kNunC},
        {"D-down = Fi = D-down", SDL_CONTROLLER_BUTTON_DPAD_DOWN, -1, kWiiDown, 0},
        {"D-right = help = Wii 2", SDL_CONTROLLER_BUTTON_DPAD_RIGHT, -1, kWiiTwo, 0},
        {"D-left = D-left", SDL_CONTROLLER_BUTTON_DPAD_LEFT, -1, kWiiLeft, 0},
        {"Home = Wii HOME", SDL_CONTROLLER_BUTTON_GUIDE, -1, kWiiHome, 0},
        {"ZL = lock-on = Nunchuk Z", -1, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0, kNunZ},
    };
    for (const auto& m : maps) {
        clean();
        if (m.button >= 0) buttons[size_t(m.button)] = 1;
        if (m.axis >= 0) axes[size_t(m.axis)] = 30000;
        step(3);
        check(2, (last.buttons & m.wii) == m.wii && (last.nunchuk.buttons & m.nun) == m.nun &&
                 (m.wii || m.nun), std::string(m.name) + " (got buttons 0x" + std::to_string(last.buttons) + ")");
    }
    clean();
    buttons[SDL_CONTROLLER_BUTTON_LEFTSTICK] = 1;
    step(2);
    check(2, g_virtual_gesture[0].nunchuk_active() && !g_virtual_gesture[0].remote_active(), "L3 = shield (Nunchuk flick)");
    clean();
    buttons[SDL_CONTROLLER_BUTTON_RIGHTSTICK] = 1;
    step(2);
    check(2, g_virtual_gesture[0].remote_active(), "R3 = thrust (remote gesture)");
    clean();
    axes[SDL_CONTROLLER_AXIS_LEFTX] = 32767;
    step(2);
    check(2, last.nunchuk.stick_x > 0.99f, "left stick = Nunchuk stick");

    // ---------------------------------------------------------------------------------------------- U3
    std::printf("U3 HD B: dash while moving / cancel prompts on A / Wii B in events and on the Loftwing\n");
    {
        GameContext play{};
        clean(play);
        axes[SDL_CONTROLLER_AXIS_LEFTY] = -32767; // moving forward; A would dash (the game's default)
        buttons[size_t(face(0))] = 1;
        step(4);
        check(3, (last.buttons & kWiiA) && !(last.buttons & kWiiB), "B held while moving (A = dash) -> Wii A held");
        GameContext put_away{};
        put_away.link_request[0] = kPromptPutAway;
        clean(put_away);
        buttons[size_t(face(0))] = 1;
        step(3);
        check(3, (last.buttons & kWiiA) != 0, "B at a PUT AWAY prompt -> Wii A (sheathe)");
        GameContext ev{};
        ev.event_state = 3;
        clean(ev);
        buttons[size_t(face(0))] = 1;
        step(3);
        check(3, (last.buttons & kWiiB) && !(last.buttons & kWiiA), "B in an event / dialogue -> Wii B");
        GameContext bird{};
        bird.riding = 2;
        clean(bird);
        buttons[size_t(face(0))] = 1;
        step(3);
        check(3, (last.buttons & kWiiB) != 0, "B on the Loftwing -> Wii B (brake)");
        GameContext talk{};
        talk.prompt_a = 16; // talk
        clean(talk);
        buttons[size_t(face(0))] = 1;
        step(3);
        check(3, (last.buttons & (kWiiA | kWiiB)) == 0, "NEGATIVE: B standing at a TALK prompt does nothing (no B item, no talk)");
        GameContext beetle{};
        beetle.aim_flags = 0x10;
        beetle.cam_mode = 5;
        beetle.item_state = kItemBeetle;
        clean(beetle);
        buttons[size_t(face(0))] = 1;
        step(3);
        check(3, (last.buttons & kWiiB) && !(last.buttons & kWiiA), "B with the beetle aimed -> Wii B (put away)");
    }

    // ---------------------------------------------------------------------------------------------- U4
    std::printf("U4 A does not dash (HD)\n");
    {
        clean(GameContext{});
        axes[SDL_CONTROLLER_AXIS_LEFTY] = -32767;
        buttons[size_t(face(1))] = 1;
        step(3);
        check(4, (last.buttons & kWiiA) == 0, "A while moving with A = dash: no Wii A");
        GameContext talk{};
        talk.prompt_a = 16;
        clean(talk);
        buttons[size_t(face(1))] = 1;
        step(3);
        check(4, (last.buttons & kWiiA) != 0, "A at a TALK prompt: Wii A");
        cfg.a_does_not_dash = false;
        request_input_bindings_reload();
        clean(GameContext{});
        axes[SDL_CONTROLLER_AXIS_LEFTY] = -32767;
        buttons[size_t(face(1))] = 1;
        step(3);
        check(4, (last.buttons & kWiiA) != 0, "a_does_not_dash = false: A dashes (Wii A)");
        cfg.a_does_not_dash = true;
        request_input_bindings_reload();
    }

    // ---------------------------------------------------------------------------------------------- U5
    std::printf("U5 Y (gyro off) = camera behind Link: a short Nunchuk Z tap\n");
    {
        clean(GameContext{});
        buttons[size_t(face(2))] = 1;
        int z_frames = 0;
        for (int k = 0; k < 60; ++k) { step(); z_frames += (last.nunchuk.buttons & kNunZ) != 0; }
        check(5, z_frames >= 12 && z_frames <= 16, "Y held 0.5 s: Z for 0.12 s (" + std::to_string(z_frames) + " updates)");
    }

    // ---------------------------------------------------------------------------------------------- U6
    std::printf("U6 - = map; in a cutscene with the skip prompt = hold Wii 2 (skip)\n");
    {
        GameContext ev{};
        ev.event_state = 3;
        ev.skip_shown = true;
        clean(ev);
        buttons[SDL_CONTROLLER_BUTTON_BACK] = 1;
        step(6);
        check(6, (last.buttons & kWiiTwo) && !(last.buttons & kWiiPlus), "cutscene + skip prompt: - = Wii 2 held");
        ev.skip_shown = false;
        clean(ev);
        buttons[SDL_CONTROLLER_BUTTON_BACK] = 1;
        step(3);
        check(6, (last.buttons & (kWiiTwo | kWiiPlus)) == 0, "event without a skip prompt: nothing");
    }

    // ---------------------------------------------------------------------------------------------- U7
    std::printf("U7 slow right-stick pushes in play: the remote does not move (no drift); classic still aims\n");
    {
        clean(GameContext{});
        double worst = 0;
        for (int k = 0; k < 360; ++k) {
            const double t = k / 120.0;
            axes[SDL_CONTROLLER_AXIS_RIGHTX] = Sint16(std::lround(0.6 * std::sin(t * 2.0) * 32767));
            axes[SDL_CONTROLLER_AXIS_RIGHTY] = Sint16(std::lround(-0.5 * std::min(1.0, t) * 32767));
            step();
            worst = std::max(worst, rest_err());
        }
        std::printf("  switch-hd: max pointing error %.4f rad\n", worst);
        check(7, worst < 0.005 && !g_virtual_gesture[0].remote_active(), "switch-hd: the remote stays at REST");
        set_presets("classic", "kbm");
        clean(GameContext{});
        for (int k = 0; k < 360; ++k) {
            const double t = k / 120.0;
            axes[SDL_CONTROLLER_AXIS_RIGHTX] = Sint16(std::lround(0.6 * std::sin(t * 2.0) * 32767));
            axes[SDL_CONTROLLER_AXIS_RIGHTY] = Sint16(std::lround(-0.5 * std::min(1.0, t) * 32767));
            step();
        }
        std::printf("  classic: pointing error after the same input %.3f rad (the old drift)\n", rest_err());
        check(7, rest_err() > 0.3, "classic (control): the same input turns the remote");
        set_presets("switch-hd", "kbm");
    }

    // ---------------------------------------------------------------------------------------------- U8
    std::printf("U8 L held + right stick = free camera; no sword\n");
    {
        clean(GameContext{});
        buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 1;
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = 32767;
        step(4);
        const int cx = g_cam_stick_x_milli.load();
        check(8, cx > 900 && !g_virtual_gesture[0].remote_active(), "L + stick right: camera x = " + std::to_string(cx) + ", no swing");
        g_guest_free_camera_ok.store(false);
        step(2);
        check(8, g_cam_stick_x_milli.load() == 0, "camera gate closed (Z / aim / event): no orbit request");
        g_guest_free_camera_ok.store(true);
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = 0;
        buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 0;
        step(2);
        check(8, g_cam_stick_x_milli.load() == 0, "L released: the request goes back to 0");
    }

    // ---------------------------------------------------------------------------------------------- U9
    std::printf("U9 aiming the bow: the right stick aims, ZR = Wii A (draw/fire), back in play = REST\n");
    {
        GameContext bow{};
        bow.aim_flags = 0x2;
        bow.cam_mode = 3;
        bow.item_state = kItemBow;
        clean(bow);
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = 26000;
        axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 30000;
        seconds(0.5);
        const double aimed = rest_err();
        check(9, aimed > 0.2 && (last.buttons & kWiiA) && !(last.buttons & kWiiB),
              "bow: stick aims (" + std::to_string(aimed) + " rad), ZR = Wii A");
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = 0;
        axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 0;
        context(GameContext{});
        seconds(1.5);
        check(9, rest_err() < 0.02, "back in play: the remote returns to REST (" + std::to_string(rest_err()) + ")");
        clean(GameContext{});
        axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 30000;
        step(3);
        check(9, (last.buttons & kWiiB) && !(last.buttons & kWiiA), "no item out: ZR = Wii B");
    }

    // ---------------------------------------------------------------------------------------------- U10
    std::printf("U10 item wheel open: right stick left tilts the remote left (the game's wheel rule)\n");
    {
        GameContext wheel{};
        wheel.sword_states = kSwordBWheelOpen;
        clean(wheel);
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = -32767;
        seconds(0.4);
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = 0;
        seconds(0.3);
        double m[3][3];
        g_virtual_orientation[0].matrix(m);
        std::printf("  pointing (%.3f, %.3f, %.3f)\n", m[0][2], m[1][2], m[2][2]);
        check(10, m[0][2] > 0.3 && m[0][2] < 0.6, "left slot: pointing tilted left (and kept after the stick is released)");
    }

    // ---------------------------------------------------------------------------------------------- U11
    std::printf("U11 menus: the right stick moves the game's pointer (closed loop)\n");
    {
        clean(GameContext{});
        g_guest_player_present.store(false);
        GameContext title{};
        title.player = false;
        ctx_now = title;
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = 32767;
        seconds(0.4);
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = 0;
        seconds(0.2);
        float px = 0, py = 0;
        PointerAimLoop::pointer_of(g_virtual_orientation[0], px, py);
        check(11, px > 0.2f, "menu: stick right moves the pointer right (" + std::to_string(px) + ")");
        g_guest_player_present.store(true);
    }

    // ---------------------------------------------------------------------------------------------- U12
    std::printf("U12 gyro aiming (HD motion aiming): only in aim situations, never the accelerometer\n");
    {
        has_sensors = true;
        clean(GameContext{});
        // hold the gyro toggle (touchpad) 1 s
        buttons[SDL_CONTROLLER_BUTTON_TOUCHPAD] = 1;
        seconds(1.1);
        buttons[SDL_CONTROLLER_BUTTON_TOUCHPAD] = 0;
        seconds(0.6); // bias estimate settles while still
        check(12, g_gamepad_motion_mode.load() == 1, "touchpad held 1 s: gyro aiming on");
        GameContext fp{};
        fp.aim_flags = 0x40;
        fp.cam_mode = 2;
        clean(fp);
        gyro_yaw_rate = 1.0; // turn left
        seconds(0.4);
        gyro_yaw_rate = 0.0;
        double m[3][3];
        g_virtual_orientation[0].matrix(m);
        const double yaw_left = std::atan2(m[0][2], m[2][2]);
        std::printf("  first person: yaw %.3f rad after 0.4 s at 1 rad/s\n", yaw_left);
        check(12, yaw_left > 0.25 && yaw_left < 0.55, "first person: the gyro turns the remote");
        check(12, !g_gyro_drives_channel[0].load() && g_measured_motion[0].count == 0,
              "the measured sensors never replace the virtual remote (no accelerometer, no flags)");
        clean(GameContext{});
        gyro_yaw_rate = 1.0;
        seconds(0.4);
        gyro_yaw_rate = 0.0;
        check(12, rest_err() < 0.005, "play: the same gyro motion does not move the remote");
        buttons[SDL_CONTROLLER_BUTTON_TOUCHPAD] = 1;
        seconds(1.1);
        buttons[SDL_CONTROLLER_BUTTON_TOUCHPAD] = 0;
        step(2);
        check(12, g_gamepad_motion_mode.load() == 0, "touchpad held again: gyro aiming off");
        clean(fp);
        gyro_yaw_rate = 1.0;
        seconds(0.4);
        gyro_yaw_rate = 0.0;
        check(12, rest_err() < 0.005, "gyro aiming off: the gyro does not aim");
        has_sensors = false;
        clean(GameContext{});
    }

    // ---------------------------------------------------------------------------------------------- U13
    std::printf("U13 keyboard+mouse keys and the automatic mouse capture\n");
    {
        struct KeyMap { const char* name; int key; uint32_t wii, nun; };
        const KeyMap km[] = {
            {"E = A", SDL_SCANCODE_E, kWiiA, 0}, {"R = item = B", SDL_SCANCODE_R, kWiiB, 0},
            {"Tab = pouch = -", SDL_SCANCODE_TAB, kWiiMinus, 0}, {"M = map = +", SDL_SCANCODE_M, kWiiPlus, 0},
            {"I = gear = 1", SDL_SCANCODE_I, kWiiOne, 0}, {"Esc = gear = 1", SDL_SCANCODE_ESCAPE, kWiiOne, 0},
            {"C = first person = C", SDL_SCANCODE_C, 0, kNunC}, {"T = Fi = D-down", SDL_SCANCODE_T, kWiiDown, 0},
            {"H = help = 2", SDL_SCANCODE_H, kWiiTwo, 0}, {"V = harp = D-up", SDL_SCANCODE_V, kWiiUp, 0},
            {"Q = lock-on = Z", SDL_SCANCODE_Q, 0, kNunZ}, {"Left = D-left", SDL_SCANCODE_LEFT, kWiiLeft, 0},
            {"Home = HOME", SDL_SCANCODE_HOME, kWiiHome, 0},
        };
        GameContext talk{};
        talk.prompt_a = 16;
        for (const auto& k : km) {
            clean(talk);
            keys[size_t(k.key)] = 1;
            step(3);
            check(13, (last.buttons & k.wii) == k.wii && (last.nunchuk.buttons & k.nun) == k.nun,
                  std::string(k.name) + " (got 0x" + std::to_string(last.buttons) + ")");
        }
        clean(GameContext{});
        keys[SDL_SCANCODE_W] = keys[SDL_SCANCODE_D] = 1;
        step(2);
        check(13, last.nunchuk.stick_y > 0.6f && last.nunchuk.stick_x > 0.6f, "W + D = stick up-right");
        keys[SDL_SCANCODE_LALT] = 1;
        step(2);
        check(13, last.nunchuk.stick_y < 0.4f && last.nunchuk.stick_y > 0.3f, "Alt = walk slowly (half)");
        clean(GameContext{});
        keys[SDL_SCANCODE_W] = keys[SDL_SCANCODE_LSHIFT] = 1;
        step(3);
        check(13, (last.buttons & kWiiA) != 0, "Shift while moving (A = dash) = Wii A (dash)");
        clean(GameContext{});
        step(3);
        check(13, relative_mode == 1, "play: the mouse is captured (relative mode)");
        g_guest_player_present.store(false);
        GameContext menu{};
        menu.player = false;
        ctx_now = menu;
        step(3);
        check(13, relative_mode == 0, "menu / title: the mouse is released");
        g_guest_player_present.store(true);
        context(GameContext{});
        step(3);
        g_port_menu_open.store(true);
        step(3);
        check(13, relative_mode == 0, "port menu (F1) open: released at once");
        g_port_menu_open.store(false);
        step(3);
        focused = false;
        step(3);
        check(13, relative_mode == 0, "no keyboard focus: released");
        focused = true;
        step(3);
        check(13, relative_mode == 1, "back in play: captured again");
    }

    // ---------------------------------------------------------------------------------------------- U14
    std::printf("U14 keyboard+mouse: mouse = camera, left click = slash, drag = directed slash, right = shield\n");
    {
        clean(GameContext{});
        step(2);
        const int yaw0 = g_cam_delta_yaw_mdeg.load();
        rel_x = 40;
        step();
        check(14, g_cam_delta_yaw_mdeg.load() - yaw0 == 6000, "mouse 40 px right = camera 6 deg (CTRL18-CAM 0.15 deg/px)");
        clean(GameContext{});
        mouse_buttons = SDL_BUTTON(SDL_BUTTON_LEFT);
        step(3);
        mouse_buttons = 0;
        step(2);
        check(14, g_virtual_gesture[0].remote_active(), "a left click = a slash");
        clean(GameContext{});
        seconds(1.0);
        mouse_buttons = SDL_BUTTON(SDL_BUTTON_LEFT);
        step();
        GestureStepOutput probe{};
        float first_yaw = 0.0f;
        for (int k = 0; k < 6; ++k) { rel_x = 12; step(); }
        // The swing turned the remote right: yaw < 0.
        double m[3][3];
        seconds(0.05);
        g_virtual_orientation[0].matrix(m);
        first_yaw = float(std::atan2(m[0][2], m[2][2]));
        (void)probe;
        std::printf("  drag right: remote yaw %.2f deg (negative = right)\n", first_yaw * rad2deg);
        check(14, first_yaw < -0.2f, "hold + drag right = a slash to the right");
        mouse_buttons = 0;
        seconds(1.5);
        check(14, rest_err() < 0.02, "after the slash: REST");
        clean(GameContext{});
        mouse_buttons = SDL_BUTTON(SDL_BUTTON_RIGHT);
        step(2);
        check(14, g_virtual_gesture[0].nunchuk_active() && !g_virtual_gesture[0].remote_active(), "right button = shield (Nunchuk flick)");
        clean(GameContext{});
        mouse_buttons = SDL_BUTTON(SDL_BUTTON_MIDDLE);
        step(2);
        check(14, g_virtual_gesture[0].remote_active(), "middle button = thrust");
        GameContext sling{};
        sling.aim_flags = 0x80;
        sling.cam_mode = 8;
        sling.item_state = kItemSlingshot;
        clean(sling);
        for (int k = 0; k < 20; ++k) { rel_x = 5; step(); }
        mouse_buttons = SDL_BUTTON(SDL_BUTTON_LEFT);
        step(2);
        check(14, rest_err() > 0.3 && (last.buttons & kWiiA), "slingshot: the mouse aims, left button = Wii A");
        mouse_buttons = 0;
        context(GameContext{});
        seconds(1.5);
        check(14, rest_err() < 0.02, "back in play: REST");
    }

    // ---------------------------------------------------------------------------------------------- U15
    std::printf("U15 quick items: key 3 = the third slot clockwise from the top\n");
    {
        clean(GameContext{});
        keys[SDL_SCANCODE_3] = 1;
        step(3);
        keys[SDL_SCANCODE_3] = 0;
        check(15, (last.buttons & kWiiB) != 0, "key 3: Wii B held (the game opens the item wheel)");
        GameContext open{};
        open.sword_states = kSwordBWheelOpen;
        context(open);
        seconds(0.2);
        double m[3][3];
        g_virtual_orientation[0].matrix(m);
        check(15, (last.buttons & kWiiB) && m[0][2] < -0.2, "wheel open: the remote tilts right (slot 3 = right)");
        open.wheel_index = 6; // the game highlights wheel index 6 (third clockwise from the top)
        context(open);
        seconds(0.3);
        check(15, (last.buttons & kWiiB) == 0, "highlighted: B released (the game equips it)");
    }

    // ---------------------------------------------------------------------------------------------- U16
    std::printf("U16 bindings: [input.gamepad_hd] rebinds, preset swap while running\n");
    {
        cfg.gamepad_hd_bindings = {{"action", {"south"}}, {"dash", {"east"}}, {"nonsense", {"a"}}};
        request_input_bindings_reload();
        GameContext talk{};
        talk.prompt_a = 16;
        clean(talk);
        buttons[SDL_CONTROLLER_BUTTON_A] = 1; // south
        step(3);
        check(16, (last.buttons & kWiiA) != 0, "action rebound to the bottom button");
        bool msg = false;
        for (const auto& s : input_bindings().messages) msg = msg || s.find("unknown action \"nonsense\"") != std::string::npos;
        check(16, msg, "an unknown action name is reported");
        cfg.gamepad_hd_bindings.clear();
        request_input_bindings_reload();
        step(2);
        check(16, input_bindings().pad_preset == PadPreset::SwitchHd, "default preset = switch-hd");
        set_presets("wii-motion", "classic");
        check(16, input_bindings().pad_preset == PadPreset::WiiMotion && input_bindings().key_preset == KeyPreset::Classic,
              "preset swap: wii-motion + classic keyboard");
        set_presets("switch-hd", "kbm");
    }

    // ---------------------------------------------------------------------------------------------- U17
    std::printf("U17 config keys\n");
    {
        const std::string path = "ctrl18-test-settings.toml";
        {
            std::ofstream f(path);
            f << "[input]\npreset_gamepad = \"classic\"\npreset_keyboard = \"classic\"\ngyro_aim = true\n"
                 "a_does_not_dash = false\nfire_on_release = true\nquick_items = false\n"
                 "[input.gamepad_hd]\naction = \"south\"\n[input.kbm]\nslash = \"Mouse Right\"\n"
                 "[mouse]\nsensitivity_camera = 2.0\nsensitivity_aim = 0.5\ninvert_y = true\n"
                 "[camera]\nflight_invert_y = true\n";
        }
        cfg.load(path);
        check(17, cfg.preset_gamepad == "classic" && cfg.preset_keyboard == "classic" && cfg.gyro_aim &&
                  !cfg.a_does_not_dash && cfg.fire_on_release && !cfg.quick_items, "[input] preset keys");
        check(17, cfg.gamepad_hd_bindings.size() == 1 && cfg.kbm_bindings.size() == 1, "[input.gamepad_hd] / [input.kbm] tables");
        check(17, cfg.mouse_camera_sensitivity == 2.0f && cfg.mouse_aim_sensitivity == 0.5f && cfg.mouse_invert_y &&
                  cfg.flight_invert_y, "[mouse] + [camera] flight_invert_y");
        std::remove(path.c_str());
        cfg.preset_gamepad = "switch-hd"; cfg.preset_keyboard = "kbm"; cfg.gyro_aim = false; cfg.a_does_not_dash = true;
        cfg.fire_on_release = false; cfg.quick_items = true; cfg.gamepad_hd_bindings.clear(); cfg.kbm_bindings.clear();
        cfg.mouse_invert_y = false; cfg.flight_invert_y = false; cfg.mouse_camera_sensitivity = 1; cfg.mouse_aim_sensitivity = 1;
        request_input_bindings_reload();
        step(2);
    }

    // ---------------------------------------------------------------------------------------------- U18
    std::printf("U18 live status + device tracking\n");
    {
        clean(GameContext{});
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = 32767;
        step(3);
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = 0;
        step(2);
        const Ctrl18Status st = ctrl18_live_status();
        std::printf("  status: preset '%s', situation '%s', last move '%s'\n", st.preset, st.situation, st.last_move);
        check(18, std::string(st.preset) == "switch-hd" && std::string(st.situation) == "Lopen" &&
                  std::string(st.last_move) == "Slag rechts", "status: switch-hd / Lopen / Slag rechts");
        check(18, g_active_device[0].load() == int(ActiveDevice::Gamepad), "the pad drove Wii Remote 1");
        clean(GameContext{});
        keys[SDL_SCANCODE_E] = 1;
        step(2);
        check(18, g_active_device[0].load() == int(ActiveDevice::KeyboardMouse), "the keyboard drove Wii Remote 1");
        set_presets("wii-motion", "kbm");
        release_all();
        buttons[SDL_CONTROLLER_BUTTON_A] = 1;
        step(3);
        check(18, !g_free_camera.scheme_allows.load(), "wii-motion pad driving: no free camera");
        set_presets("switch-hd", "kbm");
        step(2);
        check(18, g_free_camera.scheme_allows.load(), "switch-hd: free camera allowed");
    }

    for (int id = 1; id < 20; ++id)
        if (ufail[id]) std::printf("FAIL: U%d (%u check(s))\n", id, ufail[id]);

    std::printf("U19 mixed mouse/controller menu ownership through production sources\n");
    set_presets("switch-hd", "kbm");
    clean();
    g_guest_pointer_cursor.store(true);
    g_pointer_aim[0] = PointerAimLoop{};
    g_virtual_orientation[0].reset();
    const int width=Config::get().window_width, height=Config::get().window_height;
    mouse_x=int(width*.70);mouse_y=height/2;
    g_aim_owner.store(0);
    seconds(.6);
    float px=0,py=0;
    PointerAimLoop::pointer_of(g_virtual_orientation[0],px,py);
    const float mouse_target=2.0f*mouse_x/width-1.0f;
    check(19,std::abs(px-mouse_target)<.02f,"resting pad leaves mouse menu target intact");
    axes[SDL_CONTROLLER_AXIS_RIGHTX]=-32767;
    seconds(.25);
    PointerAimLoop::pointer_of(g_virtual_orientation[0],px,py);
    check(19,g_aim_owner.load()==1 && px<mouse_target-.15f,"stick takes aim back and moves left");
    axes.fill(0);
    const float hold_x=px;
    seconds(.7);
    PointerAimLoop::pointer_of(g_virtual_orientation[0],px,py);
    check(19,std::abs(px-hold_x)<.02f,"released controller cursor stays in place");
    mouse_x=int(width*.30);
    seconds(.6);
    PointerAimLoop::pointer_of(g_virtual_orientation[0],px,py);
    check(19,g_aim_owner.load()==0 && std::abs(px-(2.0f*mouse_x/width-1.0f))<.02f,
          "mouse takes aim back without a conflicting pad correction");
    g_guest_pointer_cursor.store(false);

    std::printf("U19 gyro timing across host/sensor rates and deliberate slow aim\n");
    has_sensors=true;g_gamepad_motion_mode.store(1);
    GameContext fp{};fp.aim_flags=0x40;fp.cam_mode=2;
    for (int host : {30,60,120,240,500,1000}) for (int sensor : {250,1000}) {
        sensor_rate=sensor;sensor_budget=0;
        clean(fp);g_aim_owner.store(1);gyro_yaw_rate=.3;
        for(int k=0;k<host;k++) {
            now_ns+=uint64_t(std::llround(1e9/host));push_sensors(1.0/host);
            ctx_now.frame++;g_guest_context.publish(ctx_now);
            InputManager::get().update_at(now_ns);
        }
        double m[3][3];g_virtual_orientation[0].matrix(m);
        const double yaw=std::atan2(m[0][2],m[2][2]);
        std::printf("  host=%d sensor=%d: yaw %.6f expected .300000\n",host,sensor,yaw);
        check(19,std::fabs(yaw-.3)<.005,"gyro angular integral independent of host/sensor rate");
    }
    sensor_rate=1000;clean(fp);g_aim_owner.store(1);gyro_yaw_rate=.03;
    seconds(2.5);double slow[3][3];g_virtual_orientation[0].matrix(slow);
    const double slow_yaw=std::atan2(slow[0][2],slow[2][2]);
    std::printf("  deliberate slow aiming: %.6f expected .075000\n",slow_yaw);
    check(19,std::fabs(slow_yaw-.075)<.005,"fine aiming is not learned as sensor drift");
    gyro_yaw_rate=.002;clean(fp);g_aim_owner.store(0);seconds(.5);
    check(19,g_aim_owner.load()==0,"idle sensor noise does not steal mouse ownership");
    gyro_yaw_rate=.3;seconds(.1);
    check(19,g_aim_owner.load()==1,"deliberate gyro motion takes aim back from the mouse");
    // A host polling faster than the sensor must still recognise continuous gyro motion.
    sensor_rate=250;clean(fp);g_aim_owner.store(0);gyro_yaw_rate=.3;
    for(int k=0;k<80;k++) {
        now_ns+=1000000;push_sensors(.001);ctx_now.frame++;g_guest_context.publish(ctx_now);
        InputManager::get().update_at(now_ns);
    }
    check(19,g_aim_owner.load()==1,"1000 Hz host / 250 Hz gyro: empty polls do not cancel takeover");
    set_presets("wii-motion","kbm");clean(fp);g_aim_owner.store(0);gyro_yaw_rate=.3;
    seconds(.1);
    check(19,g_aim_owner.load()==1,"Wii motion mode: moderate motion can reclaim the pointer");
    sensor_rate=250;clean(fp);g_aim_owner.store(1);gyro_yaw_rate=.03;
    seconds(2.5);g_virtual_orientation[0].matrix(slow);
    check(19,std::fabs(std::atan2(slow[0][2],slow[2][2])-.075)<.005,
          "Wii mode preserves fine aiming instead of learning it as bias");
    gyro_yaw_rate=.3;sensor_budget=0;
    bool saw_empty=false,held=true;
    for(int k=0;k<40;k++) {
        const auto previous=InputManager::get().get_wiimote_state(0).motion;
        now_ns+=1000000;push_sensors(.001);ctx_now.frame++;g_guest_context.publish(ctx_now);
        InputManager::get().update_at(now_ns);
        if(g_measured_motion[0].count==0) {
            saw_empty=true;
            held=held && InputManager::get().get_wiimote_state(0).motion.sequence==previous.sequence;
        }
    }
    check(19,saw_empty && held,"Wii mode empty hardware polls hold motion sequence/history");
    set_presets("switch-hd","kbm");
    release_all();has_sensors=false;g_gamepad_motion_mode.store(0);


    std::printf("U19 Beetle slow steering must not redefine sensor zero\n");
    has_sensors=true;g_gamepad_motion_mode.store(1);
    for (const char* preset : {"classic", "wii-motion", "switch-hd"}) {
        for (int hz : {250,1000}) for (bool cursor : {false,true}) {
            cfg.gyro_aim=true;set_presets(preset,"kbm");g_gamepad_motion_mode.store(1);
            sensor_rate=hz;sensor_budget=0;
            g_guest_pointer_cursor.store(false);
            clean(fp);gyro_yaw_rate=0;g_aim_owner.store(1);
            calibration_begin(0);seconds(2.1);
            check(19,calibration_apply(0),"guided still calibration succeeds before Beetle launch");
            step(2);
            GameContext beetle{};beetle.item_state=kItemBeetle;beetle.cam_handler=6;
            clean(beetle);g_aim_owner.store(1);g_guest_pointer_cursor.store(cursor);
            gyro_yaw_rate=.03;seconds(4);
            double m[3][3];g_virtual_orientation[0].matrix(m);
            const double turn=std::atan2(m[0][2],m[2][2]);
            gyro_yaw_rate=0;seconds(2);
            g_virtual_orientation[0].matrix(m);
            const double held=std::atan2(m[0][2],m[2][2]);
            gyro_yaw_rate=-.03;seconds(4);
            g_virtual_orientation[0].matrix(m);
            const double returned=std::atan2(m[0][2],m[2][2]);
            std::printf("  Beetle %s sensor=%d cursor=%d turn=%.6f hold=%.6f return=%.6f\n",
                        preset,hz,int(cursor),turn,held,returned);
            if(std::string(preset)!="switch-hd") {
                check(19,std::fabs(turn-.12)<.002,"four-second slow Beetle turn remains fully responsive");
                check(19,std::fabs(held-turn)<.002,"stopping Beetle steering does not create reverse drift");
                check(19,std::fabs(returned)<.002,"equal opposite steering returns to the launch orientation");
            }
            // The HD preset uses sticks in flight, but sampling the gyro must not
            // poison the calibration used when it returns to item aiming.
            g_guest_pointer_cursor.store(false);context(fp);gyro_yaw_rate=0;
            g_virtual_orientation[0].reset();seconds(2);
            g_virtual_orientation[0].matrix(m);
            check(19,std::fabs(std::atan2(m[0][2],m[2][2]))<.002,
                  "returning from Beetle to aiming does not carry a learned steering bias");
        }
    }
    g_guest_pointer_cursor.store(false);has_sensors=false;g_gamepad_motion_mode.store(0);


    std::printf("U19 remapping with an explicit HD layout and real gyro enabled\n");
    cfg.gamepad_motion="gyro";cfg.gyro_aim=true;
    cfg.gamepad_hd_bindings={{"action",{"paddle1"}}};
    set_presets("switch-hd","kbm");
    check(19,input_bindings().pad_preset==PadPreset::SwitchHd,
          "enabling gyro must not silently change the selected layout or binding table");
    GameContext talk{};talk.prompt_a=16;clean(talk);
    buttons[SDL_CONTROLLER_BUTTON_PADDLE1]=1;step(3);
    check(19,(last.buttons&kWiiA)!=0,"the custom HD action button reaches Wii A with gyro enabled");
    release_all();buttons[SDL_CONTROLLER_BUTTON_B]=1;step(3);
    check(19,(last.buttons&kWiiA)==0,"the original HD action button no longer triggers a remapped action");
    release_all();cfg.gamepad_hd_bindings.clear();cfg.gamepad_motion="off";cfg.gyro_aim=false;
    for(const char* contents:{"[input]\ngamepad_motion=\"gyro\"\n",
                             "[input]\ngamepad_motion=\"gyro\"\npreset_gamepad=\"switch-hd\"\n",
                             "[input]\ngamepad_motion=\"gyro\"\npreset_gamepad=\"classic\"\n"}) {
        const char* path="bindings36-migration.toml";
        {std::ofstream f(path);f<<contents;}
        cfg.preset_gamepad="switch-hd";cfg.load(path);std::remove(path);
        const char* expected=std::string(contents).find("preset_gamepad")==std::string::npos?"wii-motion":
                             std::string(contents).find("classic")!=std::string::npos?"classic":"switch-hd";
        check(19,cfg.preset_gamepad==expected,"legacy gyro-only settings migrate without overriding explicit layouts");
    }

    std::printf("%s %u/%u checks\n", failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
