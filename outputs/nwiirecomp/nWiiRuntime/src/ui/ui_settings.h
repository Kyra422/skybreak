#pragma once
// UI13: the values the settings screens edit, where they are saved, and how the saved user file
// is laid over the profile at start. Pure C++ on top of runtime::Config (no SDL/GL/ImGui).
//
// User settings file (runtime::write_user_settings keeps comments and unrelated keys):
//   1. env NWII_UI_SETTINGS_FILE, else [ui] settings_file;
//   2. the loaded config itself when it is named instellingen.toml (the release folder, PKG10);
//   3. dev layout: <config dir>/../pc-prototype/instellingen.toml (profile in outputs/pc-profile;
//      the profile recomp_config.toml itself is never written);
//   4. otherwise instellingen.toml next to the config.
// At start (ApplyUserSettings) a user file other than the config is loaded over it with the same
// Config::load (it only sets the keys it contains). Probe/replay/headless runs skip this, so test
// runs never depend on what the player chose.
#include "runtime/config.h"
#include "runtime/ui_config.h"
#include "runtime/hdtex_config.h" // HDTEX16
#include "runtime/interp_config.h" // INTERP17

#include <map> // CTRL18-UI
#include <string>
#include <vector>

namespace nwii::ui {

// True for automated runs (NWII_RUN_SECONDS, NWII_SS_INPUT_REPLAY or NWII_HEADLESS set): no
// launcher, no user settings, no overlay. NWII_UI_FORCE=1 overrides (UI tests).
bool AutomatedRun();

struct Settings {
    // [graphics]
    int internal_scale = 0; // 0 = auto, 1..6
    bool fullscreen = false;
    bool vsync = true;
    int aspect = 0; // 0 auto, 1 4:3, 2 16:9, 3 stretch
    // [graphics] HDTEX16 (runtime/hdtex_config.h): custom (HD) textures, preload, texture dump
    bool hd_textures = true; // "auto" (key absent) shows as on: on when a pack exists
    bool hd_preload = false;
    bool texture_dump = false;
    // [graphics] INTERP17 (runtime/interp_config.h): 60 fps through in-between frames (live toggle)
    bool frame_interpolation = false;
    // [ui]
    std::string present_pacing = "auto";
    int audio_latency_ms = 0;
    bool launcher = true;
    bool toasts = true;
    // [ui] THPVID19: launcher background video (field names = LAUNCH19's ui_settings_fields.h; rows on its INTERFACE tab)
    bool background_video = true;      // background_video
    bool background_music = true;      // background_music
    int background_music_volume = 60;  // background_music_volume 0..100 (user decision: 60)
    std::string background_video_file; // background_video_file "file.thp[:N]" ("" = the film of `background`)
    std::string background;            // background "file.thp[:N]" ("" = Demo01_02.thp:400): the still (video off)
    // [audio]
    float speaker_volume = 0.8f;
    // [input]
    bool gyro = false;
    float grip_tilt = 0.0f;
    float mouse_sensitivity = 1.0f;
    float aim_stick_speed = 1.0f;
    float left_deadzone = -1.0f, right_deadzone = -1.0f; // < 0 = built-in default
    // CTRL18-UI: controls. [input] preset_gamepad / preset_keyboard / gyro_aim and [mouse] sensitivity_camera
    // (CTRL18-INPUT, Config); [input] rumble 0..100 and [camera] free_camera / speed / invert_x / invert_y
    // (CTRL18-CAM, runtime/camera_stick.h atomics: live); [camera] flight_invert_y (runtime/ui_config.h, read by the
    // Loftwing steering); [graphics] glyph_set (runtime/hdtex_config.h, live through HdTexSetGlyphSet).
    std::string preset_gamepad = "switch-hd", preset_keyboard = "kbm";
    bool gyro_aim = false;
    bool gyro_player_space = false;
    int rumble = 100;
    bool free_camera = true;
    int camera_speed = 3;
    bool camera_invert_x = false, camera_invert_y = false, flight_invert_y = false;
    float mouse_camera_sensitivity = 1.0f, mouse_aim_sensitivity = 1.0f; // [mouse] sensitivity_camera / sensitivity_aim
    std::string glyph_set = "auto";
    // Rebinds (overrides only, action -> inputs) of the four tables; see src/ui/ui_controls.h.
    using BindMap = std::map<std::string, std::vector<std::string>>;
    BindMap bind_pad_hd, bind_pad_classic, bind_kbm, bind_kb_classic;

