// UI13: settings model, user settings file and merge (see ui_settings.h).
#include "ui_settings.h"

#include "runtime/camera_stick.h" // CTRL18-UI: CTRL18-CAM's [camera] / rumble atomics
#include "ui_settings_catalog.h"     // LAUNCH19: the one settings table (generic write-back / restart rules)

#include <cctype> // CTRL18-UI
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <system_error> // CTRL18-UI

namespace nwii::ui {

namespace {
namespace fs = std::filesystem;
fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string p8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}
void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str()); // "" removes the variable
#else
    if (value.empty()) unsetenv(name); else setenv(name, value.c_str(), 1);
#endif
}
std::string fnum(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}
const char* aspect_name(int a) { return a == 1 ? "4:3" : a == 2 ? "16:9" : a == 3 ? "stretch" : "auto"; }
bool feq(float a, float b) { return std::fabs(a - b) < 0.0005f; }
// CTRL18-UI: Config's binding list <-> the UI's override map
Settings::BindMap to_map(const std::vector<std::pair<std::string, std::vector<std::string>>>& v) {
    Settings::BindMap m;
    for (const auto& [k, list] : v) m[k] = list;
    return m;
}
std::vector<std::pair<std::string, std::vector<std::string>>> to_list(const Settings::BindMap& m) {
    return {m.begin(), m.end()};
}
std::string toml_str(const std::string& v) {
    std::string o = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}
std::string toml_list(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + toml_str(v[i]);
    return o + "]";
}
std::string toml_key(const std::string& k) {
    for (char c : k)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return toml_str(k);
    return k;
}
} // namespace

bool AutomatedRun() {
    if (const char* f = std::getenv("NWII_UI_FORCE"); f && *f == '1') return false;
    return env_set("NWII_RUN_SECONDS") || env_set("NWII_SS_INPUT_REPLAY") ||
           (std::getenv("NWII_HEADLESS") && std::getenv("NWII_HEADLESS")[0] == '1');
}

Settings FromConfig(const runtime::Config& c) {
    const runtime::UiConfig& u = runtime::ui_config();
    Settings s;
    s.internal_scale = c.internal_scale;
    s.fullscreen = c.fullscreen;
    s.vsync = c.enable_vsync;
    s.aspect = c.aspect_mode;
    s.hd_textures = runtime::hdtex_config().hd_textures != 0; // HDTEX16
    s.hd_preload = runtime::hdtex_config().preload;          // HDTEX16
    s.texture_dump = runtime::hdtex_config().dump;           // HDTEX16
    s.frame_interpolation = runtime::interp_config().enabled.load() != 0; // INTERP17
    s.present_pacing = u.present_pacing;
    s.audio_latency_ms = u.audio_latency_ms;
    s.launcher = u.launcher;
    s.toasts = u.toasts;
    s.background_video = u.background_video;               // THPVID19
    s.background_music = u.background_music;               // THPVID19
    s.background_music_volume = u.background_music_volume; // THPVID19
    s.background_video_file = u.background_video_file;     // THPVID19
    s.background = u.background;                           // THPVID19
    s.speaker_volume = c.remote_speaker_volume;
    s.gyro = c.gamepad_motion == "gyro";
    s.grip_tilt = c.gyro_grip_tilt;
    s.mouse_sensitivity = c.mouse_sensitivity;
    s.aim_stick_speed = c.aim_stick_speed;
    s.left_deadzone = c.left_stick_deadzone;
    s.right_deadzone = c.right_stick_deadzone;
    // CTRL18-UI
    namespace in = runtime::input;
    s.preset_gamepad = c.preset_gamepad;
    s.preset_keyboard = c.preset_keyboard;
    s.gyro_aim = c.gyro_aim;
    s.gyro_player_space = c.gyro_player_space;
    s.mouse_camera_sensitivity = c.mouse_camera_sensitivity;
    s.mouse_aim_sensitivity = c.mouse_aim_sensitivity;
    s.rumble = in::g_rumble_strength.load();
    s.free_camera = in::g_free_camera.enabled.load();
    s.camera_speed = in::g_free_camera.speed.load();
    s.camera_invert_x = in::g_free_camera.invert_x.load();
    s.camera_invert_y = in::g_free_camera.invert_y.load();
    s.flight_invert_y = u.flight_invert_y;
    s.glyph_set = runtime::hdtex_config().glyph_set;
    s.bind_pad_hd = to_map(c.gamepad_hd_bindings);
    s.bind_pad_classic = to_map(c.gamepad_bindings);
    s.bind_kbm = to_map(c.kbm_bindings);
    s.bind_kb_classic = to_map(c.keyboard_bindings);
    // LAUNCH19: keys without UI before, and the new keys (runtime/ui_config.h)
    s.widescreen = c.nand_widescreen;
    s.nand_root = c.nand_root;
    s.interp_2d = runtime::interp_config().interp_2d.load() != 0;
    s.interp_mode = runtime::interp_config().dup_mode.load() ? "dup" : "interp";
    s.texture_pack_dir = runtime::hdtex_config().pack_dir;
    s.screenshot_dir = c.screenshot_dir;
    s.a_does_not_dash = c.a_does_not_dash;
    s.quick_items = c.quick_items;
    s.fire_on_release = c.fire_on_release;
    s.trigger_threshold = c.trigger_threshold;
    s.gyro_deadband = c.gyro_deadband;
    s.gyro_auto_calibrate = c.gyro_auto_calibrate;
    s.mouse_invert_y = c.mouse_invert_y;
    s.auto_recentre_s = in::g_free_camera.auto_recentre_s.load();
    s.game_font = u.game_font;
    s.overlay = u.overlay;
    s.game_language = u.game_language;
    s.save_backups = u.backups;
    s.master_volume = u.master_volume;
    s.mute_unfocused = u.mute_when_unfocused;
    s.audio_device = u.audio_device;
    s.pause_unfocused = u.pause_when_unfocused;
    s.clock = u.clock;
    s.ui_language = u.language;
    s.ui_scale = u.scale;
    s.accent = u.accent;
    s.perf_overlay = u.perf_overlay;
    s.remember_window = u.remember_window;
    s.confirm_button = u.confirm_button;
    s.crash_report = u.crash_report;
    s.freeze_watchdog = u.freeze_watchdog;
    s.directinput = u.directinput;
    s.color_screenlinear = u.color_screenlinear;
    s.discord_presence = u.discord_presence;
    s.discord_show_location = u.discord_show_location;
    s.copy_filter = u.copy_filter;
    return s;
}

