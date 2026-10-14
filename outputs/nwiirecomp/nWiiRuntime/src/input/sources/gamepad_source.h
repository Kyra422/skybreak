#pragma once
#include "input/input_source.h"
#include "../motion_gesture.h"
#include "../gamepad_motion.h"
#include "../gyro_calibration.h"
#include "../pad_hub.h"
#include "../hd_translator.h" // CTRL18-INPUT
#include <SDL.h>
#include <cstdint>
#include <vector>

namespace nwii::runtime::input {

class GamepadSource : public IInputSource {
public:
    GamepadSource() = default;
    ~GamepadSource() override = default;

    void update(GameCubePadState pads[4], WiimoteState motes[4]) override;

private:
    void update_wii_remote(int i, const PadSnapshot& snap, WiimoteState& mote, bool& any_gyro);
    float wii_ir_reference[4][2]{};
    bool wii_ir_reference_set[4]{};
    float wii_nunchuk_previous[4][3]{};
    bool wii_nunchuk_seen[4]{};
    void check_controller_thread(); // INPUT13 watchdog: "[Input] controller detection is blocked ..."

    // Sword gesture state per controller (motion_gesture.h StickFlick, thrust / Nunchuk flick edges).
    StickFlick flick[4];
    bool previous_rb[4] = {}, previous_rt[4] = {};
    // Menu cursor (MotionPlus pointer, no player actor): the right stick moves a screen
    // target (-1..1) and the virtual remote is steered onto it, so the pointer stays put
    // when the stick is released. Touchpad (PS4/PS5) or R3+L3 recentres it.
    float cursor_x[4] = {}, cursor_y[4] = {};
    // INPUT10 gyro mode (gamepad_motion.h): sensor stream per controller, toggle hold time.
    GyroMotionTracker gyro[4];
    bool sensors_on[4] = {};
    int32_t sensor_id[4] = {-1, -1, -1, -1};
    double toggle_held_s[4] = {};
    bool toggle_fired[4] = {};
    bool no_sensor_logged[4] = {};
    double gyro_active_s[4] = {};
    double gyro_claim_s[4] = {}; // deliberate hybrid gyro motion, suppress idle noise after mouse use
    bool stats_logged[4] = {};
    float recent_pitch[4] = {}, recent_yaw[4] = {}; // |rate| averages: Nunchuk shake axis
    std::vector<SensorEvent> scratch;
    // INPUT13: controller snapshot hand-over (pad_hub.h) and pointer recovery.
    uint32_t generation[4] = {};
    bool recenter_prev[4] = {};       // gyro mode: recentre fires on the press edge
    double recenter_left_s[4] = {};   // gyro mode: steering the remote back to the centre
    bool point_center_prev[4] = {};   // gyro mode: the point-at-screen step captures the grip when it ends
    bool menu_prev[4] = {};           // stick mode: entering the menu cursor syncs it to the pointer
    int owner_prev = -1;
    bool mouse_note_logged = false;
    bool blocked_reported = false;
    uint64_t blocked_since_ms = 0;
    // CTRL18-INPUT: the "switch-hd" preset (hd_translator.h): one translator per controller; preset changes at runtime.
    void update_switch_hd(int i, const PadSnapshot& snap, GameCubePadState& gc, WiimoteState& mote, bool& any_gyro);
    HdPadTranslator hd[4];
    GravityAim gravity_aim[4];
    uint64_t calibration_serial[4]{};
    bool hd_claimed[4] = {}, hd_recentre_prev[4] = {}, gyro_hint_logged = false;
    int last_preset = -1;
};

// INPUT13: the SDL functions the controller thread needs besides the ones the gamepad source calls itself.
// Injected by main.cpp (sdl_controller_api below) so the unit tests, which mock only the per-controller SDL
// calls, link without them. A null api = inline capture (tests, headless): no SDL_JoystickUpdate, no close.
struct ControllerApi {
    int (*init)() = nullptr;                 // SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER); < 0 = failure
    void (*quit)() = nullptr;                // SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER)
    void (*update)() = nullptr;              // SDL_JoystickUpdate (with SDL_HINT_AUTO_UPDATE_JOYSTICKS=0)
    int (*num_joysticks)() = nullptr;        // SDL_NumJoysticks
    void (*close)(SDL_GameController*) = nullptr;
    int (*type)(SDL_GameController*) = nullptr;           // SDL_GameControllerGetType
    float (*gyro_rate)(SDL_GameController*) = nullptr;    // SDL_GameControllerGetSensorDataRate(GYRO)
    const char* (*error)() = nullptr;        // SDL_GetError
    bool (*wii_ir)(SDL_GameController*, float out[4], bool& valid, uint16_t& serial) = nullptr;
    void (*pump_messages)() = nullptr;       // this thread's window messages (tests: optional)
};

// Reads every controller once (enumerate, open new devices once, close removed ones, apply sensor/rumble
// requests, publish a snapshot). Called by the controller thread, or inline by the gamepad source when no
// thread was started.
void capture_controllers(const ControllerApi* api);

// Starts the controller thread (SDL joystick init + SDL_JoystickUpdate + capture every ~2 ms). Waits up to
// `wait_init_s` for the initial enumeration; returns false if it did not finish in time (it keeps running,
// the input watchdog reports a blocked driver). Call after SDL_Init (without SDL_INIT_GAMECONTROLLER).
bool start_controller_thread(const ControllerApi& api, double wait_init_s = 1.5);
// Stops it. Returns false when the thread is stuck inside a driver call: then the caller must NOT call
// SDL_Quit (it would wait for the same joystick lock forever).
bool stop_controller_thread(double wait_s = 1.0);
bool controller_thread_running();

} // namespace nwii::runtime::input

// Only main.cpp calls this (it is the one translation unit that links SDL's joystick subsystem).
namespace nwii::runtime::input {
inline ControllerApi sdl_controller_api() {
    ControllerApi a;
    a.init = [] { return SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER); };
    a.quit = [] { SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER); };
    a.update = [] { SDL_JoystickUpdate(); };
    a.num_joysticks = [] { return SDL_NumJoysticks(); };
    a.close = [](SDL_GameController* c) { SDL_GameControllerClose(c); };
    a.type = [](SDL_GameController* c) { return int(SDL_GameControllerGetType(c)); };
    a.gyro_rate = [](SDL_GameController* c) { return SDL_GameControllerGetSensorDataRate(c, SDL_SENSOR_GYRO); };
    a.error = [] { return SDL_GetError(); };
    a.wii_ir=[](SDL_GameController* c,float out[4],bool& valid,uint16_t& serial) {
        auto* j=SDL_GameControllerGetJoystick(c);
        if(!j || SDL_JoystickNumAxes(j)<12)return false;
        out[0]=(SDL_JoystickGetAxis(j,6)/32767.0f+1.0f)*0.5f;
        out[1]=(SDL_JoystickGetAxis(j,7)/32767.0f+1.0f)*0.5f;
        out[2]=SDL_JoystickGetAxis(j,8)/32767.0f;
        out[3]=SDL_JoystickGetAxis(j,9)/32767.0f;
        valid=SDL_JoystickGetAxis(j,10)>0;
        serial=uint16_t(SDL_JoystickGetAxis(j,11));return true;
    };
    return a;
}
} // namespace nwii::runtime::input
