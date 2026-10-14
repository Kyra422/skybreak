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
static bool connected = true, focused = true, has_sensors = true, nun = true;
static const char* controller_name="Nintendo Wii Remote with Nunchuk";
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
SDL_bool SDLCALL SDL_GameControllerHasSensor(SDL_GameController*, SDL_SensorType sensor) { return (sensor==SDL_SENSOR_ACCEL_L ? nun : has_sensors) ? SDL_TRUE : SDL_FALSE; }
int SDLCALL SDL_GameControllerSetSensorEnabled(SDL_GameController*, SDL_SensorType, SDL_bool) { return 0; }
int SDLCALL SDL_GameControllerGetSensorData(SDL_GameController*, SDL_SensorType, float*, int) { return -1; }
void SDLCALL SDL_AddEventWatch(SDL_EventFilter f, void*) { watch_fn = f; }
SDL_JoystickID SDLCALL SDL_JoystickGetDeviceInstanceID(int index) { return index; }
int SDLCALL SDL_GameControllerRumble(SDL_GameController*, Uint16, Uint16, Uint32) { return 0; }
const char* SDLCALL SDL_GameControllerName(SDL_GameController*) { return controller_name; }
Uint32 SDLCALL SDL_GetMouseState(int* x, int* y) { *x = mouse_x; *y = mouse_y; return mouse_buttons; }
const Uint8* SDLCALL SDL_GetKeyboardState(int* size) { if (size) *size = int(keys.size()); return keys.data(); }
}

static unsigned checks = 0, failures = 0, ufail[20] = {};
static void check(int id, bool v, const std::string& what) {
    ++checks;
    if (!v) { ++failures; ++ufail[id]; std::printf("  CHECK FAILED: U%d %s\n", id, what.c_str()); }
}


