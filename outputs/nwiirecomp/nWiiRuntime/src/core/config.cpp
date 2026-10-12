#include "runtime/config.h"
#include "runtime/ui_config.h" // UI13
#include "runtime/hdtex_config.h" // HDTEX16
#include "runtime/interp_config.h" // INTERP17
#include "runtime/camera_stick.h" // CTRL18-CAM
#include <toml++/toml.hpp>
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <cctype>   // INPUT10
#include <sstream>  // INPUT10
#include <fstream>  // UI13

namespace nwii {
namespace runtime {

namespace {
// PKG10 (M7.1) path helpers. Config strings are UTF-8 (TOML); nand_fs reads
// nand_root as UTF-8 too (u8_to_path).
std::filesystem::path path_from_u8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
std::string u8_from_path(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
// "%NAME%" -> value of environment variable NAME. Unknown names and a lone
// '%' are kept as written.
std::string expand_env(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        const size_t a = in.find('%', i);
        const size_t b = a == std::string::npos ? a : in.find('%', a + 1);
        if (b == std::string::npos) {
            out.append(in, i, std::string::npos);
            break;
        }
        out.append(in, i, a - i);
        const std::string name = in.substr(a + 1, b - a - 1);
        bool found = false;
        if (!name.empty()) {
#ifdef _WIN32
            const std::wstring wname = path_from_u8(name).wstring();
            if (const wchar_t* v = _wgetenv(wname.c_str())) {
                out += u8_from_path(std::filesystem::path(v));
                found = true;
            }
#else
            if (const char* v = std::getenv(name.c_str())) {
                out += v;
                found = true;
            }
#endif
        }
        if (found) {
            i = b + 1;
        } else {
            out.append(in, a, b - a);  // keep "%NAME", rescan from the second '%'
            i = b;
        }
    }
    return out;
}
}  // namespace

std::string Config::resolve_path(const std::string& value) const {
    if (value.empty()) return value;
    std::filesystem::path p = path_from_u8(expand_env(value));
    if (!p.has_root_name() && !p.has_root_directory() && !config_dir.empty())
        p = path_from_u8(config_dir) / p;
    return u8_from_path(p.lexically_normal());
}

// INPUT10 (M4.3): [input] gyro/bindings keys; defined at the end of this file.
static void load_input10(const toml::table& tbl, Config& cfg);
// SPK11 (M5.4): [audio] keys; defined at the end of this file.
static void load_audio_spk11(const toml::table& tbl, Config& cfg);
// UI13: [ui] keys; defined at the end of this file.
static void load_ui13(const toml::table& tbl, Config& cfg);
// HDTEX16: [graphics] custom texture / dump keys; defined at the end of this file.
static void load_hdtex16(const toml::table& tbl, Config& cfg);
// INTERP17: [graphics] frame_interpolation keys (runtime/interp_config.h); defined at the end of this file.
static void load_interp17(const toml::table& tbl);
// CTRL18-CAM: [input] rumble + [camera] keys -> the atomics in include/runtime/camera_stick.h; end of this file.
static void load_ctrl18cam(const toml::table& tbl, const Config& cfg);
static void load_launch19(const toml::table& tbl);

void Config::load(const std::string& filepath) {
    if (!std::filesystem::exists(filepath)) {
        std::cout << "[Config] Config file " << filepath << " not found, using defaults." << std::endl;
        return;
    }
    {
        std::error_code ec;
        const auto abs = std::filesystem::absolute(std::filesystem::path(filepath), ec);
        if (!ec) config_dir = u8_from_path(abs.parent_path());
    }

    try {
        toml::table tbl = toml::parse_file(filepath);

        auto plat = tbl["system"]["platform"].value<std::string>();
        if (plat) {
            if (*plat == "GameCube" || *plat == "gamecube") {
                platform = Platform::GameCube;
                std::cout << "[Config] Platform set to GameCube." << std::endl;
            } else if (*plat == "WiiU" || *plat == "wiiu" || *plat == "Wii U") {
                platform = Platform::WiiU;
                std::cout << "[Config] Platform set to Wii U." << std::endl;
            } else {
                platform = Platform::Wii;
                std::cout << "[Config] Platform set to Wii." << std::endl;
            }
        }

        auto vsync = tbl["graphics"]["vsync"].value<bool>();
        if (vsync) enable_vsync = *vsync;

        auto back = tbl["graphics"]["backend"].value<std::string>();
        if (back) {
            if (*back == "Metal" || *back == "metal") {
                backend = Backend::Metal;
                std::cout << "[Config] Backend set to Metal." << std::endl;
            } else {
                backend = Backend::OpenGL;
                std::cout << "[Config] Backend set to OpenGL." << std::endl;
            }
        }

        auto w = tbl["graphics"]["window_width"].value<int>();
        if (w) window_width = *w;

        auto h = tbl["graphics"]["window_height"].value<int>();
        if (h) window_height = *h;

        // RES9 (M7.3): internal resolution, fullscreen, aspect, screenshots.
        if (auto is = tbl["graphics"]["internal_scale"].value<int64_t>()) {
            internal_scale = (int)std::clamp<int64_t>(*is, 0, 6);
        } else if (auto iss = tbl["graphics"]["internal_scale"].value<std::string>()) {
            internal_scale = 0; // "auto" (anything non-numeric)
        }
        if (auto fs = tbl["graphics"]["fullscreen"].value<bool>()) fullscreen = *fs;
        if (auto ar = tbl["graphics"]["aspect"].value<std::string>()) {
            if (*ar == "4:3") aspect_mode = 1;
            else if (*ar == "16:9") aspect_mode = 2;
            else if (*ar == "stretch") aspect_mode = 3;
            else aspect_mode = 0;
        }
        if (auto sd = tbl["graphics"]["screenshot_dir"].value<std::string>()) screenshot_dir = resolve_path(*sd);
        std::cout << "[Config] graphics: internal_scale="
                  << (internal_scale ? std::to_string(internal_scale) : std::string("auto"))
                  << " fullscreen=" << (fullscreen ? 1 : 0) << " vsync=" << (enable_vsync ? 1 : 0)
                  << " window=" << window_width << "x" << window_height
                  << " aspect=" << aspect_mode << std::endl;

        auto gid = tbl["game"]["id"].value<std::string>();
        if (gid) {
            game_id = *gid;
            std::cout << "[Config] Game ID override: " << game_id << std::endl;
        }

        auto nand_root_value = tbl["nand"]["root"].value<std::string>();
        if (nand_root_value) {
            nand_root = resolve_path(*nand_root_value);
            std::cout << "[Config] NAND root: " << nand_root << std::endl;
        }
        auto nand_widescreen_value = tbl["nand"]["widescreen"].value<bool>();
        if (nand_widescreen_value) nand_widescreen = *nand_widescreen_value ? 1 : 0;

        auto mode = tbl["input"]["mode"].value<int>();
        if (mode) {
            input_mode = *mode;
            static const char* mode_names[] = {
                "", "wiimote-bluetooth", "gamepad-classic",
                "gamepad-pointer-assist", "gamepad-full-tilt",
                "smartphone", "keyboard-mouse", "touch"
            };
            if (input_mode >= 1 && input_mode <= 7)
                std::cout << "[Config] Input mode " << input_mode << " ("
                          << mode_names[input_mode] << ")" << std::endl;
            else
                std::cout << "[Config] Unknown input mode " << input_mode
                          << ", falling back to 6 (keyboard-mouse)" << std::endl;
        }

        auto gyro = tbl["input"]["gyro_sensitivity"].value<double>();
        if (gyro) gyro_sensitivity = (float)*gyro;

        auto pspeed = tbl["input"]["pointer_speed"].value<double>();
        if (pspeed) pointer_speed = (float)*pspeed;

        auto pport = tbl["input"]["phone_port"].value<int>();
        if (pport) phone_port = *pport;

        load_input10(tbl, *this); // INPUT10
        load_audio_spk11(tbl, *this); // SPK11
        load_ui13(tbl, *this); // UI13
        load_hdtex16(tbl, *this); // HDTEX16
        load_interp17(tbl); // INTERP17
        load_launch19(tbl);
        load_ctrl18cam(tbl, *this); // CTRL18-CAM (after load_input10: [mouse] sensitivity_camera)

        auto eid = tbl["hle"]["ext_interrupt_dispatch"].value<int64_t>();
        if (eid && *eid != 0) {
            game_profile.ext_interrupt_dispatch = (uint32_t)*eid;
            std::cout << "[Config] ext_interrupt_dispatch = 0x"
                      << std::hex << game_profile.ext_interrupt_dispatch
                      << std::dec << std::endl;
        }

        auto slo = tbl["hle"]["sched_lock_offset"].value<int64_t>();
        if (slo && *slo != 0) {
            game_profile.sched_lock_offset = (uint32_t)*slo;
            std::cout << "[Config] sched_lock_offset = 0x"
                      << std::hex << game_profile.sched_lock_offset
                      << std::dec << std::endl;
        }

    } catch (const toml::parse_error& err) {

        std::cerr << "[Config] Failed to parse config.toml:\n" << err << std::endl;
    }
}

void Config::load_game_profile() {
    
    std::vector<std::string> search_paths = {
        "assets/games/" + game_id + ".toml",
        game_dir + "/../assets/games/" + game_id + ".toml",
    };

    std::string found_path;
    for (const auto& p : search_paths) {
        if (std::filesystem::exists(p)) {
            found_path = p;
            break;
        }
    }

    if (found_path.empty()) {
        std::cout << "[Config] No game profile for " << game_id << " (using defaults)." << std::endl;
        return;
    }

    try {
        toml::table tbl = toml::parse_file(found_path);

        auto cst = tbl["quirks"]["callback_stack_top"].value<int64_t>();
        if (cst) {
            game_profile.callback_stack_top = (uint32_t)*cst;
            std::cout << "[Config] Game profile: callback_stack_top = 0x"
                      << std::hex << game_profile.callback_stack_top << std::dec << std::endl;
        }

        if (auto mods = tbl["quirks"]["disable_module"].as_array()) {
            mods->for_each([this](const toml::value<std::string>& v) {
                game_profile.disable_modules.push_back(*v);
                std::cout << "[Config] Game profile: disable_module = " << *v << std::endl;
            });
        }

        auto rpl = tbl["game"]["rpl_path"].value<std::string>();
        if (rpl) game_profile.rpl_path = *rpl;

        auto eid = tbl["hle"]["ext_interrupt_dispatch"].value<int64_t>();
        if (eid) {
            game_profile.ext_interrupt_dispatch = (uint32_t)*eid;
            std::cout << "[Config] Game profile: ext_interrupt_dispatch = 0x"
                      << std::hex << game_profile.ext_interrupt_dispatch
                      << std::dec << std::endl;
        }

        std::cout << "[Config] Loaded game profile: " << found_path << std::endl;

    } catch (const toml::parse_error& err) {
        std::cerr << "[Config] Failed to parse game profile " << found_path
                  << ":\n" << err << std::endl;
    }
}

// INPUT10 (M4.3): [input] gamepad_motion, gyro_auto_calibrate, gyro_grip_tilt, mouse_sensitivity, aim_stick_speed,
// left/right_stick_deadzone, trigger_threshold and the [input.keyboard] / [input.gamepad] tables.
// Only types and ranges are checked here (bad values keep the default, with a message); action and
// input names are resolved and validated by src/input/input_bindings.h when the input sources start.
static void load_input10(const toml::table& tbl, Config& cfg) {
    const auto error = [&cfg](const std::string& message) {
        cfg.input_config_errors.push_back(message);
        std::cout << "[Config] " << message << std::endl;
    };
    // CTRL18-INPUT: [mouse] sensitivity_camera / sensitivity_aim / invert_y (also without an [input] table).
    if (const toml::table* mouse = tbl["mouse"].as_table()) {
        const auto mnumber = [&](const char* key, float& out) {
            const toml::node* node = mouse->get(key);
            if (!node) return;
            const auto v = node->value<double>();
            if (!v || !(*v >= 0.05 && *v <= 20.0)) {
                error(std::string("[mouse] ") + key + " must be a number from 0.05 to 20; keeping the default");
                return;
            }
            out = static_cast<float>(*v);
        };
        mnumber("sensitivity_camera", cfg.mouse_camera_sensitivity);
        mnumber("sensitivity_aim", cfg.mouse_aim_sensitivity);
        if (const toml::node* node = mouse->get("invert_y")) {
            if (auto b = node->value<bool>()) cfg.mouse_invert_y = *b;
            else error("[mouse] invert_y must be true or false; keeping the default");
        }
    }
    if (const toml::table* camera = tbl["camera"].as_table()) { // CTRL18-INPUT: the rest of [camera] is CTRL18-CAM's
        if (const toml::node* node = camera->get("flight_invert_y")) {
            if (auto b = node->value<bool>()) cfg.flight_invert_y = *b;
            else error("[camera] flight_invert_y must be true or false; keeping the default");
        }
    }
    const toml::table* input = tbl["input"].as_table();
    if (!input) return;
    if (const toml::node* node = input->get("gamepad_motion")) {
        if (auto s = node->value<std::string>()) {
            std::string v = *s;
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (v == "off" || v == "gyro") cfg.gamepad_motion = v;
            else error("[input] gamepad_motion = \"" + *s + "\" is not \"off\" or \"gyro\"; using \"off\"");
        } else if (auto b = node->value<bool>()) {
            cfg.gamepad_motion = *b ? "gyro" : "off";
        } else {
            error("[input] gamepad_motion must be \"off\" or \"gyro\"; using \"off\"");
        }
    }
    if (const toml::node* node = input->get("gyro_auto_calibrate")) {
        if (auto b = node->value<bool>()) cfg.gyro_auto_calibrate = *b;
        else error("[input] gyro_auto_calibrate must be true or false; using true");
    }
    const auto number = [&](const char* key, float& out, double lo, double hi) {
        const toml::node* node = input->get(key);
        if (!node) return;
        const auto v = node->value<double>();
        if (!v || !(*v >= lo && *v <= hi)) {
            std::ostringstream m;
            m << "[input] " << key << " must be a number from " << lo << " to " << hi << "; keeping the default";
            error(m.str());
            return;
        }
        out = static_cast<float>(*v);
    };
    number("gyro_grip_tilt", cfg.gyro_grip_tilt, -60.0, 60.0);
    number("mouse_sensitivity", cfg.mouse_sensitivity, 0.05, 20.0);
    number("aim_stick_speed", cfg.aim_stick_speed, 0.1, 2.0);
    number("left_stick_deadzone", cfg.left_stick_deadzone, 0.0, 0.9);
    number("right_stick_deadzone", cfg.right_stick_deadzone, 0.0, 0.9);
    number("trigger_threshold", cfg.trigger_threshold, 0.05, 0.95);
    const auto bindings = [&](const char* name, std::vector<std::pair<std::string, std::vector<std::string>>>& out) {
        const toml::node* node = input->get(name);
        if (!node) return;
        const toml::table* table = node->as_table();
        if (!table) {
            error(std::string("[input] ") + name + " must be a table: [input." + name + "]");
            return;
        }
        for (auto&& [key, value] : *table) {
            std::vector<std::string> list;
            bool ok = true;
            if (auto s = value.value<std::string>()) {
                list.push_back(*s);
            } else if (const toml::array* array = value.as_array()) {
                for (auto&& element : *array) {
                    if (auto e = element.value<std::string>()) list.push_back(*e);
                    else ok = false;
                }
            } else {
                ok = false;
            }
            if (!ok) {
                error(std::string("[input.") + name + "] " + std::string(key.str()) +
                      ": expected a name or a list of names, e.g. \"Space\" or [\"Keypad 8\", \"I\"]; entry ignored");
                continue;
            }
            out.emplace_back(std::string(key.str()), std::move(list));
        }
    };
    bindings("keyboard", cfg.keyboard_bindings);
    bindings("gamepad", cfg.gamepad_bindings);
    // CTRL18-INPUT: presets + options ([input]) and the rebind tables [input.gamepad_hd] / [input.kbm].
    const auto choice = [&](const char* key, std::string& out, std::initializer_list<const char*> allowed) {
        const toml::node* node = input->get(key);
        if (!node) return;
        std::string list;
        for (const char* a : allowed) list += (list.empty() ? "\"" : ", \"") + std::string(a) + "\"";
        if (auto s = node->value<std::string>()) {
            std::string v = *s;
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            for (const char* a : allowed)
                if (v == a) { out = v; return; }
        }
        error(std::string("[input] ") + key + " must be one of " + list + "; using \"" + out + "\"");
    };
    choice("preset_gamepad", cfg.preset_gamepad, {"switch-hd", "classic", "wii-motion"});
    choice("preset_keyboard", cfg.preset_keyboard, {"kbm", "classic"});
    const auto flag = [&](const toml::table* t, const char* block, const char* key, bool& out) {
        if (!t) return;
        if (const toml::node* node = t->get(key)) {
            if (auto b = node->value<bool>()) out = *b;
            else error(std::string("[") + block + "] " + key + " must be true or false; keeping the default");
        }
    };
    flag(input, "input", "gyro_aim", cfg.gyro_aim);
    flag(input, "input", "gyro_player_space", cfg.gyro_player_space);
    flag(input, "input", "a_does_not_dash", cfg.a_does_not_dash);
    flag(input, "input", "fire_on_release", cfg.fire_on_release);
    flag(input, "input", "quick_items", cfg.quick_items);
    cfg.gamepad_hd_bindings.clear();
    cfg.kbm_bindings.clear();
    bindings("gamepad_hd", cfg.gamepad_hd_bindings);
    bindings("kbm", cfg.kbm_bindings);
}

// SPK11 (M5.4): [audio] remote_speaker_volume = 0.0 .. 2.0 (default 0.8; 0 = Wii Remote speaker off).
static void load_audio_spk11(const toml::table& tbl, Config& cfg) {
    const toml::table* audio = tbl["audio"].as_table();
    if (!audio) return;
    if (const toml::node* node = audio->get("remote_speaker_volume")) {
        std::optional<double> v = node->value<double>();  // integers convert too
        if (v && *v >= 0.0 && *v <= 2.0) {
            cfg.remote_speaker_volume = static_cast<float>(*v);
            std::cout << "[Config] [audio] remote_speaker_volume = " << cfg.remote_speaker_volume << std::endl;
        } else {
            std::cerr << "[Config] [audio] remote_speaker_volume must be a number from 0 to 2; keeping "
                      << cfg.remote_speaker_volume << std::endl;
        }
    }
}


// UI13: [ui] launcher / overlay / toasts / game_font (booleans), background, settings_file,
// present_pacing (strings) and audio_latency_ms. Bad values keep the default, with a message.
UiConfig& ui_config() {
    static UiConfig c;
    return c;
}

static void load_ui13(const toml::table& tbl, Config& cfg) {
    if (auto f = tbl["camera"]["flight_invert_y"].value<bool>()) ui_config().flight_invert_y = *f; // CTRL18-UI
    const toml::table* ui = tbl["ui"].as_table();
    if (!ui) return;
    UiConfig& u = ui_config();
    const auto flag = [&](const char* key, bool& out) {
        if (const toml::node* node = ui->get(key)) {
            if (auto b = node->value<bool>()) out = *b;
            else std::cerr << "[Config] [ui] " << key << " must be true or false; keeping " << out << std::endl;
        }
    };
    flag("launcher", u.launcher);
    flag("overlay", u.overlay);
    flag("toasts", u.toasts);
    flag("game_font", u.game_font);
    flag("gyro_hint_shown", u.gyro_hint_shown); // CTRL18-UI
    if (auto s = (*ui)["background"].value<std::string>()) u.background = *s;
    flag("background_video", u.background_video); // THPVID19
    flag("background_music", u.background_music); // THPVID19
    if (const toml::node* node = ui->get("background_music_volume")) { // THPVID19: 0..100, clamped
        if (const auto v = node->value<int64_t>()) u.background_music_volume = int(std::clamp<int64_t>(*v, 0, 100));
        else std::cerr << "[Config] [ui] background_music_volume must be a whole number 0..100; keeping "
                       << u.background_music_volume << std::endl;
    }
    if (const toml::node* node = ui->get("background_video_file")) { // THPVID19: "file.thp[:N]", "" = the still's film
        if (auto s = node->value<std::string>()) u.background_video_file = *s;
        else std::cerr << "[Config] [ui] background_video_file must be a string" << std::endl;
    }
    if (auto s = (*ui)["settings_file"].value<std::string>()) u.settings_file = cfg.resolve_path(*s);
    if (auto s = (*ui)["present_pacing"].value<std::string>()) {
        if (*s == "auto" || *s == "dwm" || *s == "vsync" || *s == "none") u.present_pacing = *s;
        else std::cerr << "[Config] [ui] present_pacing must be \"auto\", \"dwm\", \"vsync\" or \"none\"" << std::endl;
    }
    if (const toml::node* node = ui->get("audio_latency_ms")) {
        const auto v = node->value<int64_t>();
        if (v && (*v == 0 || (*v >= 20 && *v <= 1000))) u.audio_latency_ms = int(*v);
        else std::cerr << "[Config] [ui] audio_latency_ms must be 0 or 20..1000" << std::endl;
    }
}

// UI13: line-based TOML editing for the settings write-back (see runtime/ui_config.h).
namespace {
std::string ui13_trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// Scan of a TOML value text: where a trailing comment starts, the [ ] nesting it leaves open and
// whether it leaves a multi-line string open.
struct Ui13Scan {
    size_t comment = std::string::npos;
    int depth = 0;
    bool open_ml = false;
};
Ui13Scan ui13_scan(const std::string& v, bool in_ml_string, std::string* ml_delim) {
    Ui13Scan r;
    size_t i = 0;
    if (in_ml_string) {
        const size_t e = v.find(*ml_delim);
        if (e == std::string::npos) {
            r.open_ml = true;
            return r;
        }
        i = e + 3;
    }
    while (i < v.size()) {
        const char c = v[i];
        if (c == '#') {
            r.comment = i;
            break;
        }
        if ((c == '"' || c == '\'') && v.compare(i, 3, std::string(3, c)) == 0) {
            const std::string d(3, c);
            const size_t e = v.find(d, i + 3);
            if (e == std::string::npos) {
                r.open_ml = true;
                *ml_delim = d;
                return r;
            }
            i = e + 3;
            continue;
        }
        if (c == '"') {
            ++i;
            while (i < v.size() && v[i] != '"') i += v[i] == '\\' ? 2 : 1;
            ++i;
            continue;
        }
        if (c == '\'') {
            const size_t e = v.find('\'', i + 1);
            i = e == std::string::npos ? v.size() : e + 1;
            continue;
        }
        if (c == '[') ++r.depth;
        if (c == ']') --r.depth;
        ++i;
    }
    return r;
}
struct Ui13Line {
    std::string section;       // table the line belongs to
    std::string key;           // non-empty on a "key = value" line
    bool header = false;       // a [table] line
    bool continuation = false; // inside a multi-line value of the previous key
};
std::vector<Ui13Line> ui13_parse(const std::vector<std::string>& lines) {
    std::vector<Ui13Line> out(lines.size());
    std::string section, ml_delim;
    int depth = 0;
    bool in_ml = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        Ui13Line& l = out[i];
        l.section = section;
        if (in_ml || depth > 0) {
            l.continuation = true;
            const Ui13Scan s = ui13_scan(lines[i], in_ml, &ml_delim);
            in_ml = s.open_ml;
            if (!in_ml) depth = std::max(0, depth + s.depth);
            continue;
        }
        const std::string t = ui13_trim(lines[i]);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[') {
            const bool arr = t.size() > 1 && t[1] == '[';
            const size_t start = arr ? 2 : 1;
            const size_t e = t.find(arr ? "]]" : "]", start);
            std::string name;
            for (char c : t.substr(start, e == std::string::npos ? std::string::npos : e - start))
                if (c != ' ' && c != '\t') name.push_back(c);
            section = arr ? "[[" + name + "]]" : name; // an array of tables never matches an edit
            l.section = section;
            l.header = true;
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = ui13_trim(t.substr(0, eq));
        if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'') && key.back() == key.front())
            key = key.substr(1, key.size() - 2);
        l.key = key;
        const Ui13Scan s = ui13_scan(ui13_trim(t.substr(eq + 1)), false, &ml_delim);
        in_ml = s.open_ml;
        depth = in_ml ? 0 : std::max(0, s.depth);
    }
    return out;
}
} // namespace

