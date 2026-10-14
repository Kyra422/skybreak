#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace nwii {
namespace runtime {

enum class Platform {
    Wii,
    GameCube,
    WiiU
};

enum class Backend {
    OpenGL,
    Metal
};

struct GameProfile {

    uint32_t callback_stack_top = 0;

    std::vector<std::string> disable_modules;

    
    std::string rpl_path;

    

    uint32_t ext_interrupt_dispatch = 0;

    

    uint32_t sched_lock_offset = 0;
};

class Config {
public:
    static Config& get() {
        static Config instance;
        return instance;
    }

    void load(const std::string& filepath);

    // PKG10 (M7.1): folder of the loaded settings file (absolute, UTF-8; empty
    // until load() found a file). Path settings ([nand] root, [graphics]
    // screenshot_dir) go through resolve_path: %NAME% environment variables
    // are expanded (e.g. "%APPDATA%/SkywardSwordPC/nand") and a relative path
    // is taken relative to this folder, so a packaged build works from any
    // location. Absolute paths are unchanged. Result is UTF-8.
    std::string config_dir;
    std::string resolve_path(const std::string& value) const;


    void load_game_profile();

    Platform platform = Platform::Wii;
    Backend backend = Backend::OpenGL;
    // [graphics] vsync: swap interval 1 (true) or 0 (false; present only new
    // frames). Env NWII_SWAP_INTERVAL (-1/0/1) overrides.
    bool enable_vsync = true;
    // [graphics] window_width/height: initial window size. main.cpp keeps these
    // equal to the current window size (SDL points) after resizes and
    // fullscreen toggles, so the mouse pointer mapping follows the window.
    int window_width = 640;
    int window_height = 480;
    // RES9 (M7.3) [graphics] settings:
    // internal_scale: EFB render scale 1..6 (Dolphin "internal resolution",
    // EFB = 640*s x 528*s); 0 = auto: 2 if the GPU allows, else 1. TOML accepts
    // an integer or "auto". Env NWII_INTERNAL_SCALE overrides.
    int internal_scale = 0;
    // fullscreen: start in borderless fullscreen (F11 / Alt+Enter toggles).
    // Env NWII_FULLSCREEN=0/1 overrides.
    bool fullscreen = false;
    // aspect: 0 auto (16:9 when the game runs in widescreen, i.e. SYSCONF
    // IPL.AR = 1; else 4:3), 1 = 4:3, 2 = 16:9, 3 = stretch to the window.
    // TOML: "auto", "4:3", "16:9", "stretch".
    int aspect_mode = 0;
    // screenshot_dir: F12 full-resolution screenshots. Empty = the first of
    // <exe dir>/../../pc-testresultaten/screenshots (when pc-testresultaten
    // exists) or <exe dir>/screenshots.
    std::string screenshot_dir;
    std::string game_dir;

    
    
    std::string game_id = "RSZK";

    

    

    
    int input_mode = 6;

    float gyro_sensitivity = 1.0f;

    float pointer_speed = 1.0f;

    int phone_port = 4313;

    // INPUT10 (M4.3) [input] keys. Parsed by load_input10 (config.cpp, types only); resolved and
    // validated with log messages by src/input/input_bindings.h. The defaults keep the controls
    // exactly as before INPUT10.
    // gamepad_motion: "off" (default) or "gyro" = the controller's real gyroscope/accelerometer
    // (DualSense, DualShock 4, Switch Pro) drive the virtual MotionPlus remote. In game: hold the
    // touchpad or Create+Options for 1 s to switch.
    std::string gamepad_motion = "off";
    float gyro_deadband = 0.02f; // rad/s; zero disables idle suppression
    bool gyro_auto_calibrate = true;   // legacy key, retained for file compatibility; no in-play bias learning
    float gyro_grip_tilt = 0.0f;       // degrees the far edge is raised when you hold the pad "level"
    float mouse_sensitivity = 1.0f;    // Shift + mouse relative turn, x 0.008 rad/px
    float aim_stick_speed = 1.0f;      // right-stick aim / menu cursor speed multiplier
    // Fractions of full deflection; < 0 = built-in default (8000/32768 sticks, 16384/32768 triggers).
    float left_stick_deadzone = -1.0f, right_stick_deadzone = -1.0f, trigger_threshold = -1.0f;
    // [input.keyboard] / [input.gamepad]: action name -> input names, in file order.
    std::vector<std::pair<std::string, std::vector<std::string>>> keyboard_bindings, gamepad_bindings;
    // Type errors found while reading the [input] keys (also printed when found).
    std::vector<std::string> input_config_errors;
    // CTRL18-INPUT (work/claude-notes/CTRL18-design.md section 6): control presets, parsed by load_input10.
    // preset_gamepad: "switch-hd" (Skyward Sword HD button-only layout, default) | "classic" (the mapping before
    // CTRL18, byte-identical) | "wii-motion" (gyro = the Wii Remote, right stick = camera). preset_keyboard: "kbm"
    // (default) | "classic". [input.gamepad_hd] / [input.kbm]: rebinds of the switch-hd / kbm intents (same format
    // as [input.gamepad] / [input.keyboard], which stay the classic tables). Env overrides for tests:
    // NWII_INPUT_PRESET_GAMEPAD / NWII_INPUT_PRESET_KEYBOARD.
    std::string preset_gamepad = "switch-hd";
    std::string preset_keyboard = "kbm";
    bool gyro_player_space = false; // optional gravity-relative aim; puzzles retain all axes
    bool gyro_aim = false;            // switch-hd: the controller's gyro also aims (HD "motion aiming"), aim contexts only
    bool a_does_not_dash = true;      // switch-hd/kbm: A never sprints (HD); B / Shift does
    bool fire_on_release = false;     // stick/mouse sword: swing when the stick returns instead of at the rim
    bool quick_items = true;          // kbm: keys 1-8 pick an item-wheel slot (short input sequence, no game writes)
    float mouse_camera_sensitivity = 1.0f; // [mouse] sensitivity_camera
    float mouse_aim_sensitivity = 1.0f;    // [mouse] sensitivity_aim (kbm aim mode, 0.008 rad/px x this)
    bool mouse_invert_y = false;           // [mouse] invert_y (kbm aim)
    bool flight_invert_y = false;          // [camera] flight_invert_y (Loftwing/swim/skydive: stick up = climb)
    std::vector<std::pair<std::string, std::vector<std::string>>> gamepad_hd_bindings, kbm_bindings;

    // Host-backed NAND (M6.1). Empty = <exe dir>/../nand; env NWII_NAND_ROOT
    // overrides. TOML: [nand] root = "C:/path/to/nand".
    std::string nand_root;
    // SYSCONF IPL.AR at boot: -1 keeps the NAND's value (16:9 when the default
    // SYSCONF is generated), 0 forces 4:3, 1 forces 16:9. TOML: [nand] widescreen.
    int nand_widescreen = -1;

    // SPK11 (M5.4) [audio] block. remote_speaker_volume: gain of the Wii Remote speaker channel mixed
    // into the PC audio output (0.0 .. 2.0, default 0.8). 0 = speaker off: the game is told the Wii
    // speaker volume is 0 and plays those sounds through the normal (TV) mix instead.
    float remote_speaker_volume = 0.8f;

    GameProfile game_profile;

private:
    Config() = default;
};

} 
} 
