#pragma once
// CTRL18-UI (T8): the model behind the "Besturing" tab - presets, rebind tables, conflicts, the names a captured
// key/button gets, labels per controller family and the "last used device". Header-only, no ImGui/GL and no SDL
// calls (SDL enums only), so the unit tests (outputs/pc-tests/ctrl18ui) run it without a window.
//
// Tables (CTRL18-INPUT owns the binding model, src/input/input_bindings.h):
//   pad preset "switch-hd"            [input.gamepad_hd]  hd_pad_intents()   face buttons BY POSITION (east = Switch A)
//   pad preset "classic"/"wii-motion" [input.gamepad]     gamepad_actions()  SDL buttons (a = south on Xbox/PS)
//   keyboard preset "kbm"             [input.kbm]         kbm_intents()
//   keyboard preset "classic"         [input.keyboard]    keyboard_actions()
// A rebind is stored as an override list for one action; "Herstel standaard" removes the key (empty edit value).
#include <SDL.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../input/input_bindings.h" // path: CTRL18-CAM (lib include path)
#include "runtime/gx/glyph_sets.h"

namespace nwii::ui {
namespace glyphs = nwii::runtime::glyphs; // CTRL18-UI: device kinds of the glyph sets
}

namespace nwii::ui::controls {

using BindMap = std::map<std::string, std::vector<std::string>>; // action -> inputs (overrides only)

enum class TableId { PadHd, PadClassic, KbmHd, KbClassic };

struct TableInfo {
    TableId id;
    const char* section; // TOML table
    bool pad;
};
inline TableInfo Table(TableId t) {
    switch (t) {
    case TableId::PadHd: return {t, "input.gamepad_hd", true};
    case TableId::PadClassic: return {t, "input.gamepad", true};
    case TableId::KbmHd: return {t, "input.kbm", false};
    case TableId::KbClassic: return {t, "input.keyboard", false};
    }
    return {t, "input.gamepad", true};
}
inline TableId PadTable(const std::string& preset_gamepad) {
    return preset_gamepad == "switch-hd" ? TableId::PadHd : TableId::PadClassic;
}
inline TableId KeyTable(const std::string& preset_keyboard) {
    return preset_keyboard == "classic" ? TableId::KbClassic : TableId::KbmHd;
}

struct Row {
    std::string name, group, help;
    std::vector<std::string> defaults;
};

// Dutch labels + groups for the classic actions (the intent tables carry their own Dutch help and group).
inline void ClassicLabel(const std::string& n, std::string& label, std::string& group) {
    static const std::map<std::string, std::pair<const char*, const char*>> k = {
        {"a", {"Wii A", "Knoppen"}}, {"b", {"Wii B", "Knoppen"}}, {"1", {"Wii 1", "Knoppen"}}, {"2", {"Wii 2", "Knoppen"}},
        {"plus", {"Wii +", "Knoppen"}}, {"minus", {"Wii -", "Knoppen"}}, {"home", {"Wii Home", "Knoppen"}},
        {"up", {"Kruis omhoog", "Knoppen"}}, {"down", {"Kruis omlaag", "Knoppen"}}, {"left", {"Kruis links", "Knoppen"}},
        {"right", {"Kruis rechts", "Knoppen"}}, {"c", {"Nunchuk C", "Knoppen"}}, {"z", {"Nunchuk Z", "Knoppen"}},
        {"stick_up", {"Nunchuk-stick omhoog", "Bewegen"}}, {"stick_down", {"Nunchuk-stick omlaag", "Bewegen"}},
        {"stick_left", {"Nunchuk-stick links", "Bewegen"}}, {"stick_right", {"Nunchuk-stick rechts", "Bewegen"}},
        {"swing_up", {"Zwaardslag omhoog", "Zwaard"}}, {"swing_down", {"Zwaardslag omlaag", "Zwaard"}},
        {"swing_left", {"Zwaardslag links", "Zwaard"}}, {"swing_right", {"Zwaardslag rechts", "Zwaard"}},
        {"swing_up_left", {"Slag linksboven", "Zwaard"}}, {"swing_up_right", {"Slag rechtsboven", "Zwaard"}},
        {"swing_down_left", {"Slag linksonder", "Zwaard"}}, {"swing_down_right", {"Slag rechtsonder", "Zwaard"}},
        {"thrust", {"Steek", "Zwaard"}}, {"raise", {"Zwaard heffen (vasthouden)", "Zwaard"}},
        {"nunchuk_flick", {"Nunchuk-zwiep (schild)", "Zwaard"}}, {"spin_horizontal", {"Draaiaanval horizontaal", "Zwaard"}},
        {"spin_vertical", {"Draaiaanval verticaal", "Zwaard"}}, {"mouse_swing", {"Vasthouden + muis zwiepen = slag", "Zwaard"}},
        {"roll_left", {"Remote naar links kantelen", "Bewegen"}}, {"roll_right", {"Remote naar rechts kantelen", "Bewegen"}},
        {"aim_relative", {"Vasthouden: muis draait de remote", "Camera"}},
        {"spin_modifier", {"Met stick-zwiep: draaiaanval", "Zwaard"}}, {"nunchuk_shake", {"Nunchuk schudden", "Zwaard"}},
        {"recenter", {"Wijzer centreren", "Camera"}}, {"gyro_toggle", {"1 s vasthouden: gyro aan/uit", "Camera"}}};
    const auto it = k.find(n);
    label = it != k.end() ? it->second.first : n;
    group = it != k.end() ? it->second.second : "Knoppen";
}

inline std::vector<Row> Rows(TableId t) {
    using namespace runtime::input;
    std::vector<Row> out;
    const auto add = [&](const auto& table, bool classic) {
        for (const ActionInfo& a : table) {
            Row r;
            r.name = a.name;
            for (const char* d : a.defaults) r.defaults.push_back(d);
            if (classic) {
                ClassicLabel(r.name, r.help, r.group);
            } else {
                r.help = a.help ? a.help : r.name;
                r.group = (a.group && *a.group) ? a.group : "Overig";
            }
            out.push_back(std::move(r));
        }
    };
    switch (t) {
    case TableId::PadHd: add(hd_pad_intents(), false); break;
    case TableId::PadClassic: add(gamepad_actions(), true); break;
    case TableId::KbmHd: add(kbm_intents(), false); break;
    case TableId::KbClassic: add(keyboard_actions(), true); break;
    }
    return out;
}

// The UI groups in display order (rows of other groups follow at the end).
inline const std::vector<std::string>& GroupOrder() {
    static const std::vector<std::string> g = {"Bewegen", "Zwaard", "Voorwerpen", "Camera", "Menu's",
                                               "Vliegen/Zwemmen", "Knoppen"};
    return g;
}

inline std::vector<std::string> Effective(const Row& r, const BindMap& overrides) {
    const auto it = overrides.find(r.name);
    return it != overrides.end() ? it->second : r.defaults;
}

// ------------------------------------------------------------------------------------------- input identity
// Canonical identity of one input text ("east", "LT", "Left Shift", "back+start"): parsed by CTRL18-INPUT's parser,
// so "lt" == "lefttrigger" == "zl". Empty = not a valid input for that kind of table.
inline std::string Identity(const std::string& text, bool pad) {
    using namespace runtime::input;
    InputCombo c;
    std::string bad;
    if (!bindings_detail::parse_combo(text, pad, c, bad) || c.n <= 0) return {};
    std::vector<std::pair<int, int>> parts;
    for (int i = 0; i < c.n; ++i) parts.emplace_back(int(c.in[i].kind), c.in[i].code);
    std::sort(parts.begin(), parts.end());
    std::string id;
    for (const auto& [k, code] : parts) id += std::to_string(k) + ":" + std::to_string(code) + ";";
    return id;
}

struct Conflict {
    std::string input;                // as the player sees it (first spelling)
    std::vector<std::string> actions; // every action of this table bound to it
};
// Inputs bound to more than one action of the table where at least one of those bindings is the player's own
// (shared defaults like the touchpad = recentre + gyro toggle are by design and not reported).
inline std::vector<Conflict> Conflicts(const std::vector<Row>& rows, const BindMap& overrides, bool pad) {
    std::map<std::string, Conflict> by;
    std::map<std::string, bool> custom;
    for (const Row& r : rows) {
        const bool overridden = overrides.count(r.name) > 0;
        for (const std::string& in : Effective(r, overrides)) {
            const std::string id = Identity(in, pad);
            if (id.empty()) continue;
            Conflict& c = by[id];
            if (c.input.empty()) c.input = in;
            if (std::find(c.actions.begin(), c.actions.end(), r.name) == c.actions.end()) c.actions.push_back(r.name);
            if (overridden) custom[id] = true;
        }
    }
    std::vector<Conflict> out;
    for (auto& [id, c] : by)
        if (c.actions.size() > 1 && custom.count(id)) out.push_back(c);
    return out;
}

// ------------------------------------------------------------------------------------------- capture -> name
// The canonical text written for a captured key / mouse button (what CTRL18-INPUT's parser reads back).
inline std::string KeyName(int scancode) {
    using namespace runtime::input;
    static std::map<int, std::string> table = [] {
        std::map<int, std::string> m;
        std::vector<std::string> names = {
            "Space", "Return", "Backspace", "Tab", "Escape", "Home", "End", "Insert", "Delete", "Page Up", "Page Down",
            "Up", "Down", "Left", "Right", "Left Shift", "Right Shift", "Left Ctrl", "Right Ctrl", "Left Alt",
            "Right Alt", "Caps Lock", ",", ".", "/", ";", "'", "-", "=", "[", "]", "`", "\\", "Keypad Plus",
            "Keypad Minus", "Keypad Multiply", "Keypad Divide", "Keypad Enter", "Keypad Period"};
        for (char c = 'A'; c <= 'Z'; ++c) names.push_back(std::string(1, c));
        for (char c = '0'; c <= '9'; ++c) names.push_back(std::string(1, c));
        for (int f = 1; f <= 12; ++f) names.push_back("F" + std::to_string(f));
        for (int d = 0; d <= 9; ++d) names.push_back("Keypad " + std::to_string(d));
        for (const std::string& n : names) {
            BoundInput b;
            if (bindings_detail::key_input(n, b) && b.kind == BoundInput::Key && !m.count(b.code)) m[b.code] = n;
        }
        return m;
    }();
    const auto it = table.find(scancode);
    return it != table.end() ? it->second : std::string();
}
inline std::string MouseName(int button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return "Mouse Left";
    case SDL_BUTTON_RIGHT: return "Mouse Right";
    case SDL_BUTTON_MIDDLE: return "Mouse Middle";
    case SDL_BUTTON_X1: return "Mouse 4";
    case SDL_BUTTON_X2: return "Mouse 5";
    }
    return {};
}
// A controller button. In the switch-hd table face buttons are stored BY POSITION ("east" ...), resolved with
// CTRL18-INPUT's face_button_for for the pad that was pressed (nintendo = a Switch Pro / Joy-Con in SDL label mode).
inline std::string PadButtonName(int button, bool by_position, bool nintendo) {
    if (by_position)
        for (int pos = 0; pos < 4; ++pos)
            if (runtime::input::face_button_for(pos, nintendo ? 5 : 0) == button)
                return pos == 0 ? "south" : pos == 1 ? "east" : pos == 2 ? "west" : "north";
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: return "a";
    case SDL_CONTROLLER_BUTTON_B: return "b";
    case SDL_CONTROLLER_BUTTON_X: return "x";
    case SDL_CONTROLLER_BUTTON_Y: return "y";
    case SDL_CONTROLLER_BUTTON_BACK: return "back";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "guide";
    case SDL_CONTROLLER_BUTTON_START: return "start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "leftstick";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "rightstick";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "leftshoulder";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "rightshoulder";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "dpup";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "dpdown";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "dpleft";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "dpright";
    case SDL_CONTROLLER_BUTTON_PADDLE1: return "paddle1";
    case SDL_CONTROLLER_BUTTON_PADDLE2: return "paddle2";
    case SDL_CONTROLLER_BUTTON_PADDLE3: return "paddle3";
    case SDL_CONTROLLER_BUTTON_PADDLE4: return "paddle4";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "touchpad";
    }
    return {}; // misc1 = the port menu (reserved), unknown buttons
}
inline std::string TriggerName(int axis) {
    return axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? "lefttrigger"
         : axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? "righttrigger" : std::string();
}
// Keys the port keeps for itself (never bound): F1 = port menu, F11 = fullscreen, F12 = screenshot.
inline bool ReservedKey(int scancode) {
    return scancode == SDL_SCANCODE_F1 || scancode == SDL_SCANCODE_F11 || scancode == SDL_SCANCODE_F12;
}