std::string apply_setting_edits(const std::string& text, const std::vector<SettingEdit>& edits) {
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    for (size_t a = 0; a < text.size();) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string l = text.substr(a, b - a);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        a = b + 1;
    }
    for (const SettingEdit& ed : edits) {
        const std::vector<Ui13Line> info = ui13_parse(lines);
        const std::string new_line = ed.key + " = " + ed.value_toml;
        long header = -1, last_in_section = -1, key_line = -1;
        for (size_t i = 0; i < info.size(); ++i) {
            if (info[i].section != ed.section) continue;
            if (info[i].header) {
                if (header < 0) header = long(i);
                last_in_section = long(i);
                continue;
            }
            if (header < 0) continue;
            if (!info[i].key.empty() || info[i].continuation) last_in_section = long(i);
            if (info[i].key == ed.key && key_line < 0) key_line = long(i);
        }
        if (ed.value_toml.empty()) { // CTRL18-UI: an empty value removes the key (back to the built-in default)
            if (key_line >= 0) {
                size_t end = size_t(key_line) + 1;
                while (end < info.size() && info[end].continuation) ++end;
                lines.erase(lines.begin() + std::ptrdiff_t(key_line), lines.begin() + std::ptrdiff_t(end));
            }
            continue;
        }
        if (key_line >= 0) {
            const std::string old = lines[size_t(key_line)];
            const size_t indent = old.find_first_not_of(" \t");
            std::string value = old.substr(old.find('=') + 1);
            const size_t lead = value.find_first_not_of(" \t");
            value = lead == std::string::npos ? std::string() : value.substr(lead);
            std::string ml_delim;
            const Ui13Scan s = ui13_scan(value, false, &ml_delim);
            std::string trailing;
            if (s.comment != std::string::npos && !s.open_ml && s.depth == 0) trailing = " " + value.substr(s.comment);
            lines[size_t(key_line)] = old.substr(0, indent == std::string::npos ? 0 : indent) + new_line + trailing;
            // Drop the continuation lines of an old multi-line value.
            size_t end = size_t(key_line) + 1;
            while (end < info.size() && info[end].continuation) ++end;
            lines.erase(lines.begin() + std::ptrdiff_t(key_line + 1), lines.begin() + std::ptrdiff_t(end));
        } else if (header >= 0) {
            lines.insert(lines.begin() + std::ptrdiff_t(last_in_section + 1), new_line);
        } else {
            while (!lines.empty() && ui13_trim(lines.back()).empty()) lines.pop_back();
            if (!lines.empty()) lines.push_back("");
            lines.push_back("[" + ed.section + "]");
            lines.push_back(new_line);
        }
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || !lines[i].empty()) out += eol;
    }
    return out;
}