void ToConfig(const Settings& s, runtime::Config& c) {
    runtime::UiConfig& u = runtime::ui_config();
    c.internal_scale = s.internal_scale;
    c.fullscreen = s.fullscreen;
    c.enable_vsync = s.vsync;
    c.aspect_mode = s.aspect;
    runtime::hdtex_config().hd_textures = s.hd_textures ? 1 : 0; // HDTEX16 (1 = on when a pack exists)
    runtime::hdtex_config().preload = s.hd_preload;               // HDTEX16
    runtime::hdtex_config().dump = s.texture_dump;                // HDTEX16
    runtime::interp_config().enabled.store(s.frame_interpolation ? 1 : 0); // INTERP17: read every frame (live)
    u.present_pacing = s.present_pacing;
    u.audio_latency_ms = s.audio_latency_ms;
    u.launcher = s.launcher;
    u.toasts = s.toasts;
    u.background_video = s.background_video;               // THPVID19: the hook reads ui_config() every frame (live)
    u.background_music = s.background_music;               // THPVID19
    u.background_music_volume = s.background_music_volume; // THPVID19
    u.background_video_file = s.background_video_file;     // THPVID19
    u.background = s.background;                           // THPVID19
    c.remote_speaker_volume = s.speaker_volume;
    c.gamepad_motion = s.gyro ? "gyro" : "off";
    c.gyro_grip_tilt = s.grip_tilt;
    c.mouse_sensitivity = s.mouse_sensitivity;
    c.aim_stick_speed = s.aim_stick_speed;
    c.left_stick_deadzone = s.left_deadzone;
    c.right_stick_deadzone = s.right_deadzone;
    // CTRL18-UI (the input layer re-reads Config after request_input_bindings_reload(); the camera atomics are live)
    namespace in = runtime::input;
    c.preset_gamepad = s.preset_gamepad;
    c.preset_keyboard = s.preset_keyboard;
    c.gyro_aim = s.gyro_aim;
    c.gyro_player_space = s.gyro_player_space;
    c.mouse_camera_sensitivity = s.mouse_camera_sensitivity;
    c.mouse_aim_sensitivity = s.mouse_aim_sensitivity;
    in::g_free_camera.mouse_sensitivity.store(s.mouse_camera_sensitivity);
    in::g_rumble_strength.store(s.rumble);
    in::g_free_camera.enabled.store(s.free_camera);
    in::g_free_camera.speed.store(s.camera_speed);
    in::g_free_camera.invert_x.store(s.camera_invert_x);
    in::g_free_camera.invert_y.store(s.camera_invert_y);
    u.flight_invert_y = s.flight_invert_y;
    runtime::hdtex_config().glyph_set = s.glyph_set;
    c.gamepad_hd_bindings = to_list(s.bind_pad_hd);
    c.gamepad_bindings = to_list(s.bind_pad_classic);
    c.kbm_bindings = to_list(s.bind_kbm);
    c.keyboard_bindings = to_list(s.bind_kb_classic);
    // LAUNCH19 (AtBoot keys are read when the game starts; the env-backed ones go through ApplyEnvFromConfig)
    c.nand_widescreen = s.widescreen;
    c.nand_root = s.nand_root;
    runtime::interp_config().interp_2d.store(s.interp_2d ? 1 : 0);
    runtime::interp_config().dup_mode.store(s.interp_mode == "dup" ? 1 : 0);
    runtime::hdtex_config().pack_dir = s.texture_pack_dir;
    c.screenshot_dir = s.screenshot_dir;
    c.a_does_not_dash = s.a_does_not_dash;
    c.quick_items = s.quick_items;
    c.fire_on_release = s.fire_on_release;
    c.trigger_threshold = s.trigger_threshold;
    c.gyro_deadband = s.gyro_deadband;
    c.gyro_auto_calibrate = s.gyro_auto_calibrate;
    c.mouse_invert_y = s.mouse_invert_y;
    in::g_free_camera.auto_recentre_s.store(s.auto_recentre_s);
    u.game_font = s.game_font;
    u.overlay = s.overlay;
    if (u.game_language != s.game_language) u.game_language_set = true;
    u.game_language = s.game_language;
    u.backups = s.save_backups;
    u.master_volume = s.master_volume;
    u.mute_when_unfocused = s.mute_unfocused;
    u.audio_device = s.audio_device;
    u.pause_when_unfocused = s.pause_unfocused;
    u.clock = s.clock;
    u.language = s.ui_language;
    u.scale = s.ui_scale;
    u.accent = s.accent;
    u.perf_overlay = s.perf_overlay;
    u.remember_window = s.remember_window;
    u.confirm_button = s.confirm_button;
    u.crash_report = s.crash_report;
    u.freeze_watchdog = s.freeze_watchdog;
    u.directinput = s.directinput;
    u.color_screenlinear = s.color_screenlinear;
    u.discord_presence = s.discord_presence;
    u.discord_show_location = s.discord_show_location;
    u.copy_filter = s.copy_filter;
}