#include "input/wii_remote_controls.h"
#include <thread>
#include <chrono>
static ControllerApi ir_api;
static bool bar=false;
static uint16_t ir_serial=0;
static float ir_data[4]={0.5f,0.5f,1.f,0.f};
static uint64_t now=1'000'000'000, sensor_time=1000;
static WiimoteState last;
static void event(SDL_SensorType sensor,float x,float y,float z,bool untimed=false) {
 SDL_Event e{};e.type=SDL_CONTROLLERSENSORUPDATE;e.csensor.which=0;
 e.csensor.sensor=sensor;e.csensor.timestamp_us=untimed?0:sensor_time;
 e.csensor.data[0]=x;e.csensor.data[1]=y;e.csensor.data[2]=z;
 if(watch_fn)watch_fn(nullptr,&e);
}
static void poll(unsigned elapsed=1000) {
 now+=uint64_t(elapsed)*1000;capture_controllers(&ir_api);InputManager::get().update_at(now);
 last=InputManager::get().get_wiimote_state(0);
}
static void packet(float pitch=0,float yaw=0,float roll=0) {
 sensor_time+=10000;event(SDL_SENSOR_ACCEL,0,9.80665f,0);
 event(SDL_SENSOR_GYRO,-pitch,yaw,-roll);poll();
}
static void clear() {buttons.fill(0);axes.fill(0);keys.fill(0);rel_x=rel_y=0;}
int main() {
 ir_api.num_joysticks=[](){return connected?1:0;};
 ir_api.wii_ir=[](SDL_GameController*,float out[4],bool& valid,uint16_t& serial){
  std::copy(ir_data,ir_data+4,out);valid=bar;serial=ir_serial;return true;};
 pad_hub().threaded.store(true);
 InputManager::get().add_source(std::make_unique<GamepadSource>());
 InputManager::get().add_source(std::make_unique<MouseKeyboardSource>());
 poll();packet();
 check(1,g_wii_remote_active[0] && g_gyro_drives_channel[0],"remote automatically uses physical gyro even when gamepad gyro is off");
 check(1,is_wii_remote_name(controller_name) && !is_wii_remote_name("Nintendo Wii U Pro Controller") &&
           !is_wii_remote_name("Nintendo Wii Remote with Classic Controller"),"remote identification excludes other Nintendo devices");
 static constexpr struct {int button;unsigned expected;} map[]={
 {SDL_CONTROLLER_BUTTON_B,0x800},{SDL_CONTROLLER_BUTTON_A,0x400},{SDL_CONTROLLER_BUTTON_X,0x200},
 {SDL_CONTROLLER_BUTTON_Y,0x100},{SDL_CONTROLLER_BUTTON_START,0x10},{SDL_CONTROLLER_BUTTON_BACK,0x1000},
 {SDL_CONTROLLER_BUTTON_GUIDE,0x8000},{SDL_CONTROLLER_BUTTON_DPAD_UP,8},
 {SDL_CONTROLLER_BUTTON_DPAD_DOWN,4},{SDL_CONTROLLER_BUTTON_DPAD_LEFT,1},{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,2}};
 for(const char* preset:{"classic","switch-hd","wii-motion"}) {
  Config::get().preset_gamepad=preset;reload_input_bindings();
  for(const auto& m:map) {clear();buttons[m.button]=1;packet();
   check(1,last.buttons==m.expected,std::string(preset)+" preserves exact physical button "+std::to_string(m.button));}
 }
 clear();for(int k=0;k<70;++k){packet();poll(9000);} // Let Down's centring interval finish.
 buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER]=1;axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT]=32767;
 axes[SDL_CONTROLLER_AXIS_LEFTX]=32767;axes[SDL_CONTROLLER_AXIS_LEFTY]=-32768;packet();
 check(2,last.nunchuk.available && last.nunchuk.buttons==0x6000 && last.buttons==0,"physical Nunchuk C/Z do not trigger remote buttons");
 check(2,last.nunchuk.stick_x==1 && last.nunchuk.stick_y==1,"physical Nunchuk stick retains full calibrated range and up");
 clear();event(SDL_SENSOR_ACCEL_L,19.6133f,9.80665f,-9.80665f);packet();
 check(2,std::abs(last.nunchuk.accel_x-2)<.001 && std::abs(last.nunchuk.accel_y+1)<.001 &&
          std::abs(last.nunchuk.accel_z+1)<.001,"Nunchuk acceleration retains all physical axes in guest g units");
 keys[SDL_SCANCODE_LSHIFT]=1;rel_x=100;rel_y=100;mouse_buttons=SDL_BUTTON_LMASK;
 packet(12,5,-3);
 check(3,!g_virtual_gesture[0].remote_active() && last.buttons==0,"keyboard/mouse cannot add a synthetic sword or duplicate button");
 check(3,std::abs(last.motion.gyro_x_rad_s-12)<.001 && std::abs(last.motion.gyro_y_rad_s-5)<.001 &&
          std::abs(last.motion.gyro_z_rad_s+3)<.001,"full physical swing survives without HD aim projection or smoothing");
 mouse_buttons=0;clear();packet();
 const auto sequence=last.motion.sequence;poll();poll();
 check(3,last.motion.sequence==sequence,"host polls without sensor reports preserve motion identity");
 poll(200000);
 check(3,!motion_sample_fresh(last.motion,now),"stalled physical sensor stream expires instead of drifting forever");
 focused=false;buttons[SDL_CONTROLLER_BUTTON_B]=1;poll();
 check(4,last.buttons==0 && !g_gyro_drives_channel[0],"focus loss releases physical buttons and motion");
 focused=true;clear();packet();
 connected=false;poll();check(4,!g_wii_remote_active[0] && !g_gyro_drives_channel[0],"disconnect removes native ownership");
 connected=true;packet();packet();check(4,g_wii_remote_active[0] && last.buttons==0 &&
   last.nunchuk.stick_x==0 && last.nunchuk.stick_y==0,"reconnect clears held inputs and uses a fresh sensor stream");
 // Verify zero hardware timestamps use packet arrival time, not 1 ms host polls.
 connected=false;poll();connected=true;poll();
 double covered=0;
 for(int k=0;k<8;++k) {
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  event(SDL_SENSOR_ACCEL,0,9.80665f,0,true);event(SDL_SENSOR_GYRO,0,1,0,true);poll();
  for(size_t j=0;j<g_measured_motion[0].count;++j)covered+=g_measured_motion[0].steps[j].dt;
 }
 check(5,covered>.05 && covered<.2,"Wii's untimed 100 Hz sensor intervals do not collapse to 1 kHz host intervals");
 nun=false;connected=false;poll();connected=true;poll();packet();
 check(5,!last.nunchuk.available && last.nunchuk.buttons==0,"missing Nunchuk does not fabricate attachment/buttons");

 bar=true;++ir_serial;ir_data[0]=.4f;ir_data[1]=.3f;packet();
 check(6,g_wii_sensorbar_visible[0] && std::abs(last.ir_x-.5)<.0001 && std::abs(last.ir_y-.5)<.0001,"first visible bar sets the current pointing reference");
 ++ir_serial;ir_data[0]=.55f;ir_data[1]=.4f;ir_data[2]=.8f;ir_data[3]=-.6f;packet();
 check(6,std::abs(last.ir_x-.65f)<.0001 && std::abs(last.ir_y-.6f)<.0001,"physical IR movement reaches the guest pointer");
 check(6,std::abs(last.ir_horizon_x-.8f)<.0001 && std::abs(last.ir_horizon_y+.6f)<.0001,"physical IR roll reaches the guest horizon");
 std::this_thread::sleep_for(std::chrono::milliseconds(130));poll();
 check(6,!g_wii_sensorbar_visible[0] && last.ir_x<0 && last.ir_y<0,"stalled IR packets cannot hold a stale visible bar");
 bar=false;++ir_serial;packet();check(6,last.ir_x<0 && last.ir_y<0,"bar leaving the camera disables physical IR");
 bar=true;++ir_serial;packet();check(6,std::abs(last.ir_x-.65f)<.0001,"regaining IR does not silently reset the reference");
 buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN]=1;++ir_serial;packet();
 check(6,std::abs(last.ir_x-.5f)<.0001 && std::abs(last.ir_y-.5f)<.0001,"Down recalibrates the sensor bar's pointing origin");clear();
 has_sensors=false;connected=false;poll();connected=true;poll();
 check(5,!g_gyro_drives_channel[0] && !last.motion.available,"missing MotionPlus is reported instead of synthesizing physical gyro");
 std::printf("%s %u/%u checks\n",failures?"FAIL":"PASS",checks-failures,checks);
 return failures?1:0;
}
