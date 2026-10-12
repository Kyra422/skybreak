#pragma once
// UI13: the [ui] settings block and the user-settings write-back. Parsed by Config::load
// (src/core/config.cpp, load_ui13) like every other block, but kept out of config.h on purpose:
// every generated output_*.cpp includes runtime/config.h, so a change there rebuilds the whole game.
//
// [ui]
//   launcher = true        title/launcher screen before the game boots. Also skipped by the argument
//                          --no-launcher, NWII_NO_LAUNCHER=1, and always for probe/replay/headless runs
//                          (NWII_RUN_SECONDS / NWII_SS_INPUT_REPLAY / NWII_HEADLESS set)
//   overlay = true         in-game menu: Esc / F1 / controller Mute (DualSense), Share (Xbox Series),
//                          Capture (Switch)
//   toasts = true          controller connected/disconnected notices
//   game_font = true       titles in the disc's own font (files/US/Font/en_US/normal_00.brfnt)
//   background = ""        launcher scene "<file in files/THP>:<frame>" (empty = "Demo01_02.thp:400")
//   background_video = true          THPVID19: a film from the disc plays in a loop behind the launcher
//                          (src/ui/ui_thp_player.h; NWII_UI_VIDEO=0|1)
//   background_music = true          THPVID19: the film's music (muted while the window is inactive, stops when
//                          the game starts; NWII_UI_MUSIC=0|1)
//   background_music_volume = 60     THPVID19: its volume 0..100 (user decision: 60; NWII_UI_MUSIC_VOLUME)
//   background_video_file = ""       THPVID19: the film "<file in files/THP>[:<start frame>]" (empty = the film of
//                          `background`, starting at its still frame; NWII_UI_VIDEO_FILE)
//   settings_file = ""     where the settings screens save (empty = automatic, src/ui/ui_settings.h)
//   present_pacing = "auto" | "dwm" | "vsync" | "none"  -> NWII_PRESENT_PACING when that is not set
//   audio_latency_ms = 0   0 = default (80 ms), 20..1000 -> NWII_AUDIO_LATENCY_MS when that is not set
// [camera] flight_invert_y = false  CTRL18-UI: Loftwing climb/dive inverted (separate from the camera's invert_y)
//   gyro_hint_shown = false  CTRL18-UI: the one-time "aim with the gyro?" hint was shown (a gyro pad, gyro_aim off)
#include <climits> // LAUNCH19
#include <string>
#include <vector>

namespace nwii::runtime {

struct UiConfig {
    bool launcher = true;
    bool overlay = true;
    bool toasts = true;
    bool game_font = true;
    std::string background;
    bool background_video = true;       // THPVID19/LAUNCH19 [ui] background_video
    bool background_music = true;       // THPVID19/LAUNCH19 [ui] background_music
    int background_music_volume = 60;   // THPVID19/LAUNCH19 [ui] background_music_volume 0..100 (user decision: 60)
    std::string background_video_file;  // THPVID19/LAUNCH19 [ui] background_video_file ("" = the film of `background`)
    std::string settings_file; // resolved (Config::resolve_path)
    std::string present_pacing = "auto";
    int audio_latency_ms = 0;
    bool gyro_hint_shown = false; // CTRL18-UI
    bool flight_invert_y = false; // CTRL18-UI: [camera] flight_invert_y (Loftwing up/down inverted; read by the input layer)
    // LAUNCH19 (config.cpp load_launch19; the settings catalog src/ui/ui_settings_catalog.h describes every key). Keys
    // that replace an NWII_* switch reach their reader as that variable (ui_settings.cpp ApplyEnvFromConfig; a
    // variable the player set himself wins).
    std::string language = "nl";        // [ui] language nl|en (launcher, F1 menu, notices)
    int scale = 0;                      // [ui] scale 0 = auto (by window size), else percent 75..200
    std::string accent = "gold";        // [ui] accent gold|red|sky
    std::string perf_overlay = "off";   // [ui] perf_overlay off|fps|frametime|full
    bool remember_window = true;        // [ui] remember_window (size/position/fullscreen saved at exit)
    std::string confirm_button = "east"; // [ui] confirm_button east (Switch layout, user decision) | south
    int window_x = INT_MIN, window_y = INT_MIN; // [graphics] window_x / window_y (remember_window; INT_MIN = centred)
    int master_volume = 100;            // [audio] master_volume 0..100 (audio_out output gain)
    bool mute_when_unfocused = false;   // [audio] mute_when_unfocused
    std::string audio_device;           // [audio] device ("" = Windows default) -> NWII_AUDIO_DEVICE
    std::string game_language = "en";   // [nand] language en|fr|es -> NWII_GAME_LANGUAGE (SYSCONF IPL.LNG, nand_fs.cpp)
    bool game_language_set = false;     //   the key is present (absent = SYSCONF untouched)
    int backups = 5;                    // [nand] backups 0 = off, else keep the newest N save backups (user decision: 5)
    bool pause_when_unfocused = false;  // [game] pause_when_unfocused (experimental, user decision: default off)
    std::string clock = "host";         // [game] clock host|fixed -> NWII_RTC (fixed = "0")
    bool crash_report = false;          // [diag] crash_report -> NWII_CRASH_REPORT=1
    bool freeze_watchdog = true;        // [diag] freeze_watchdog -> NWII_FREEZE_WATCHDOG=0 when false
    bool directinput = false;           // [input] directinput -> NWII_DIRECTINPUT=1
    bool color_screenlinear = false;    // [graphics] color_screenlinear -> NWII_COLOR_SCREENLINEAR=1
    bool discord_presence = true;
    bool discord_show_location = true;
    std::string discord_large_image; // Optional Developer Portal asset key.
    bool copy_filter = true;            // [graphics] copy_filter -> NWII_COPY_FILTER=0 when false
};
// CTRL18-UI: parse check of a settings file (toml++). "" = parses (or missing); else "line N: <toml++ description>".
std::string toml_file_problem(const std::string& path_utf8, int* line = nullptr);
UiConfig& ui_config(); // config.cpp

// One edit: `key = value_toml` in [section]. value_toml is a TOML literal ("\"auto\"", "2", "true").
// CTRL18-UI: an empty value_toml removes the key (the runtime default applies again).
struct SettingEdit {
    std::string section, key, value_toml;
};
// Applies the edits to TOML text, keeping every other line, comment and the key's own trailing comment;
// a missing key goes to the end of its section, a missing section to the end of the text.
std::string apply_setting_edits(const std::string& toml_text, const std::vector<SettingEdit>& edits);
// Applies the edits to the file at path_utf8 (created with a short Dutch header when missing). The result
// must parse as TOML before it replaces the file (written to <path>.tmp, then renamed).
bool write_user_settings(const std::string& path_utf8, const std::vector<SettingEdit>& edits,
                         std::string* error = nullptr);

} // namespace nwii::runtime