namespace {
using Edit = runtime::SettingEdit;
void edits_for(const Settings& a, const Settings* before, std::vector<Edit>& out) {
    const auto add = [&](bool changed, const char* sec, const char* key, const std::string& v) {
        if (!before || changed) out.push_back({sec, key, v});
    };
    const Settings& b = before ? *before : a;
    add(a.internal_scale != b.internal_scale, "graphics", "internal_scale",
        a.internal_scale ? std::to_string(a.internal_scale) : "\"auto\"");
    add(a.fullscreen != b.fullscreen, "graphics", "fullscreen", a.fullscreen ? "true" : "false");
    add(a.vsync != b.vsync, "graphics", "vsync", a.vsync ? "true" : "false");
    add(a.aspect != b.aspect, "graphics", "aspect", std::string("\"") + aspect_name(a.aspect) + "\"");
    add(a.hd_textures != b.hd_textures, "graphics", "hd_textures", a.hd_textures ? "true" : "false"); // HDTEX16
    add(a.hd_preload != b.hd_preload, "graphics", "hd_textures_preload", a.hd_preload ? "true" : "false");
    add(a.texture_dump != b.texture_dump, "graphics", "texture_dump", a.texture_dump ? "true" : "false");
    add(a.frame_interpolation != b.frame_interpolation, "graphics", "frame_interpolation", // INTERP17
        a.frame_interpolation ? "true" : "false");
    add(a.present_pacing != b.present_pacing, "ui", "present_pacing", "\"" + a.present_pacing + "\"");
    add(a.audio_latency_ms != b.audio_latency_ms, "ui", "audio_latency_ms", std::to_string(a.audio_latency_ms));
    add(a.launcher != b.launcher, "ui", "launcher", a.launcher ? "true" : "false");
    add(a.toasts != b.toasts, "ui", "toasts", a.toasts ? "true" : "false");
    add(a.background_video != b.background_video, "ui", "background_video", a.background_video ? "true" : "false"); // THPVID19
    add(a.background_music != b.background_music, "ui", "background_music", a.background_music ? "true" : "false");
    add(a.background_music_volume != b.background_music_volume, "ui", "background_music_volume",
        std::to_string(a.background_music_volume));
    add(a.background_video_file != b.background_video_file, "ui", "background_video_file", toml_str(a.background_video_file));
    add(a.background != b.background, "ui", "background", toml_str(a.background)); // THPVID19
    add(!feq(a.speaker_volume, b.speaker_volume), "audio", "remote_speaker_volume", fnum(a.speaker_volume));
    add(a.gyro != b.gyro, "input", "gamepad_motion", a.gyro ? "\"gyro\"" : "\"off\"");
    add(!feq(a.grip_tilt, b.grip_tilt), "input", "gyro_grip_tilt", fnum(a.grip_tilt));
    add(!feq(a.mouse_sensitivity, b.mouse_sensitivity), "input", "mouse_sensitivity", fnum(a.mouse_sensitivity));
    add(!feq(a.aim_stick_speed, b.aim_stick_speed), "input", "aim_stick_speed", fnum(a.aim_stick_speed));
    // A deadzone left at the built-in default is not written ("< 0" has no TOML form in Config).
    if (a.left_deadzone >= 0) add(!feq(a.left_deadzone, b.left_deadzone), "input", "left_stick_deadzone", fnum(a.left_deadzone));
    if (a.right_deadzone >= 0) add(!feq(a.right_deadzone, b.right_deadzone), "input", "right_stick_deadzone", fnum(a.right_deadzone));
    // CTRL18-UI
    add(a.preset_gamepad != b.preset_gamepad, "input", "preset_gamepad", toml_str(a.preset_gamepad));
    add(a.preset_keyboard != b.preset_keyboard, "input", "preset_keyboard", toml_str(a.preset_keyboard));
    add(a.gyro_aim != b.gyro_aim, "input", "gyro_aim", a.gyro_aim ? "true" : "false");
    add(a.gyro_player_space != b.gyro_player_space, "input", "gyro_player_space", a.gyro_player_space ? "true" : "false");
    add(a.rumble != b.rumble, "input", "rumble", std::to_string(a.rumble));
    add(a.free_camera != b.free_camera, "camera", "free_camera", a.free_camera ? "true" : "false");
    add(a.camera_speed != b.camera_speed, "camera", "speed", std::to_string(a.camera_speed));
    add(a.camera_invert_x != b.camera_invert_x, "camera", "invert_x", a.camera_invert_x ? "true" : "false");
    add(a.camera_invert_y != b.camera_invert_y, "camera", "invert_y", a.camera_invert_y ? "true" : "false");
    add(a.flight_invert_y != b.flight_invert_y, "camera", "flight_invert_y", a.flight_invert_y ? "true" : "false");
    add(!feq(a.mouse_camera_sensitivity, b.mouse_camera_sensitivity), "mouse", "sensitivity_camera",
        fnum(a.mouse_camera_sensitivity));
    add(!feq(a.mouse_aim_sensitivity, b.mouse_aim_sensitivity), "mouse", "sensitivity_aim", fnum(a.mouse_aim_sensitivity));
    add(a.glyph_set != b.glyph_set, "graphics", "glyph_set", toml_str(a.glyph_set));
    // Rebinds: a changed / new override is written, a removed one is deleted (empty value = remove the key).
    const auto binds = [&](const Settings::BindMap& am, const Settings::BindMap& bm, const char* section) {
        for (const auto& [k, list] : am) {
            const auto it = bm.find(k);
            if (!before || it == bm.end() || it->second != list) out.push_back({section, toml_key(k), toml_list(list)});
        }
        if (before)
            for (const auto& [k, list] : bm)
                if (!am.count(k)) out.push_back({section, toml_key(k), ""});
    };
    binds(a.bind_pad_hd, b.bind_pad_hd, "input.gamepad_hd");
    binds(a.bind_pad_classic, b.bind_pad_classic, "input.gamepad");
    binds(a.bind_kbm, b.bind_kbm, "input.kbm");
    binds(a.bind_kb_classic, b.bind_kb_classic, "input.keyboard");
}
} // namespace