// CTRL18-UI: parse check of a settings file (the launcher warns instead of silently ignoring a broken file).
std::string toml_file_problem(const std::string& path_utf8, int* line) {
    const std::filesystem::path path = path_from_u8(path_utf8);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return {};
    try {
        (void)toml::parse_file(path_utf8); // same call as Config::load
    } catch (const toml::parse_error& e) {
        if (line) *line = int(e.source().begin.line);
        std::ostringstream m;
        m << "line " << e.source().begin.line << ": " << e.description();
        return m.str();
    } catch (...) {
        return "unreadable";
    }
    return {};
}

bool write_user_settings(const std::string& path_utf8, const std::vector<SettingEdit>& edits,
                         std::string* error) {
    const std::filesystem::path path = path_from_u8(path_utf8);
    std::string text;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (error) *error = "cannot read " + path_utf8;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    } else {
        text = "# Skyward Sword PC - instellingen uit het menu (launcher / Esc-menu).\n"
               "# Deze waarden gaan boven het profiel. Verwijder een regel voor de standaardwaarde.\n";
    }
    const std::string result = apply_setting_edits(text, edits);
    try {
        (void)toml::parse(result);
    } catch (const toml::parse_error& e) {
        if (error) {
            std::ostringstream m;
            m << "result is not valid TOML: " << e.description();
            *error = m.str();
        }
        return false;
    }
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(result.data(), std::streamsize(result.size()))) {
            if (error) *error = "cannot write " + u8_from_path(tmp);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (error) *error = "cannot replace " + path_utf8 + ": " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} 
} 