// ------------------------------------------------------------------------------------------- labels
// How an input reads for the player, per controller family ("east" on a DualSense = "Cirkel").
inline std::string PadLabel(const std::string& part, glyphs::Device d) {
    using D = glyphs::Device;
    const std::string n = runtime::input::bindings_detail::normalize(part);
    const int fam = d == D::PlayStation ? 1 : d == D::Switch ? 2 : 0; // 0 Xbox/generic
    const auto pick = [&](const char* x, const char* p, const char* s) { return std::string(fam == 1 ? p : fam == 2 ? s : x); };
    // by position (switch-hd)
    if (n == "east") return pick("B", "Cirkel", "A");
    if (n == "south") return pick("A", "Kruis", "B");
    if (n == "west") return pick("X", "Vierkant", "Y");
    if (n == "north") return pick("Y", "Driehoek", "X");
    // SDL buttons (classic): Xbox labels; PlayStation shapes; Nintendo in SDL label mode = the printed letter
    if (n == "a" || n == "cross") return pick("A", "Kruis", "A");
    if (n == "b" || n == "circle") return pick("B", "Cirkel", "B");
    if (n == "x" || n == "square") return pick("X", "Vierkant", "X");
    if (n == "y" || n == "triangle") return pick("Y", "Driehoek", "Y");
    if (n == "leftshoulder" || n == "l1" || n == "lb") return pick("LB", "L1", "L");
    if (n == "rightshoulder" || n == "r1" || n == "rb") return pick("RB", "R1", "R");
    if (n == "lefttrigger" || n == "l2" || n == "lt" || n == "zl") return pick("LT", "L2", "ZL");
    if (n == "righttrigger" || n == "r2" || n == "rt" || n == "zr") return pick("RT", "R2", "ZR");
    if (n == "back" || n == "create" || n == "share" || n == "select" || n == "view") return pick("View", "Create", "-");
    if (n == "start" || n == "options" || n == "menu") return pick("Menu", "Options", "+");
    if (n == "guide" || n == "ps" || n == "home") return pick("Xbox", "PS", "Home");
    if (n == "leftstick" || n == "l3" || n == "ls") return pick("LS (indrukken)", "L3", "L-stick (indrukken)");
    if (n == "rightstick" || n == "r3" || n == "rs") return pick("RS (indrukken)", "R3", "R-stick (indrukken)");
    if (n == "dpup" || n == "dpadup" || n == "up") return "D-pad omhoog";
    if (n == "dpdown" || n == "dpaddown" || n == "down") return "D-pad omlaag";
    if (n == "dpleft" || n == "dpadleft" || n == "left") return "D-pad links";
    if (n == "dpright" || n == "dpadright" || n == "right") return "D-pad rechts";
    if (n == "touchpad") return "Touchpad";
    if (n == "misc1" || n == "mute" || n == "capture") return pick("Share", "Mute", "Capture");
    return part;
}
inline std::string KeyLabel(const std::string& part) {
    const std::string n = runtime::input::bindings_detail::normalize(part);
    if (n == "mouseleft" || n == "mouse1" || n == "lmb") return "Linkermuisknop";
    if (n == "mouseright" || n == "mouse2" || n == "rmb") return "Rechtermuisknop";
    if (n == "mousemiddle" || n == "mouse3" || n == "mmb") return "Middelste muisknop";
    if (n == "mouse4" || n == "mousex1") return "Muisknop 4";
    if (n == "mouse5" || n == "mousex2") return "Muisknop 5";
    if (n == "escape" || n == "esc") return "Esc";
    if (n == "return" || n == "enter") return "Enter";
    if (n == "space") return "Spatie";
    if (n == "up") return "Pijl omhoog";
    if (n == "down") return "Pijl omlaag";
    if (n == "left") return "Pijl links";
    if (n == "right") return "Pijl rechts";
    return part;
}
// "back+start" -> "View + Menu"
inline std::string InputLabel(const std::string& text, bool pad, glyphs::Device d) {
    std::string out, part;
    const auto flush = [&] {
        if (part.empty()) return;
        out += (out.empty() ? "" : " + ") + (pad ? PadLabel(part, d) : KeyLabel(part));
        part.clear();
    };
    for (char c : text) {
        if (c == '+' && !part.empty()) flush();
        else part += c;
    }
    flush();
    return out;
}