// LAUNCH19: the write-back is generated from the settings catalog (one row = one key), plus the two things that are
// not catalog rows: the launcher still `background` (THPVID19) and the four rebind tables (CTRL18-UI). The UI13 /
// CTRL18-UI hand-written edits_for above stays as the reference of the byte-compatibility test (outputs/pc-tests/
// launch19), exported as LegacyEdits / NeedsRestartLegacy when LAUNCH19_LEGACY_EDITS is defined (test builds only).
std::vector<runtime::SettingEdit> catalog::EditsFromTable(std::span<const Desc> table, const Settings& a,
                                                         const Settings* before) {
    std::vector<Edit> out;
    for (const catalog::Desc& d : table) {
        if (!catalog::IsValueRow(d)) continue;
        if (before && catalog::SameValue(d, a, *before)) continue;
        bool skip = false;
        const std::string lit = catalog::Literal(d, a, &skip);
        if (skip) {
            if (before) out.push_back({d.section, d.key, ""}); // back to the built-in value: remove the key
            continue;
        }
        out.push_back({d.section, d.key, lit});
    }
    if (!before || a.background != before->background) out.push_back({"ui", "background", toml_str(a.background)});
    const Settings& b = before ? *before : a;
    const auto binds = [&](const Settings::BindMap& am, const Settings::BindMap& bm, const char* section) {
        for (const auto& [k, list] : am) {
            const auto it = bm.find(k);
            if (!before || it == bm.end() || it->second != list) out.push_back({section, toml_key(k), toml_list(list)});
        }
        if (before)
            for (const auto& [k, list] : bm)
                if (!am.count(k)) out.push_back({section, toml_key(k), ""});
    };
    binds(a.bind_pad_hd, b.bind_pad_hd, "input.gamepad_hd");
    binds(a.bind_pad_classic, b.bind_pad_classic, "input.gamepad");
    binds(a.bind_kbm, b.bind_kbm, "input.kbm");
    binds(a.bind_kb_classic, b.bind_kb_classic, "input.keyboard");
    return out;
}