// HDTEX16: [graphics] hd_textures / hd_textures_preload / texture_dump (booleans), texture_pack_dir
// (path, resolve_path). Bad values keep the default, with a message (runtime/hdtex_config.h).
namespace nwii::runtime {
HdTexConfig& hdtex_config() {
    static HdTexConfig c;
    return c;
}
static void load_hdtex16(const toml::table& tbl, Config& cfg) {
    const toml::table* g = tbl["graphics"].as_table();
    if (!g) return;
    HdTexConfig& h = hdtex_config();
    const auto flag = [&](const char* key, auto&& set) {
        if (const toml::node* node = g->get(key)) {
            if (auto b = node->value<bool>()) set(*b);
            else std::cerr << "[Config] [graphics] " << key << " must be true or false" << std::endl;
        }
    };
    flag("hd_textures", [&](bool b) { h.hd_textures = b ? 1 : 0; });
    flag("hd_textures_preload", [&](bool b) { h.preload = b; });
    flag("texture_dump", [&](bool b) { h.dump = b; });
    if (auto s = (*g)["texture_pack_dir"].value<std::string>()) h.pack_dir = s->empty() ? std::string() : cfg.resolve_path(*s);
    if (const toml::node* node = g->get("glyph_set")) { // CTRL18-UI: controller glyph set (runtime/gx/glyph_sets.h)
        if (auto s = node->value<std::string>()) h.glyph_set = s->empty() ? std::string("auto") : *s;
        else std::cerr << "[Config] [graphics] glyph_set must be a string (\"auto\", \"ps\", \"xbox\", \"switch\", \"kbm\", \"wii\")" << std::endl;
    }
}
} // namespace nwii::runtime