    // LAUNCH19: keys that worked before but had no UI (Config / ui_config / interp / camera atomics) ...
    int widescreen = -1;                 // [nand] widescreen (-1 = key absent = SYSCONF default 16:9, 0 = 4:3, 1 = 16:9)
    std::string nand_root;               // [nand] root ("" = default)
    bool interp_2d = true;               // [graphics] frame_interpolation_2d
    std::string interp_mode = "interp";    // [graphics] frame_interpolation_mode (MVGEN21: auto/rerender/warp/dup)
    std::string texture_pack_dir;        // [graphics] texture_pack_dir
    std::string screenshot_dir;          // [graphics] screenshot_dir
    bool a_does_not_dash = true;         // [input] a_does_not_dash (CTRL18-INPUT)
    bool quick_items = true;             // [input] quick_items
    bool fire_on_release = false;        // [input] fire_on_release
    float trigger_threshold = -1.0f;     // [input] trigger_threshold (< 0 = built-in)
    bool gyro_auto_calibrate = true;     // [input] gyro_auto_calibrate
    bool mouse_invert_y = false;         // [mouse] invert_y
    int auto_recentre_s = 0;             // [camera] auto_recentre_s (CTRL18-CAM)
    bool game_font = true;               // [ui] game_font
    bool overlay = true;                 // [ui] overlay
    // ... and new keys (runtime/ui_config.h, config.cpp load_launch19)
    std::string game_language = "en";    // [nand] language en|fr|es -> SYSCONF IPL.LNG at boot
    int save_backups = 5;                // [nand] backups 0 = off, else keep N (user decision: last 5, on)
    int master_volume = 100;             // [audio] master_volume 0..100
    bool mute_unfocused = false;         // [audio] mute_when_unfocused
    std::string audio_device;            // [audio] device ("" = Windows default)
    bool pause_unfocused = false;        // [game] pause_when_unfocused (experimental, user decision: default off)
    std::string clock = "host";          // [game] clock host|fixed (-> NWII_RTC)
    std::string ui_language = "nl";      // [ui] language nl|en
    int ui_scale = 0;                    // [ui] scale 0 = auto, else percent
    std::string accent = "gold";         // [ui] accent gold|red|sky
    std::string perf_overlay = "off";    // [ui] perf_overlay off|fps|frametime|full
    bool remember_window = true;         // [ui] remember_window
    std::string confirm_button = "east"; // [ui] confirm_button east|south
    bool crash_report = false;           // [diag] crash_report (-> NWII_CRASH_REPORT)
    bool freeze_watchdog = true;         // [diag] freeze_watchdog (-> NWII_FREEZE_WATCHDOG)
    bool directinput = false;            // [input] directinput (-> NWII_DIRECTINPUT)
    bool color_screenlinear = false;     // [graphics] color_screenlinear (-> NWII_COLOR_SCREENLINEAR)
    bool discord_presence = true;
    bool discord_show_location = true;
    bool copy_filter = true;             // [graphics] copy_filter (false -> NWII_COPY_FILTER=0)
};

Settings FromConfig(const runtime::Config& cfg);
// Writes the values into Config (what the runtime reads afterwards; some only at the next start).
void ToConfig(const Settings& s, runtime::Config& cfg);
// Edits for the keys whose value differs between `before` and `after`.
std::vector<runtime::SettingEdit> DiffEdits(const Settings& before, const Settings& after);
// All keys of `s` as edits (used by tests and "reset").
std::vector<runtime::SettingEdit> AllEdits(const Settings& s);

// Which fields need a restart when changed while the game runs (launcher: before boot most apply).
// LAUNCH19: generated from the catalog's apply levels (Restart always, AtBoot in game); compare against what the
// running SkyBreak/game started with ("boot"), not the saved file, so the marker stays until the restart happened.
struct RestartInfo {
    bool any = false;
    std::vector<std::string> what; // labels (Dutch, or English with en)
    std::vector<std::string> ids;  // LAUNCH19: catalog ids
};
RestartInfo NeedsRestart(const Settings& before, const Settings& after, bool in_game, bool en = false);
#ifdef LAUNCH19_LEGACY_EDITS // test builds: the UI13/CTRL18-UI hand-written write-back + restart rules (reference)
std::vector<runtime::SettingEdit> LegacyEdits(const Settings& after, const Settings* before);
RestartInfo NeedsRestartLegacy(const Settings& before, const Settings& after, bool in_game);
#endif

std::string UserSettingsPath(const std::string& config_path_utf8, const runtime::Config& cfg);
// Merge at start. Returns the user file path that was loaded (empty when none / skipped).
std::string ApplyUserSettings(const std::string& config_path_utf8);
// NWII_PRESENT_PACING / NWII_AUDIO_LATENCY_MS from [ui] when the player did not set them himself.
// LAUNCH19: also the keys whose readers read an environment switch: [audio] device -> NWII_AUDIO_DEVICE, [nand]
// language -> NWII_GAME_LANGUAGE, [game] clock fixed -> NWII_RTC=0, [diag] crash_report -> NWII_CRASH_REPORT=1,
// [diag] freeze_watchdog false -> NWII_FREEZE_WATCHDOG=0, [input] directinput -> NWII_DIRECTINPUT=1, [graphics]
// color_screenlinear -> NWII_COLOR_SCREENLINEAR=1, copy_filter false -> NWII_COPY_FILTER=0. A variable that was set
// before SkyBreak started is never touched (it wins; the settings page shows the row locked).
void ApplyEnvFromConfig(const runtime::Config& cfg);
// LAUNCH19: true when `name` was set by the player before SkyBreak started (not by ApplyEnvFromConfig).
bool EnvSetByPlayer(const char* name);
// LAUNCH19: removes the variables ApplyEnvFromConfig set itself (before SkyBreak restarts itself: the new process
// would otherwise take them for the player's own and lock the rows).
void ClearBridgedEnv();

// CTRL18-UI: a settings file that does not parse (TOML error) is ignored as a whole by Config::load. Returns "" when
// `path_utf8` is missing or parses, else a Dutch description ("regel 12: [graphics] staat er twee keer in").
std::string SettingsFileProblem(const std::string& path_utf8);
// Saves `after` to the user file. A file that does not parse is first renamed to <file>.bak (or .bak2, .bak3, ...:
// an older backup is never overwritten) and a fresh file with ALL current values is written; otherwise only the keys
// that differ from `before` are edited (comments and unknown keys kept).
struct SaveOutcome {
    bool ok = false;
    size_t keys = 0;
    std::string error;  // when !ok
    std::string backup; // the backup file made (broken file), else ""
};
SaveOutcome SaveUserSettings(const std::string& path_utf8, const Settings& before, const Settings& after);

// Built-in defaults of the stick deadzones (input_bindings.h: 8000 / 32768).
inline constexpr float kDefaultStickDeadzone = 8000.0f / 32768.0f;

} // namespace nwii::ui