std::vector<runtime::SettingEdit> DiffEdits(const Settings& before, const Settings& after) {
    return catalog::EditsFromTable(catalog::All(), after, &before);
}

std::vector<runtime::SettingEdit> AllEdits(const Settings& s) { return catalog::EditsFromTable(catalog::All(), s, nullptr); }

// LAUNCH19: from the catalog's apply levels. Restart = read when SkyBreak starts (always "na herstart"); AtBoot = read
// when the game boots (from the launcher it simply applies; in game it needs a restart). `what` = the row labels.
RestartInfo NeedsRestart(const Settings& a, const Settings& b, bool in_game, bool en) {
    return catalog::RestartFromTable(catalog::All(), a, b, in_game, en);
}
RestartInfo catalog::RestartFromTable(std::span<const Desc> table, const Settings& a, const Settings& b, bool in_game,
                                      bool en) {
    RestartInfo r;
    for (const catalog::Desc& d : table) {
        if (!catalog::IsValueRow(d) || catalog::SameValue(d, a, b)) continue;
        if (d.apply == catalog::Apply::Restart || (d.apply == catalog::Apply::AtBoot && in_game)) {
            r.any = true;
            r.what.push_back(catalog::T(d.label, en));
            r.ids.push_back(d.id);
        }
    }
    return r;
}