// INTERP17: [graphics] frame_interpolation / frame_interpolation_2d (booleans), frame_interpolation_mode
// ("interp" | "dup"). Bad values keep the default, with a message (runtime/interp_config.h).
namespace nwii::runtime {
static void load_interp17(const toml::table& tbl) {
    const toml::table* g = tbl["graphics"].as_table();
    if (!g) return;
    InterpConfig& c = interp_config();
    const auto flag = [&](const char* key, std::atomic<int>& dst) {
        if (const toml::node* node = g->get(key)) {
            if (auto b = node->value<bool>()) dst.store(*b ? 1 : 0);
            else std::cerr << "[Config] [graphics] " << key << " must be true or false" << std::endl;
        }
    };
    flag("frame_interpolation", c.enabled);
    flag("frame_interpolation_2d", c.interp_2d);
    if (const toml::node* node = g->get("frame_interpolation_mode")) {
        const auto m = node->value<std::string>();
        if (m && (*m == "interp" || *m == "dup")) c.dup_mode.store(*m == "dup" ? 1 : 0);
        else std::cerr << "[Config] [graphics] frame_interpolation_mode must be \"interp\" or \"dup\"" << std::endl;
    }
}
// CTRL18-CAM (work/claude-notes/CTRL18-CAM.md): [input] rumble = 0..100 (integer percent; a float <= 1.0 is read as
// a fraction), [camera] free_camera / speed (1..5) / invert_x / invert_y / auto_recentre_s (seconds, 0 = off). Stored
// into the runtime atomics (include/runtime/camera_stick.h); keys that are absent keep the current value (defaults there).
static void load_ctrl18cam(const toml::table& tbl, const Config& cfg) {
    namespace in = nwii::runtime::input;
    in::g_free_camera.mouse_sensitivity.store(cfg.mouse_camera_sensitivity);
    if (const toml::table* input = tbl["input"].as_table()) {
        if (const toml::node* node = input->get("rumble")) {
            if (auto i = node->value_exact<int64_t>())
                in::g_rumble_strength.store(int(std::clamp<int64_t>(*i, 0, 100)));
            else if (auto d = node->value_exact<double>())
                in::g_rumble_strength.store(int(std::lround(std::clamp(*d <= 1.0 ? *d * 100.0 : *d, 0.0, 100.0))));
            else if (auto b = node->value_exact<bool>())
                in::g_rumble_strength.store(*b ? 100 : 0);
            else
                std::cerr << "[Config] [input] rumble must be a number 0..100" << std::endl;
        }
    }
    const toml::table* cam = tbl["camera"].as_table();
    if (!cam) return;
    const auto flag = [&](const char* key, std::atomic<bool>& dst) {
        if (const toml::node* node = cam->get(key)) {
            if (auto b = node->value<bool>()) dst.store(*b);
            else std::cerr << "[Config] [camera] " << key << " must be true or false" << std::endl;
        }
    };
    flag("free_camera", in::g_free_camera.enabled);
    flag("invert_x", in::g_free_camera.invert_x);
    flag("invert_y", in::g_free_camera.invert_y);
    if (const toml::node* node = cam->get("speed")) {
        auto v = node->value<int64_t>();
        if (v && *v >= 1 && *v <= 5) in::g_free_camera.speed.store(int(*v));
        else std::cerr << "[Config] [camera] speed must be 1..5" << std::endl;
    }
    if (const toml::node* node = cam->get("auto_recentre_s")) {
        auto v = node->value<int64_t>();
        if (v && *v >= 0 && *v <= 600) in::g_free_camera.auto_recentre_s.store(int(*v));
        else std::cerr << "[Config] [camera] auto_recentre_s must be 0..600 (seconds, 0 = off)" << std::endl;
    }
}

} // namespace nwii::runtime

