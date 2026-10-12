#pragma once
// CTRL18-UI: descriptor list of the settings the "Besturing" tab edits (one row per setting), so a future launcher
// (wave 19: settings list + description pane) can render every setting from data. The controls tab itself takes its
// labels and tooltips from here (ui_controls_tab.cpp), so text lives in one place. Pure data, header-only.
//
// Fields: id (= the ui::Settings field and the autotest name "saved.<id>" / "edit.<id>"), TOML section + key,
// Dutch label, Dutch description (one paragraph, for a side panel), type, range, default, restart needed in game,
// which preset uses / sets it, and the track that reads it at run time.
#include <string_view>
#include <vector>

namespace nwii::ui {

enum class SettingType { Bool, Int, Float, Choice, Bindings };

struct SettingDesc {
    const char* id;
    const char* section;
    const char* key;
    const char* label;
    const char* help;
    SettingType type;
    float min, max;
    const char* def;     // TOML literal of the default
    const char* choices; // Choice: "value=Label|value=Label|..."
    bool restart;        // only after a restart when changed in game
    const char* presets; // "alle", "switch-hd", "kbm", ...
    const char* reader;  // who reads it
};

inline const std::vector<SettingDesc>& ControlsSettings() {
    static const std::vector<SettingDesc> d = {
        {"preset_gamepad", "input", "preset_gamepad", "Indeling controller",
         "Hoe je controller het spel bestuurt. 'Switch HD (knoppen)' is de indeling van Skyward Sword HD zonder "
         "bewegingsbesturing: rechterstick = zwaard, ZR = voorwerp, L + rechterstick = camera. 'Klassiek' is de "
         "indeling van voor deze versie. 'Wii (beweging/gyro)' gebruikt de bewegingssensoren van een DualSense of "
         "Switch Pro als Wii Remote; de rechterstick bestuurt dan de camera. De knoppen staan op dezelfde plek als "
         "op een Switch: de rechter knop (Cirkel / B) is A.",
         SettingType::Choice, 0, 0, "\"switch-hd\"",
         "switch-hd=Switch HD (knoppen)|classic=Klassiek|wii-motion=Wii (beweging/gyro)", false, "alle", "CTRL18-INPUT"},
        {"preset_keyboard", "input", "preset_keyboard", "Indeling toetsenbord + muis",
         "'Toetsenbord + muis' is een moderne indeling: WASD lopen, muis = camera, linkermuisknop = zwaard (slepen = "
         "gerichte slag), rechtermuisknop = schild, E = actie, R = voorwerp, Esc = uitrusting. 'Klassiek' is de "
         "indeling van voor deze versie (muis = Wii-wijzer, numeriek toetsenblok = zwaardslagen).",
         SettingType::Choice, 0, 0, "\"kbm\"", "kbm=Toetsenbord + muis|classic=Klassiek", false, "alle", "CTRL18-INPUT"},
        {"free_camera", "camera", "free_camera", "Vrije camera",
         "Draai de camera vrij rond Link: met de controller L vasthouden + rechterstick, met de muis gewoon bewegen. "
         "Het spel doet zelf de botsingen met muren en het terugveren. Uit = precies de camera van de Wii. In de "
         "Wii-indeling (beweging) staat de vrije camera altijd uit.",
         SettingType::Bool, 0, 0, "true", nullptr, false, "switch-hd, kbm (uit in wii-motion)", "CTRL18-CAM"},
        {"camera_speed", "camera", "speed", "Camerasnelheid",
         "Hoe snel de vrije camera draait bij een volledig ingedrukte stick, in 5 stappen zoals in Skyward Sword HD "
         "(1 = rustig, 5 = snel).",
         SettingType::Int, 1, 5, "3", nullptr, false, "switch-hd, kbm", "CTRL18-CAM"},
        {"camera_invert_x", "camera", "invert_x", "Camera horizontaal omkeren",
         "Stick of muis naar rechts draait de camera naar links.", SettingType::Bool, 0, 0, "false", nullptr, false,
         "switch-hd, kbm", "CTRL18-CAM"},
        {"camera_invert_y", "camera", "invert_y", "Camera verticaal omkeren",
         "Stick of muis omhoog laat de camera omlaag kijken.", SettingType::Bool, 0, 0, "false", nullptr, false,
         "switch-hd, kbm", "CTRL18-CAM"},
        {"flight_invert_y", "camera", "flight_invert_y", "Vliegen: omhoog/omlaag omkeren",
         "Alleen voor de Loftwing: stick naar je toe = stijgen (zoals een vliegtuig). Staat los van de camera.",
         SettingType::Bool, 0, 0, "false", nullptr, false, "switch-hd, kbm", "CTRL18-INPUT"},
        {"mouse_camera_sensitivity", "mouse", "sensitivity_camera", "Muisgevoeligheid camera",
         "Hoeveel de camera draait per centimeter muisbeweging (1,00 = standaard).", SettingType::Float, 0.1f, 5.0f,
         "1.0", nullptr, false, "kbm", "CTRL18-CAM"},
        {"mouse_aim_sensitivity", "mouse", "sensitivity_aim", "Muisgevoeligheid richten",
         "Hoe snel je richt met de muis als je een voorwerp vasthoudt of in eerste persoon kijkt (1,00 = standaard).",
         SettingType::Float, 0.1f, 5.0f, "1.0", nullptr, false, "kbm", "CTRL18-INPUT"},
        {"rumble", "input", "rumble", "Trillen",
         "Hoe sterk de controller trilt als het spel de Wii Remote laat trillen (zwaardslagen, schild, schatten). "
         "0 = uit. Toetsenbord en muis trillen nooit.",
         SettingType::Int, 0, 100, "100", nullptr, false, "alle controllers", "CTRL18-CAM"},
        {"gyro_aim", "input", "gyro_aim", "Richten met gyro",
         "Alleen in 'Switch HD (knoppen)' met een DualSense, DUALSHOCK 4 of Switch Pro: als je richt (boog, katapult, "
         "eerste persoon), menu's bedient of een grote sleutel draait, beweegt de controller mee. Zwaardslagen blijven "
         "op de rechterstick.",
         SettingType::Bool, 0, 0, "false", nullptr, false, "switch-hd", "CTRL18-INPUT"},
        {"glyph_set", "graphics", "glyph_set", "Knopiconen",
         "Welke knopplaatjes het spel toont (HUD, uitleg in teksten, menu's). 'Automatisch' volgt het apparaat dat je "
         "het laatst gebruikte en de indeling die daarbij hoort. Plaatjes: Kenney 'Input Prompts' (CC0). "
         "'Wii (origineel)' toont de Wii Remote-knoppen van het spel.",
         SettingType::Choice, 0, 0, "\"auto\"",
         "auto=Automatisch|ps=PlayStation|xbox=Xbox|switch=Nintendo Switch|kbm=Toetsenbord + muis|wii=Wii (origineel)",
         false, "alle", "CTRL18-UI (hd_textures.cpp)"},
        {"bind_pad_hd", "input.gamepad_hd", "<actie>", "Knoppen (Switch HD)",
         "Eigen knoppen voor de indeling 'Switch HD (knoppen)'. Gezichtsknoppen worden op hun PLEK bewaard (east = de "
         "rechter knop), zodat dezelfde instelling op elke controller werkt.",
         SettingType::Bindings, 0, 0, "", nullptr, false, "switch-hd", "CTRL18-INPUT"},
        {"bind_pad_classic", "input.gamepad", "<actie>", "Knoppen (Klassiek / Wii)",
         "Eigen knoppen voor de controllerindelingen 'Klassiek' en 'Wii (beweging/gyro)'.", SettingType::Bindings, 0, 0,
         "", nullptr, false, "classic, wii-motion", "CTRL18-INPUT"},
        {"bind_kbm", "input.kbm", "<actie>", "Toetsen (Toetsenbord + muis)",
         "Eigen toetsen voor de indeling 'Toetsenbord + muis'. F1 (menu), F11 en F12 zijn gereserveerd.",
         SettingType::Bindings, 0, 0, "", nullptr, false, "kbm", "CTRL18-INPUT"},
        {"bind_kb_classic", "input.keyboard", "<actie>", "Toetsen (Klassiek)",
         "Eigen toetsen voor de klassieke toetsenbordindeling.", SettingType::Bindings, 0, 0, "", nullptr, false,
         "classic", "CTRL18-INPUT"},
        {"gyro_hint_shown", "ui", "gyro_hint_shown", "Gyro-tip getoond",
         "Intern: de eenmalige tip over richten met gyro is al getoond.", SettingType::Bool, 0, 0, "false", nullptr,
         false, "switch-hd", "CTRL18-UI"},
    };
    return d;
}

inline const SettingDesc* FindSetting(const char* id) {
    for (const SettingDesc& s : ControlsSettings())
        if (std::string_view(s.id) == id) return &s;
    return nullptr;
}

} // namespace nwii::ui