#ifdef LAUNCH19_LEGACY_EDITS
std::vector<runtime::SettingEdit> LegacyEdits(const Settings& a, const Settings* before) {
    std::vector<Edit> out;
    edits_for(a, before, out);
    return out;
}
RestartInfo NeedsRestartLegacy(const Settings& a, const Settings& b, bool in_game) {
    RestartInfo r;
    const auto need = [&](bool changed, const char* what) {
        if (changed) { r.any = true; r.what.push_back(what); }
    };
    // Read once by the host before/at boot: swap interval, present thread pacing.
    need(a.vsync != b.vsync, "VSync");
    need(a.present_pacing != b.present_pacing && in_game, "beeldtempo");
    if (in_game) {
        // Read once when the game starts (EFB size, audio device, input bindings, speaker gain).
        need(a.internal_scale != b.internal_scale, "interne resolutie");
        need(a.hd_textures != b.hd_textures || a.hd_preload != b.hd_preload, "HD-texturen"); // HDTEX16
        need(a.texture_dump != b.texture_dump, "texturen dumpen");                          // HDTEX16
        need(a.audio_latency_ms != b.audio_latency_ms, "audiovertraging");
        need(!feq(a.speaker_volume, b.speaker_volume), "volume Wii Remote-speaker");
        need(!feq(a.grip_tilt, b.grip_tilt), "greephoek");
        need(!feq(a.mouse_sensitivity, b.mouse_sensitivity), "muisgevoeligheid");
        need(!feq(a.aim_stick_speed, b.aim_stick_speed), "richtsnelheid");
        need(!feq(a.left_deadzone, b.left_deadzone) || !feq(a.right_deadzone, b.right_deadzone), "dode zones");
    }
    return r;
}
#endif

std::string UserSettingsPath(const std::string& config_path, const runtime::Config& cfg) {
    if (const char* e = std::getenv("NWII_UI_SETTINGS_FILE"); e && *e) return e;
    if (!runtime::ui_config().settings_file.empty()) return runtime::ui_config().settings_file;
    std::error_code ec;
    fs::path conf = fs::absolute(u8p(config_path), ec);
    if (ec) conf = u8p(config_path);
    if (conf.filename() == "instellingen.toml") return p8(conf.lexically_normal());
    const fs::path dev = conf.parent_path().parent_path() / "pc-prototype";
    if (fs::is_directory(dev, ec)) return p8((dev / "instellingen.toml").lexically_normal());
    return p8((conf.parent_path() / "instellingen.toml").lexically_normal());
}

std::string ApplyUserSettings(const std::string& config_path) {
    auto& cfg = runtime::Config::get();
    if (AutomatedRun()) return {};
    const std::string user = UserSettingsPath(config_path, cfg);
    std::error_code ec;
    if (user.empty() || !fs::is_regular_file(u8p(user), ec)) return {};
    const fs::path a = fs::weakly_canonical(fs::absolute(u8p(config_path), ec), ec);
    const fs::path b = fs::weakly_canonical(u8p(user), ec);
    if (!ec && a == b) return {}; // the config is the user file (release folder)
    const std::string keep_dir = cfg.config_dir;
    std::cout << "[UI13] user settings: " << user << " over " << config_path << std::endl;
    cfg.load(user);
    cfg.config_dir = keep_dir;
    return user;
}

// CTRL18-UI: broken settings files (TOML errors) and saving over them.
std::string SettingsFileProblem(const std::string& path_utf8) {
    int line = 0;
    const std::string p = runtime::toml_file_problem(path_utf8, &line);
    if (p.empty()) return {};
    const size_t colon = p.find(": ");
    std::string what = colon == std::string::npos ? p : p.substr(colon + 2);
    // the common case in a hand-edited file: a table or key written twice
    const size_t q1 = what.find('\''), q2 = q1 == std::string::npos ? q1 : what.find('\'', q1 + 1);
    if (what.find("redefin") != std::string::npos && q2 != std::string::npos) { // "redefinition of" / "cannot redefine"
        const std::string name = what.substr(q1 + 1, q2 - q1 - 1);
        what = (what.find("table") != std::string::npos ? "[" + name + "]" : "'" + name + "'") +
               " staat er twee keer in";
    } else {
        what = "onleesbaar (" + what + ")";
    }
    return "regel " + std::to_string(line) + ": " + what;
}