// LAUNCH19: the keys of the SkyBreak settings catalog that are new (src/ui/ui_settings_catalog.h). Bad values keep the
// default, with a message. Several only become environment switches for their (env-reading) readers, see
// ui_settings.cpp ApplyEnvFromConfig.
namespace nwii::runtime {
static void load_launch19(const toml::table& tbl) {
    UiConfig& u = ui_config();
    const auto str = [&](const char* sec, const char* key, std::string& out, std::initializer_list<const char*> allowed) {
        const toml::node* node = tbl[sec][key].node();
        if (!node) return false;
        const auto s = node->value<std::string>();
        bool ok = s.has_value() && allowed.size() == 0;
        for (const char* a : allowed) ok = ok || (s && *s == a);
        if (ok) out = *s;
        else std::cerr << "[Config] [" << sec << "] " << key << " has an unknown value; keeping \"" << out << "\"" << std::endl;
        return ok;
    };
    const auto flag = [&](const char* sec, const char* key, bool& out) {
        if (const toml::node* node = tbl[sec][key].node()) {
            if (auto b = node->value<bool>()) out = *b;
            else std::cerr << "[Config] [" << sec << "] " << key << " must be true or false; keeping " << out << std::endl;
        }
    };
    const auto num = [&](const char* sec, const char* key, int& out, int lo, int hi) {
        if (const toml::node* node = tbl[sec][key].node()) {
            const auto v = node->value<int64_t>();
            if (v && *v >= lo && *v <= hi) out = int(*v);
            else std::cerr << "[Config] [" << sec << "] " << key << " must be " << lo << ".." << hi << "; keeping " << out << std::endl;
        }
    };
    str("ui", "language", u.language, {"nl", "en"});
    num("ui", "scale", u.scale, 0, 300);
    if (u.scale > 0 && u.scale < 50) u.scale = 50;
    str("ui", "accent", u.accent, {"gold", "red", "sky"});
    str("ui", "perf_overlay", u.perf_overlay, {"off", "fps", "frametime", "full"});
    flag("ui", "remember_window", u.remember_window);
    str("ui", "confirm_button", u.confirm_button, {"east", "south"});
    num("graphics", "window_x", u.window_x, -32768, 32767);
    num("graphics", "window_y", u.window_y, -32768, 32767);
    num("audio", "master_volume", u.master_volume, 0, 100);
    flag("audio", "mute_when_unfocused", u.mute_when_unfocused);
    str("audio", "device", u.audio_device, {});
    if (str("nand", "language", u.game_language, {"en", "fr", "es"})) u.game_language_set = true;
    num("nand", "backups", u.backups, 0, 100);
    flag("game", "pause_when_unfocused", u.pause_when_unfocused);
    str("game", "clock", u.clock, {"host", "fixed"});
    flag("diag", "crash_report", u.crash_report);
    flag("diag", "freeze_watchdog", u.freeze_watchdog);
    flag("input", "directinput", u.directinput);
    flag("graphics", "color_screenlinear", u.color_screenlinear);
    flag("graphics", "copy_filter", u.copy_filter);
    flag("discord", "enabled", u.discord_presence);
    flag("discord", "show_location", u.discord_show_location);
    str("discord", "large_image", u.discord_large_image, {});
}
} // namespace nwii::runtime