// ------------------------------------------------------------------------------------------- TOML
inline std::string TomlString(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}
inline std::string TomlList(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + TomlString(v[i]);
    return o + "]";
}
// TOML key for an action name (bare when possible).
inline std::string TomlKey(const std::string& k) {
    for (char c : k)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return TomlString(k);
    return k;
}

// ------------------------------------------------------------------------------------------- last used device
// Hysteresis for the "auto" glyph set: a device must be used for real (a key/click, a button, a stick pushed past
// half) to become the last used one; mouse motion alone does not switch away from a controller.
struct DeviceTracker {
    glyphs::Device last = glyphs::Device::Keyboard;
    bool known = false;
    // Returns true when the last used device changed.
    bool Note(const SDL_Event& e, const char* pad_type) {
        glyphs::Device d;
        switch (e.type) {
        case SDL_KEYDOWN:
            if (e.key.repeat) return false;
            d = glyphs::Device::Keyboard;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEWHEEL:
            d = glyphs::Device::Keyboard;
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            d = glyphs::DeviceFromType(pad_type);
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (e.caxis.value < 16384 && e.caxis.value > -16384) return false;
            d = glyphs::DeviceFromType(pad_type);
            break;
        default:
            return false;
        }
        const bool changed = !known || d != last;
        last = d;
        known = true;
        return changed;
    }
};

} // namespace nwii::ui::controls