SaveOutcome SaveUserSettings(const std::string& path_utf8, const Settings& before, const Settings& after) {
    SaveOutcome r;
    std::vector<runtime::SettingEdit> edits;
    if (!SettingsFileProblem(path_utf8).empty()) {
        // keep the player's broken file: <file>.bak, or .bak2, .bak3 ... when an older backup exists
        const fs::path src = u8p(path_utf8);
        std::error_code ec;
        fs::path bak;
        for (int i = 1; i < 1000; ++i) {
            bak = src;
            bak += i == 1 ? std::string(".bak") : ".bak" + std::to_string(i);
            if (!fs::exists(bak, ec)) break;
        }
        fs::rename(src, bak, ec);
        if (ec) {
            r.error = "kon het oude bestand niet bewaren als " + p8(bak) + ": " + ec.message();
            return r;
        }
        r.backup = p8(bak);
        edits = AllEdits(after);
    } else {
        edits = DiffEdits(before, after);
    }
    r.keys = edits.size();
    if (edits.empty() && r.backup.empty()) {
        r.ok = true;
        return r;
    }
    if (!runtime::write_user_settings(path_utf8, edits, &r.error)) {
        if (!r.backup.empty()) { // put the player's file back
            std::error_code ec;
            fs::rename(u8p(r.backup), u8p(path_utf8), ec);
            r.backup.clear();
        }
        return r;
    }
    r.ok = true;
    return r;
}

void ApplyEnvFromConfig(const runtime::Config&) {
    const runtime::UiConfig& cfg = runtime::ui_config();
    (void)EnvSetByPlayer("NWII_RTC"); // LAUNCH19: snapshot of the player's environment before anything below sets one
    // Only variables this function set itself are changed again; a value the player set wins.
    static bool own_pacing = !env_set("NWII_PRESENT_PACING");
    static bool own_latency = !env_set("NWII_AUDIO_LATENCY_MS");
    if (own_pacing) set_env("NWII_PRESENT_PACING", cfg.present_pacing == "auto" ? "" : cfg.present_pacing);
    if (own_latency)
        set_env("NWII_AUDIO_LATENCY_MS", cfg.audio_latency_ms > 0 ? std::to_string(cfg.audio_latency_ms) : "");
    // LAUNCH19: TOML keys for switches that only existed as environment variables (their readers read the variable)
    const auto bridge = [](const char* name, const std::string& value) {
        if (!EnvSetByPlayer(name)) set_env(name, value);
    };
    bridge("NWII_AUDIO_DEVICE", cfg.audio_device);
    bridge("NWII_GAME_LANGUAGE", cfg.game_language_set ? cfg.game_language : "");
    bridge("NWII_RTC", cfg.clock == "fixed" ? "0" : "");
    bridge("NWII_CRASH_REPORT", cfg.crash_report ? "1" : "");
    bridge("NWII_FREEZE_WATCHDOG", cfg.freeze_watchdog ? "" : "0");
    bridge("NWII_DIRECTINPUT", cfg.directinput ? "1" : "");
    bridge("NWII_COLOR_SCREENLINEAR", cfg.color_screenlinear ? "1" : "");
    bridge("NWII_COPY_FILTER", cfg.copy_filter ? "" : "0");
}

bool EnvSetByPlayer(const char* name) {
    // LAUNCH19: the environment as SkyBreak found it (snapshot at the first call, from OnConfigLoaded before any
    // ApplyEnvFromConfig); later values may be ours.
    static const std::map<std::string, bool> kAtStart = [] {
        std::map<std::string, bool> m;
        for (const char* n : {"NWII_AUDIO_DEVICE", "NWII_GAME_LANGUAGE", "NWII_RTC", "NWII_CRASH_REPORT",
                              "NWII_FREEZE_WATCHDOG", "NWII_DIRECTINPUT", "NWII_COLOR_SCREENLINEAR", "NWII_COPY_FILTER",
                              "NWII_PRESENT_PACING", "NWII_AUDIO_LATENCY_MS"})
            m[n] = env_set(n);
        return m;
    }();
    const auto it = kAtStart.find(name);
    return it != kAtStart.end() ? it->second : env_set(name);
}

void ClearBridgedEnv() {
    for (const char* n : {"NWII_AUDIO_DEVICE", "NWII_GAME_LANGUAGE", "NWII_RTC", "NWII_CRASH_REPORT", "NWII_FREEZE_WATCHDOG",
                          "NWII_DIRECTINPUT", "NWII_COLOR_SCREENLINEAR", "NWII_COPY_FILTER", "NWII_PRESENT_PACING",
                          "NWII_AUDIO_LATENCY_MS"})
        if (!EnvSetByPlayer(n)) set_env(n, "");
}

} // namespace nwii::ui
