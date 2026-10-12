#pragma once
// INPUT10 (M4.3): configurable Wii Remote / Nunchuk bindings for the live keyboard+mouse and gamepad
// sources. Defaults are exactly the fixed bindings the sources had before INPUT10 (CONTROLS.md,
// POINTER.md); the [input.keyboard] / [input.gamepad] tables of the config replace an action's inputs.
//
//   [input.keyboard]                       [input.gamepad]
//   swing_up = ["Keypad 8", "I"]           thrust = "r1"
//   a = "Mouse Left"                       gyro_toggle = ["touchpad", "create+options"]
//   "1" = "1"                              plus = "options"
//
// A value is one input name or a list of alternatives; "x+y" means x and y held together; an empty
// list unbinds the action. Unknown actions/names are reported once ("[Input] config ...") and ignored
// (the action keeps its default when none of its new inputs is valid). Name lookup is table-based
// (no SDL call), so the header only needs SDL's enums. Header-only: no build-list change.
#include <SDL.h>
#include "runtime/config.h"
#include "input_intents.h" // CTRL18-INPUT
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace nwii::runtime::input {

enum KeyAction : int {
    kKeyA, kKeyB, kKeyOne, kKeyTwo, kKeyPlus, kKeyMinus, kKeyHome, kKeyUp, kKeyDown, kKeyLeft, kKeyRight,
    kKeyC, kKeyZ, kKeyStickUp, kKeyStickDown, kKeyStickLeft, kKeyStickRight,
    kKeySwingUp, kKeySwingDown, kKeySwingLeft, kKeySwingRight,
    kKeySwingUpLeft, kKeySwingUpRight, kKeySwingDownLeft, kKeySwingDownRight,
    kKeyThrust, kKeyRaise, kKeyNunchukFlick, kKeySpinHorizontal, kKeySpinVertical, kKeyMouseSwing,
    kKeyRollLeft, kKeyRollRight, kKeyAimRelative, kKeyActionCount
};
enum PadAction : int {
    kPadA, kPadB, kPadOne, kPadTwo, kPadPlus, kPadMinus, kPadHome, kPadUp, kPadDown, kPadLeft, kPadRight,
    kPadC, kPadZ, kPadThrust, kPadNunchukFlick, kPadSpinModifier, kPadNunchukShake, kPadRecenter,
    kPadRollLeft, kPadRollRight, kPadGyroToggle, kPadActionCount
};
// CTRL18-INPUT: the intent enums (HdAction, KbmAction) and presets live in input_intents.h (SDL-free).

struct BoundInput {
    enum Kind : uint8_t { Key, Mouse, Button, Axis, Face };
    Kind kind = Key;
    // SDL_Scancode, SDL_BUTTON_* index, SDL_GameControllerButton, SDL_GameControllerAxis, or (Face, CTRL18-INPUT) the
    // face button BY POSITION: 0 south, 1 east, 2 west, 3 north (resolved per controller type, face_button_for).
    int code = 0;
};
struct InputCombo {
    std::array<BoundInput, 4> in{};
    int n = 0;
};
using InputBinding = std::vector<InputCombo>; // alternatives

struct ActionInfo {
    const char* name;
    std::vector<const char*> defaults;
    const char* help;
    const char* group = ""; // CTRL18-INPUT: UI group (intent tables only)
};

// Config names and defaults, in enum order.
inline const std::array<ActionInfo, kKeyActionCount>& keyboard_actions() {
    static const std::array<ActionInfo, kKeyActionCount> table = {{
        {"a", {"Mouse Left"}, "Wii A"}, {"b", {"Mouse Right"}, "Wii B"}, {"1", {"1", "Escape"}, "Wii 1"}, {"2", {"2"}, "Wii 2"},
        {"plus", {"Return"}, "Wii +"}, {"minus", {"Backspace"}, "Wii -"}, {"home", {"Home"}, "Wii Home"},
        {"up", {"Up"}, "D-pad up"}, {"down", {"Down"}, "D-pad down"}, {"left", {"Left"}, "D-pad left"},
        {"right", {"Right"}, "D-pad right"}, {"c", {"C"}, "Nunchuk C"}, {"z", {"Z"}, "Nunchuk Z"},
        {"stick_up", {"W"}, "Nunchuk stick up"}, {"stick_down", {"S"}, "Nunchuk stick down"},
        {"stick_left", {"A"}, "Nunchuk stick left"}, {"stick_right", {"D"}, "Nunchuk stick right"},
        {"swing_up", {"Keypad 8", "I"}, "sword swing up"}, {"swing_down", {"Keypad 2", ","}, "sword swing down"},
        {"swing_left", {"Keypad 4", "J"}, "sword swing left"}, {"swing_right", {"Keypad 6", "L"}, "sword swing right"},
        {"swing_up_left", {"Keypad 7", "U"}, "swing up-left"}, {"swing_up_right", {"Keypad 9", "O"}, "swing up-right"},
        {"swing_down_left", {"Keypad 1", "M"}, "swing down-left"}, {"swing_down_right", {"Keypad 3", "."}, "swing down-right"},
        {"thrust", {"Keypad 5", "K"}, "sword thrust"}, {"raise", {"Keypad 0", "X"}, "raise the sword (hold)"},
        {"nunchuk_flick", {"F"}, "Nunchuk flick (shield)"}, {"spin_horizontal", {"R"}, "spin attack, horizontal"},
        {"spin_vertical", {"T"}, "spin attack, vertical"}, {"mouse_swing", {"Mouse Middle"}, "hold + flick the mouse = swing"},
        {"roll_left", {"Q"}, "roll the remote left"}, {"roll_right", {"E"}, "roll the remote right"},
        {"aim_relative", {"Left Shift", "Right Shift"}, "hold: mouse turns the remote (relative)"},
    }};
    return table;
}
inline const std::array<ActionInfo, kPadActionCount>& gamepad_actions() {
    static const std::array<ActionInfo, kPadActionCount> table = {{
        {"a", {"a"}, "Wii A"}, {"b", {"b"}, "Wii B"}, {"1", {"x"}, "Wii 1"}, {"2", {"y"}, "Wii 2"},
        {"plus", {"start"}, "Wii +"}, {"minus", {"back"}, "Wii -"}, {"home", {"guide"}, "Wii Home"},
        {"up", {"dpup"}, "D-pad up"}, {"down", {"dpdown"}, "D-pad down"}, {"left", {"dpleft"}, "D-pad left"},
        {"right", {"dpright"}, "D-pad right"}, {"c", {"leftshoulder"}, "Nunchuk C"}, {"z", {"lefttrigger"}, "Nunchuk Z"},
        {"thrust", {"rightshoulder"}, "sword thrust (stick mode)"},
        {"nunchuk_flick", {"righttrigger"}, "Nunchuk flick (shield)"},
        {"spin_modifier", {"righttrigger"}, "held during a right-stick flick = spin attack (stick mode)"},
        {"nunchuk_shake", {"rightshoulder"}, "hold = Nunchuk shake, with a real swing = spin attack (gyro mode)"},
        {"recenter", {"touchpad", "leftstick+rightstick"}, "point the remote at the screen centre (stick mode: held; gyro mode: tap = this grip aims at the centre)"},
        {"roll_left", {"leftstick"}, "roll the remote left (stick mode)"},
        {"roll_right", {"rightstick"}, "roll the remote right (stick mode)"},
        {"gyro_toggle", {"touchpad", "back+start"}, "hold 1 s: gyro motion on/off"},
    }};
    return table;
}
// CTRL18-INPUT: the "switch-hd" pad preset (Skyward Sword HD button-only layout; work/ctrl18/sshd-controls-research.md).
// Face buttons by POSITION like a Switch (user decision 3): east = Switch A, south = Switch B, north = X, west = Y.
// Sticks are fixed: left = move, right = sword / aim / wheel / menu cursor / camera (with "camera" held).
inline const std::array<ActionInfo, kHdActionCount>& hd_pad_intents() {
    static const std::array<ActionInfo, kHdActionCount> table = {{
        {"action", {"east"}, "A: actie (praten, openen, pakken, zwemmen; Loftwing: klappen)", "Bewegen"},
        {"dash", {"south"}, "B: sprinten (vasthouden) / opbergen / loslaten / terug / remmen", "Bewegen"},
        {"harp", {"north"}, "X: harp; op de Loftwing: spiraalaanval", "Voorwerpen"},
        {"recentre", {"west", "touchpad"}, "Y: camera achter Link (gyro aan: gyro centreren)", "Camera"},
        {"shield", {"leftstick"}, "L3: schild / rollen tijdens sprinten / draaien bij zwemmen", "Zwaard"},
        {"thrust", {"rightstick"}, "R3: steken", "Zwaard"},
        {"lock_on", {"lefttrigger"}, "ZL: richten op een vijand (vasthouden); tikken = camera achter Link", "Camera"},
        {"camera", {"leftshoulder"}, "L vasthouden + rechterstick: vrije camera", "Camera"},
        {"item", {"righttrigger"}, "ZR: voorwerp; vasthouden + rechterstick = voorwerpenwiel; boog: vasthouden = spannen", "Voorwerpen"},
        {"pouch", {"rightshoulder"}, "R: buidel; vasthouden + rechterstick = buidelwiel", "Voorwerpen"},
        {"map", {"back"}, "-: kaart (bij een tussenfilmpje: overslaan)", "Menu's"},
        {"gear", {"start"}, "+: uitrusting / pauze", "Menu's"},
        {"first_person", {"dpup"}, "D-pad omhoog: eerste persoon; vasthouden = wichelroede kiezen", "Camera"},
        {"fi", {"dpdown"}, "D-pad omlaag: Fi / Loftwing roepen", "Menu's"},
        {"help", {"dpright"}, "D-pad rechts: hulp", "Menu's"},
        {"dpad_left", {"dpleft"}, "D-pad links", "Menu's"},
        {"home", {"guide"}, "Home-menu (Wii)", "Menu's"},
        {"gyro_toggle", {"touchpad", "back+start"}, "1 s vasthouden: richten met gyro aan/uit", "Camera"},
    }};
    return table;
}
// CTRL18-INPUT: the "kbm" keyboard+mouse preset (design section 4). Mouse movement is fixed: camera in play, aim with
// an item out / first person, direction in a wheel, the game's pointer in menus. Esc = the game's gear/pause menu.
inline const std::array<ActionInfo, kKbmActionCount>& kbm_intents() {
    static const std::array<ActionInfo, kKbmActionCount> table = {{
        {"move_up", {"W"}, "vooruit (Loftwing: stijgen)", "Bewegen"},
        {"move_down", {"S"}, "achteruit (Loftwing: duiken)", "Bewegen"},
        {"move_left", {"A"}, "links", "Bewegen"},
        {"move_right", {"D"}, "rechts", "Bewegen"},
        {"walk", {"Left Alt"}, "langzaam lopen (vasthouden)", "Bewegen"},
        {"dash", {"Left Shift"}, "sprinten (vasthouden) / opbergen / loslaten / terug / remmen", "Bewegen"},
        {"action", {"E"}, "actie (praten, openen, pakken)", "Bewegen"},
        {"dodge", {"Space"}, "ontwijken / rollen / springen; Loftwing: klappen", "Bewegen"},
        {"slash", {"Mouse Left"}, "zwaard: klik = slag, vasthouden + slepen = gerichte slag; voorwerp: gebruiken", "Zwaard"},
        {"thrust", {"Mouse Middle", "F"}, "steken", "Zwaard"},
        {"shield", {"Mouse Right"}, "schild", "Zwaard"},
        {"raise", {"Mouse 4"}, "zwaard omhoog (hemelslag laden, vasthouden)", "Zwaard"},
        {"spin", {"X"}, "draaiaanval", "Zwaard"},
        {"lock_on", {"Q"}, "richten op een vijand (vasthouden); tikken = camera achter Link", "Camera"},
        {"item", {"R"}, "voorwerp; vasthouden + muis = voorwerpenwiel", "Voorwerpen"},
        {"pouch", {"Tab"}, "buidel; vasthouden + muis = buidelwiel", "Voorwerpen"},
        {"harp", {"V"}, "harp", "Voorwerpen"},
        {"item_1", {"1"}, "voorwerp 1 (wiel boven)", "Voorwerpen"},
        {"item_2", {"2"}, "voorwerp 2", "Voorwerpen"},
        {"item_3", {"3"}, "voorwerp 3", "Voorwerpen"},
        {"item_4", {"4"}, "voorwerp 4", "Voorwerpen"},
        {"item_5", {"5"}, "voorwerp 5", "Voorwerpen"},
        {"item_6", {"6"}, "voorwerp 6", "Voorwerpen"},
        {"item_7", {"7"}, "voorwerp 7", "Voorwerpen"},
        {"item_8", {"8"}, "voorwerp 8", "Voorwerpen"},
        {"first_person", {"C"}, "eerste persoon; vasthouden = wichelroede kiezen", "Camera"},
        {"map", {"M"}, "kaart", "Menu's"},
        {"gear", {"I", "Escape"}, "uitrusting / pauze", "Menu's"},
        {"fi", {"T"}, "Fi / Loftwing roepen", "Menu's"},
        {"help", {"H"}, "hulp", "Menu's"},
        {"skip", {"Backspace"}, "tussenfilmpje overslaan", "Menu's"},
        {"dpad_up", {"Up"}, "D-pad omhoog", "Menu's"},
        {"dpad_down", {"Down"}, "D-pad omlaag", "Menu's"},
        {"dpad_left", {"Left"}, "D-pad links", "Menu's"},
        {"dpad_right", {"Right"}, "D-pad rechts", "Menu's"},
        {"home", {"Home"}, "Home-menu (Wii)", "Menu's"},
    }};
    return table;
}
inline const char* const kKeyboardNameExamples =
    "\"Space\", \"K\", \"Keypad 8\", \"Left Shift\", \"F5\", \",\", \"Mouse Left\", \"Mouse Middle\"";
inline const char* const kGamepadNameExamples =
    "\"a\" (cross), \"b\" (circle), \"x\" (square), \"y\" (triangle), \"leftshoulder\" (l1), \"righttrigger\" (r2), "
    "\"back\" (create), \"start\" (options), \"touchpad\", \"dpup\"";

namespace bindings_detail {
inline std::string normalize(const std::string& s) {
    std::string out;
    for (unsigned char c : s)
        if (c != ' ' && c != '_' && c != '-') out.push_back(char(std::tolower(c)));
    return out;
}
inline bool key_input(const std::string& raw, BoundInput& out) {
    static const std::pair<const char*, int> single[] = {
        {",", SDL_SCANCODE_COMMA}, {".", SDL_SCANCODE_PERIOD}, {"/", SDL_SCANCODE_SLASH},
        {";", SDL_SCANCODE_SEMICOLON}, {"'", SDL_SCANCODE_APOSTROPHE}, {"-", SDL_SCANCODE_MINUS},
        {"=", SDL_SCANCODE_EQUALS}, {"[", SDL_SCANCODE_LEFTBRACKET}, {"]", SDL_SCANCODE_RIGHTBRACKET},
        {"`", SDL_SCANCODE_GRAVE}, {"\\", SDL_SCANCODE_BACKSLASH},
    };
    std::string trimmed = raw;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
    if (trimmed.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(trimmed[0])));
        if (c >= 'a' && c <= 'z') { out = {BoundInput::Key, SDL_SCANCODE_A + (c - 'a')}; return true; }
        if (c >= '1' && c <= '9') { out = {BoundInput::Key, SDL_SCANCODE_1 + (c - '1')}; return true; }
        if (c == '0') { out = {BoundInput::Key, SDL_SCANCODE_0}; return true; }
        for (const auto& [n, code] : single)
            if (trimmed == n) { out = {BoundInput::Key, code}; return true; }
        return false;
    }
    const std::string n = normalize(trimmed);
    static const std::pair<const char*, int> named[] = {
        {"space", SDL_SCANCODE_SPACE}, {"return", SDL_SCANCODE_RETURN}, {"enter", SDL_SCANCODE_RETURN},
        {"backspace", SDL_SCANCODE_BACKSPACE}, {"tab", SDL_SCANCODE_TAB}, {"escape", SDL_SCANCODE_ESCAPE},
        {"esc", SDL_SCANCODE_ESCAPE}, {"home", SDL_SCANCODE_HOME}, {"end", SDL_SCANCODE_END},
        {"insert", SDL_SCANCODE_INSERT}, {"delete", SDL_SCANCODE_DELETE}, {"del", SDL_SCANCODE_DELETE},
        {"pageup", SDL_SCANCODE_PAGEUP}, {"pagedown", SDL_SCANCODE_PAGEDOWN},
        {"up", SDL_SCANCODE_UP}, {"down", SDL_SCANCODE_DOWN}, {"left", SDL_SCANCODE_LEFT}, {"right", SDL_SCANCODE_RIGHT},
        {"uparrow", SDL_SCANCODE_UP}, {"downarrow", SDL_SCANCODE_DOWN}, {"leftarrow", SDL_SCANCODE_LEFT},
        {"rightarrow", SDL_SCANCODE_RIGHT},
        {"leftshift", SDL_SCANCODE_LSHIFT}, {"lshift", SDL_SCANCODE_LSHIFT}, {"shift", SDL_SCANCODE_LSHIFT},
        {"rightshift", SDL_SCANCODE_RSHIFT}, {"rshift", SDL_SCANCODE_RSHIFT},
        {"leftctrl", SDL_SCANCODE_LCTRL}, {"lctrl", SDL_SCANCODE_LCTRL}, {"leftcontrol", SDL_SCANCODE_LCTRL},
        {"ctrl", SDL_SCANCODE_LCTRL}, {"rightctrl", SDL_SCANCODE_RCTRL}, {"rctrl", SDL_SCANCODE_RCTRL},
        {"rightcontrol", SDL_SCANCODE_RCTRL}, {"leftalt", SDL_SCANCODE_LALT}, {"lalt", SDL_SCANCODE_LALT},
        {"alt", SDL_SCANCODE_LALT}, {"rightalt", SDL_SCANCODE_RALT}, {"ralt", SDL_SCANCODE_RALT},
        {"capslock", SDL_SCANCODE_CAPSLOCK}, {"comma", SDL_SCANCODE_COMMA}, {"period", SDL_SCANCODE_PERIOD},
        {"dot", SDL_SCANCODE_PERIOD}, {"slash", SDL_SCANCODE_SLASH}, {"backslash", SDL_SCANCODE_BACKSLASH},
        {"semicolon", SDL_SCANCODE_SEMICOLON}, {"apostrophe", SDL_SCANCODE_APOSTROPHE}, {"quote", SDL_SCANCODE_APOSTROPHE},
        {"grave", SDL_SCANCODE_GRAVE}, {"backquote", SDL_SCANCODE_GRAVE}, {"minus", SDL_SCANCODE_MINUS},
        {"equals", SDL_SCANCODE_EQUALS}, {"leftbracket", SDL_SCANCODE_LEFTBRACKET},
        {"rightbracket", SDL_SCANCODE_RIGHTBRACKET},
        {"keypadplus", SDL_SCANCODE_KP_PLUS}, {"kpplus", SDL_SCANCODE_KP_PLUS},
        {"keypadminus", SDL_SCANCODE_KP_MINUS}, {"kpminus", SDL_SCANCODE_KP_MINUS},
        {"keypadmultiply", SDL_SCANCODE_KP_MULTIPLY}, {"kpmultiply", SDL_SCANCODE_KP_MULTIPLY},
        {"keypaddivide", SDL_SCANCODE_KP_DIVIDE}, {"kpdivide", SDL_SCANCODE_KP_DIVIDE},
        {"keypadenter", SDL_SCANCODE_KP_ENTER}, {"kpenter", SDL_SCANCODE_KP_ENTER},
        {"keypadperiod", SDL_SCANCODE_KP_PERIOD}, {"kpperiod", SDL_SCANCODE_KP_PERIOD},
        {"keypaddecimal", SDL_SCANCODE_KP_PERIOD},
    };
    for (const auto& [name, code] : named)
        if (n == name) { out = {BoundInput::Key, code}; return true; }
    // F1..F12, keypad/numpad/kp 0..9
    if (n.size() >= 2 && n[0] == 'f') {
        int f = 0;
        for (size_t i = 1; i < n.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(n[i]))) { f = -1; break; }
            f = f * 10 + (n[i] - '0');
        }
        if (f >= 1 && f <= 12) { out = {BoundInput::Key, SDL_SCANCODE_F1 + (f - 1)}; return true; }
    }
    for (const char* prefix : {"keypad", "numpad", "kp"}) {
        const std::string p = prefix;
        if (n.size() == p.size() + 1 && n.compare(0, p.size(), p) == 0 && std::isdigit(static_cast<unsigned char>(n.back()))) {
            const int d = n.back() - '0';
            out = {BoundInput::Key, d == 0 ? SDL_SCANCODE_KP_0 : SDL_SCANCODE_KP_1 + (d - 1)};
            return true;
        }
    }
    static const std::pair<const char*, int> mouse[] = {
        {"mouseleft", SDL_BUTTON_LEFT}, {"mouse1", SDL_BUTTON_LEFT}, {"lmb", SDL_BUTTON_LEFT}, {"leftmouse", SDL_BUTTON_LEFT},
        {"mouseright", SDL_BUTTON_RIGHT}, {"mouse2", SDL_BUTTON_RIGHT}, {"rmb", SDL_BUTTON_RIGHT}, {"rightmouse", SDL_BUTTON_RIGHT},
        {"mousemiddle", SDL_BUTTON_MIDDLE}, {"mouse3", SDL_BUTTON_MIDDLE}, {"mmb", SDL_BUTTON_MIDDLE},
        {"middlemouse", SDL_BUTTON_MIDDLE}, {"mousex1", SDL_BUTTON_X1}, {"mouse4", SDL_BUTTON_X1},
        {"mousex2", SDL_BUTTON_X2}, {"mouse5", SDL_BUTTON_X2},
    };
    for (const auto& [name, code] : mouse)
        if (n == name) { out = {BoundInput::Mouse, code}; return true; }
    return false;
}
inline bool pad_input(const std::string& raw, BoundInput& out) {
    const std::string n = normalize(raw);
    static const std::pair<const char*, int> buttons[] = {
        {"a", SDL_CONTROLLER_BUTTON_A}, {"cross", SDL_CONTROLLER_BUTTON_A},
        {"b", SDL_CONTROLLER_BUTTON_B}, {"circle", SDL_CONTROLLER_BUTTON_B},
        {"x", SDL_CONTROLLER_BUTTON_X}, {"square", SDL_CONTROLLER_BUTTON_X},
        {"y", SDL_CONTROLLER_BUTTON_Y}, {"triangle", SDL_CONTROLLER_BUTTON_Y},
        {"back", SDL_CONTROLLER_BUTTON_BACK}, {"create", SDL_CONTROLLER_BUTTON_BACK}, {"share", SDL_CONTROLLER_BUTTON_BACK},
        {"select", SDL_CONTROLLER_BUTTON_BACK}, {"view", SDL_CONTROLLER_BUTTON_BACK},
        {"guide", SDL_CONTROLLER_BUTTON_GUIDE}, {"ps", SDL_CONTROLLER_BUTTON_GUIDE}, {"home", SDL_CONTROLLER_BUTTON_GUIDE},
        {"start", SDL_CONTROLLER_BUTTON_START}, {"options", SDL_CONTROLLER_BUTTON_START}, {"menu", SDL_CONTROLLER_BUTTON_START},
        {"leftstick", SDL_CONTROLLER_BUTTON_LEFTSTICK}, {"l3", SDL_CONTROLLER_BUTTON_LEFTSTICK}, {"ls", SDL_CONTROLLER_BUTTON_LEFTSTICK},
        {"rightstick", SDL_CONTROLLER_BUTTON_RIGHTSTICK}, {"r3", SDL_CONTROLLER_BUTTON_RIGHTSTICK}, {"rs", SDL_CONTROLLER_BUTTON_RIGHTSTICK},
        {"leftshoulder", SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {"l1", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
        {"lb", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
        {"rightshoulder", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}, {"r1", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        {"rb", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        {"dpup", SDL_CONTROLLER_BUTTON_DPAD_UP}, {"dpadup", SDL_CONTROLLER_BUTTON_DPAD_UP}, {"up", SDL_CONTROLLER_BUTTON_DPAD_UP},
        {"dpdown", SDL_CONTROLLER_BUTTON_DPAD_DOWN}, {"dpaddown", SDL_CONTROLLER_BUTTON_DPAD_DOWN},
        {"down", SDL_CONTROLLER_BUTTON_DPAD_DOWN},
        {"dpleft", SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {"dpadleft", SDL_CONTROLLER_BUTTON_DPAD_LEFT},
        {"left", SDL_CONTROLLER_BUTTON_DPAD_LEFT},
        {"dpright", SDL_CONTROLLER_BUTTON_DPAD_RIGHT}, {"dpadright", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
        {"right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
        {"misc1", SDL_CONTROLLER_BUTTON_MISC1}, {"mute", SDL_CONTROLLER_BUTTON_MISC1}, {"capture", SDL_CONTROLLER_BUTTON_MISC1},
        {"paddle1", SDL_CONTROLLER_BUTTON_PADDLE1}, {"paddle2", SDL_CONTROLLER_BUTTON_PADDLE2},
        {"paddle3", SDL_CONTROLLER_BUTTON_PADDLE3}, {"paddle4", SDL_CONTROLLER_BUTTON_PADDLE4},
        {"touchpad", SDL_CONTROLLER_BUTTON_TOUCHPAD},
    };
    for (const auto& [name, code] : buttons)
        if (n == name) { out = {BoundInput::Button, code}; return true; }
    // CTRL18-INPUT: face buttons by position (south/east/west/north), independent of the printed labels.
    static const std::pair<const char*, int> faces[] = {
        {"south", 0}, {"facesouth", 0}, {"bottom", 0}, {"east", 1}, {"faceeast", 1},
        {"west", 2}, {"facewest", 2}, {"north", 3}, {"facenorth", 3}, {"top", 3},
    };
    for (const auto& [name, code] : faces)
        if (n == name) { out = {BoundInput::Face, code}; return true; }
    static const std::pair<const char*, int> axes[] = {
        {"lefttrigger", SDL_CONTROLLER_AXIS_TRIGGERLEFT}, {"l2", SDL_CONTROLLER_AXIS_TRIGGERLEFT},
        {"lt", SDL_CONTROLLER_AXIS_TRIGGERLEFT}, {"zl", SDL_CONTROLLER_AXIS_TRIGGERLEFT},
        {"righttrigger", SDL_CONTROLLER_AXIS_TRIGGERRIGHT}, {"r2", SDL_CONTROLLER_AXIS_TRIGGERRIGHT},
        {"rt", SDL_CONTROLLER_AXIS_TRIGGERRIGHT}, {"zr", SDL_CONTROLLER_AXIS_TRIGGERRIGHT},
    };
    for (const auto& [name, code] : axes)
        if (n == name) { out = {BoundInput::Axis, code}; return true; }
    return false;
}
// "x+y+z" -> combo. A lone "+" or an empty part is invalid.
inline bool parse_combo(const std::string& text, bool gamepad, InputCombo& combo, std::string& bad) {
    combo = InputCombo{};
    size_t start = 0;
    while (true) {
        const size_t plus = text.find('+', start);
        const std::string part = text.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        BoundInput in;
        const bool ok = !part.empty() && (gamepad ? pad_input(part, in) : key_input(part, in));
        if (!ok || combo.n >= int(combo.in.size())) { bad = part.empty() ? text : part; return false; }
        combo.in[size_t(combo.n++)] = in;
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return combo.n > 0;
}
inline std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "\"" : ", \"") + s + "\"";
    return out;
}
inline bool same_input(const BoundInput& a, const BoundInput& b) { return a.kind == b.kind && a.code == b.code; }
} // namespace bindings_detail

struct InputBindings {
    std::array<InputBinding, kKeyActionCount> keyboard{};
    std::array<InputBinding, kPadActionCount> gamepad{};
    float mouse_sensitivity = 1.0f; // Shift + mouse: 0.008 rad/px x this
    float aim_stick_speed = 1.0f;   // right-stick aim (4 rad/s) and menu cursor (1.6/s) x this
    int left_deadzone = 8000, right_deadzone = 8000; // raw SDL axis units, as before INPUT10
    int trigger_threshold = 16384;                   // trigger counts as pressed above this
    bool gyro_at_start = false;      // [input] gamepad_motion = "gyro"
    bool gyro_auto_calibrate = true;
    float gyro_grip_tilt_deg = 0.0f; // [input] gyro_grip_tilt
    int keyboard_overrides = 0, gamepad_overrides = 0;
    std::vector<std::string> messages; // validation messages (also printed with "[Input] ")
    // CTRL18-INPUT: presets ([input] preset_gamepad / preset_keyboard, env NWII_INPUT_PRESET_GAMEPAD / _KEYBOARD),
    // the intent tables of the switch-hd and kbm presets, and their options.
    PadPreset pad_preset = PadPreset::SwitchHd;
    KeyPreset key_preset = KeyPreset::Kbm;
    std::array<InputBinding, kHdActionCount> gamepad_hd{};
    std::array<InputBinding, kKbmActionCount> kbm{};
    int gamepad_hd_overrides = 0, kbm_overrides = 0;
    bool gyro_player_space = false;
    bool gyro_aim = false, a_does_not_dash = true, fire_on_release = false, quick_items = true;
    float mouse_camera_sensitivity = 1.0f, mouse_aim_sensitivity = 1.0f;
    bool mouse_invert_y = false, flight_invert_y = false;
};
inline const char* pad_preset_name(PadPreset p) {
    return p == PadPreset::Classic ? "classic" : p == PadPreset::WiiMotion ? "wii-motion" : "switch-hd";
}
inline const char* key_preset_name(KeyPreset p) { return p == KeyPreset::Classic ? "classic" : "kbm"; }

// Resolves the defaults plus the Config overrides. Prints every message once with "[Input] ".
inline InputBindings resolve_input_bindings(const Config& cfg, bool print = true) {
    using namespace bindings_detail;
    InputBindings b;
    const auto note = [&b](const std::string& m) { b.messages.push_back(m); };
    // CTRL18-INPUT: `table` / `classic` added (the intent tables have their own names and no classic aliases).
    const auto apply = [&](const auto& actions, auto& slots, const auto& overrides, bool gamepad, int& count,
                           const char* table, bool classic) {
        for (size_t a = 0; a < actions.size(); ++a) {
            for (const char* d : actions[a].defaults) {
                InputCombo c;
                std::string bad;
                if (parse_combo(d, gamepad, c, bad)) slots[a].push_back(c);
                else note(std::string("internal: default ") + table + " " + actions[a].name + " \"" + d + "\" does not resolve");
            }
        }
        std::vector<bool> overridden(actions.size(), false);
        for (const auto& [raw_name, list] : overrides) {
            std::string name = normalize(raw_name);
            if (classic && name == "one") name = "1";
            if (classic && name == "two") name = "2";
            if (classic && name == "shield") name = "nunchukflick";
            size_t a = actions.size();
            for (size_t k = 0; k < actions.size(); ++k)
                if (normalize(actions[k].name) == name) { a = k; break; }
            if (a == actions.size()) {
                std::string valid;
                for (const auto& info : actions) valid += (valid.empty() ? "" : ", ") + std::string(info.name);
                note(std::string("config ") + table + ": unknown action \"" + raw_name + "\" (ignored). Actions: " + valid);
                continue;
            }
            std::vector<std::string> defaults(actions[a].defaults.begin(), actions[a].defaults.end());
            if (list.empty()) {
                slots[a].clear();
                overridden[a] = true;
                ++count;
                note(std::string("config ") + table + " " + actions[a].name + " = [] : unbound (default was " + join(defaults) + ")");
                continue;
            }
            InputBinding parsed;
            std::vector<std::string> accepted;
            for (const auto& text : list) {
                InputCombo c;
                std::string bad;
                if (parse_combo(text, gamepad, c, bad)) { parsed.push_back(c); accepted.push_back(text); }
                else note(std::string("config ") + table + " " + actions[a].name + ": unknown " +
                          (gamepad ? "button" : "key") + " \"" + bad + "\" in \"" + text + "\" (ignored). Examples: " +
                          (gamepad ? kGamepadNameExamples : kKeyboardNameExamples));
            }
            if (parsed.empty()) {
                note(std::string("config ") + table + " " + actions[a].name + ": no valid input left; keeping the default " + join(defaults));
                continue;
            }
            slots[a] = std::move(parsed);
            overridden[a] = true;
            ++count;
            note(std::string("config ") + table + " " + actions[a].name + " (" + actions[a].help + ") = " + join(accepted));
        }
        // Informational: a single input of an overridden action that another action also uses.
        for (size_t a = 0; a < actions.size(); ++a) {
            if (!overridden[a]) continue;
            for (const auto& c : slots[a]) {
                if (c.n != 1) continue;
                for (size_t o = 0; o < actions.size(); ++o) {
                    if (o == a) continue;
                    for (const auto& oc : slots[o])
                        if (oc.n == 1 && same_input(oc.in[0], c.in[0]))
                            note(std::string("note: ") + table + " " + actions[a].name + " shares an input with " + actions[o].name);
                }
            }
        }
    };
    apply(keyboard_actions(), b.keyboard, cfg.keyboard_bindings, false, b.keyboard_overrides, "[input.keyboard]", true);
    apply(gamepad_actions(), b.gamepad, cfg.gamepad_bindings, true, b.gamepad_overrides, "[input.gamepad]", true);
    // CTRL18-INPUT: intent tables and presets.
    apply(hd_pad_intents(), b.gamepad_hd, cfg.gamepad_hd_bindings, true, b.gamepad_hd_overrides, "[input.gamepad_hd]", false);
    apply(kbm_intents(), b.kbm, cfg.kbm_bindings, false, b.kbm_overrides, "[input.kbm]", false);
    std::string presets_text; // appended to the one summary line (INPUT10: defaults give exactly one message)
    {
        std::string pg = cfg.preset_gamepad, pk = cfg.preset_keyboard;
        if (const char* e = std::getenv("NWII_INPUT_PRESET_GAMEPAD"); e && e[0]) pg = e;
        if (const char* e = std::getenv("NWII_INPUT_PRESET_KEYBOARD"); e && e[0]) pk = e;
        pg = normalize(pg);
        pk = normalize(pk);
        b.pad_preset = pg == "classic" || pg == "klassiek" ? PadPreset::Classic
                       : pg == "wiimotion" || pg == "wii" || pg == "gyro" ? PadPreset::WiiMotion : PadPreset::SwitchHd;
        b.key_preset = pk == "classic" || pk == "klassiek" ? KeyPreset::Classic : KeyPreset::Kbm;
        // INPUT10 compatibility: gamepad_motion = "gyro" with the default preset = the Wii motion preset.
        if (cfg.gamepad_motion == "gyro" && cfg.preset_gamepad == "switch-hd" && !std::getenv("NWII_INPUT_PRESET_GAMEPAD"))
            b.pad_preset = PadPreset::WiiMotion;
        b.gyro_aim = cfg.gyro_aim;
        b.gyro_player_space = cfg.gyro_player_space;
        b.a_does_not_dash = cfg.a_does_not_dash;
        b.fire_on_release = cfg.fire_on_release;
        b.quick_items = cfg.quick_items;
        b.mouse_camera_sensitivity = cfg.mouse_camera_sensitivity;
        b.mouse_aim_sensitivity = cfg.mouse_aim_sensitivity;
        b.mouse_invert_y = cfg.mouse_invert_y;
        b.flight_invert_y = cfg.flight_invert_y;
        char line[200];
        std::snprintf(line, sizeof line, "presets: controller \"%s\", keyboard \"%s\"; %d switch-hd and %d kbm override(s)%s%s",
                      pad_preset_name(b.pad_preset), key_preset_name(b.key_preset), b.gamepad_hd_overrides,
                      b.kbm_overrides, b.gyro_aim ? "; gyro aim on" : "", b.a_does_not_dash ? "" : "; A also dashes");
        presets_text = line;
    }
    b.mouse_sensitivity = cfg.mouse_sensitivity;
    b.aim_stick_speed = cfg.aim_stick_speed;
    if (cfg.left_stick_deadzone >= 0) b.left_deadzone = int(std::lround(cfg.left_stick_deadzone * 32768.0));
    if (cfg.right_stick_deadzone >= 0) b.right_deadzone = int(std::lround(cfg.right_stick_deadzone * 32768.0));
    if (cfg.trigger_threshold >= 0) b.trigger_threshold = int(std::lround(cfg.trigger_threshold * 32768.0));
    b.gyro_at_start = cfg.gamepad_motion == "gyro";
    // CTRL18-INPUT: wii-motion starts with the gyro as the remote; switch-hd starts with gyro AIMING per [input] gyro_aim.
    if (b.pad_preset == PadPreset::WiiMotion) b.gyro_at_start = true;
    else if (b.pad_preset == PadPreset::SwitchHd) b.gyro_at_start = b.gyro_aim;
    b.gyro_auto_calibrate = cfg.gyro_auto_calibrate;
    b.gyro_grip_tilt_deg = cfg.gyro_grip_tilt;
    char summary[360];
    std::snprintf(summary, sizeof summary,
                  "bindings: %d keyboard and %d gamepad override(s); mouse_sensitivity=%g aim_stick_speed=%g "
                  "stick deadzones=%.3f/%.3f trigger_threshold=%.3f gamepad_motion=%s gyro_grip_tilt=%g%s",
                  b.keyboard_overrides, b.gamepad_overrides, b.mouse_sensitivity, b.aim_stick_speed,
                  b.left_deadzone / 32768.0, b.right_deadzone / 32768.0, b.trigger_threshold / 32768.0,
                  b.gyro_at_start ? "gyro" : "off", b.gyro_grip_tilt_deg,
                  b.gyro_auto_calibrate ? "" : " (no gyro auto-calibration)");
    note(std::string(summary) + "; " + presets_text);
    if (print)
        for (const auto& m : b.messages) std::cout << "[Input] " << m << '\n';
    if (print) std::cout.flush();
    return b;
}

inline InputBindings& input_bindings_storage() {
    static InputBindings bindings = resolve_input_bindings(Config::get());
    return bindings;
}
// Resolved once, on first use (the first source update, after Config::load).
inline const InputBindings& input_bindings() { return input_bindings_storage(); }
// Tests (or a config reload): resolve again from Config::get().
inline void reload_input_bindings(bool print = true) {
    input_bindings_storage() = resolve_input_bindings(Config::get(), print);
}
// CTRL18-INPUT: preset swap / rebinding while the game runs. The UI changes Config::get() and calls
// request_input_bindings_reload(); the InputManager thread (the first source update after it) resolves the bindings
// again, so no source ever reads a half-replaced table.
inline std::atomic<uint32_t> g_input_bindings_reload{0};
inline void request_input_bindings_reload() { g_input_bindings_reload.fetch_add(1, std::memory_order_release); }
inline void apply_pending_input_bindings_reload() {
    static uint32_t applied = 0;
    const uint32_t want = g_input_bindings_reload.load(std::memory_order_acquire);
    if (want == applied) return;
    applied = want;
    reload_input_bindings(true);
}

// Keyboard/mouse state of one combo. Alt+Enter toggles fullscreen (RES9), so Return/Keypad Enter do not
// count while Alt is held (unless the combo itself includes Alt).
inline bool key_combo_held(const InputCombo& c, const uint8_t* keys, uint32_t mouse_buttons) {
    if (c.n <= 0) return false;
    bool has_alt = false, has_enter = false;
    for (int k = 0; k < c.n; ++k) {
        const auto& in = c.in[size_t(k)];
        if (in.kind == BoundInput::Key) {
            if (!keys[in.code]) return false;
            has_alt = has_alt || in.code == SDL_SCANCODE_LALT || in.code == SDL_SCANCODE_RALT;
            has_enter = has_enter || in.code == SDL_SCANCODE_RETURN || in.code == SDL_SCANCODE_KP_ENTER;
        } else if (in.kind == BoundInput::Mouse) {
            if (!(mouse_buttons & SDL_BUTTON(in.code))) return false;
        } else {
            return false;
        }
    }
    if (has_enter && !has_alt && (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT])) return false;
    return true;
}
inline bool key_action_held(KeyAction a, const uint8_t* keys, uint32_t mouse_buttons) {
    for (const auto& c : input_bindings().keyboard[size_t(a)])
        if (key_combo_held(c, keys, mouse_buttons)) return true;
    return false;
}
// CTRL18-INPUT: kbm intent state.
inline bool kbm_action_held(KbmAction a, const uint8_t* keys, uint32_t mouse_buttons) {
    for (const auto& c : input_bindings().kbm[size_t(a)])
        if (key_combo_held(c, keys, mouse_buttons)) return true;
    return false;
}
// CTRL18-INPUT: SDL_GameControllerButton of a face button BY POSITION (0 south, 1 east, 2 west, 3 north) for an
// SDL_GameControllerType. SDL 2.28 reports Nintendo pads by LABEL (SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS default
// 1): their "a" is the east button. Every other type: a = south, b = east, x = west, y = north.
inline int face_button_for(int position, int pad_type) {
    const bool nintendo = pad_type == 5 || pad_type == 11 || pad_type == 12 || pad_type == 13;
    static const int xbox[4] = {SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
                                SDL_CONTROLLER_BUTTON_Y};
    static const int nin[4] = {SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_Y,
                               SDL_CONTROLLER_BUTTON_X};
    if (position < 0 || position > 3) return -1;
    return nintendo ? nin[position] : xbox[position];
}

} // namespace nwii::runtime::input
