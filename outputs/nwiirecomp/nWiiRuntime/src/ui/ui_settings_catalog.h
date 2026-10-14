#pragma once
// LAUNCH19: ONE table of every user-facing setting of SkyBreak (design: work/claude-notes/LAUNCH19-design.md s3). It drives
//   - the launcher settings page (tabs, left list, right description pane, value editors),
//   - the in-game F1 menu (the same page; rows flagged kLauncherOnly hidden, AtBoot rows get a restart badge),
//   - the TOML write-back (DiffEdits/AllEdits generated from section/key/choices instead of hand-written code),
//   - the restart markers (apply) and reset-to-default (def),
//   - the UIT14 autotest names ("saved.<id>", "edit.<id>", click "<label>").
// A new setting = one row here + one field in Settings (+ its reader): it appears in the launcher AND the F1 menu.
//
// Compatible extension of CTRL18-UI's SettingDesc (src/ui/ui_settings_desc.h): same id / section / key / def /
// min / max meaning; adds tab + group, EN texts, editor kind, apply level, env override, status, accessors and an
// availability rule. CTRL18-UI's ControlsSettings() rows are copied 1:1 below (tab Besturing) so its tab can keep
// calling FindSetting() after the switch.
//
// Header-only, no SDL/ImGui: unit-testable (outputs/pc-tests/launch19/catalog-test.cpp). The generic TOML literal of a
// row (Literal) and its default (ApplyDefault) live here too, so ui_settings.cpp's DiffEdits/AllEdits/NeedsRestart and
// the settings page share one implementation.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "ui_settings.h"

namespace nwii::ui::catalog {

enum class Tab : uint8_t { Start, Beeld, Geluid, Besturing, Gameplay, Interface, Geavanceerd, Count };
enum class Kind : uint8_t {
    Toggle,   // bool
    Slider,   // int or float (min..max, step); fmt shows the value
    Choice,   // int (Choice::ival) or string (Choice::toml without quotes); left/right cycles
    Path,     // string; A opens the Windows folder dialog (IFileOpenDialog + FOS_PICKFOLDERS)
    Action,   // a button row (no value): open folder, import, reset, sub page
    Info,     // read-only value (renderer, disc, settings file)
    Bindings, // CTRL18-UI rebind table: opens the rebind sub page
};
enum class Apply : uint8_t {
    Live,     // applied at once, launcher and in game
    AtBoot,   // read when the game boots: from the launcher it applies; in game "na herstart"
    Restart,  // read when SkyBreak itself starts (window/GL/threads): always "na herstart"
    None,     // Info / Action
};
enum class Status : uint8_t {
    Exists,       // in the UI today
    ExistsHidden, // a TOML key that works today but has no UI
    EnvOnly,      // only an NWII_* environment switch today -> gets a TOML key
    New,          // new, host-side only (no game hook)
    NewHook,      // new and needs a game hook (none in the v1 table; listed in the design as ideas)
};
enum Flags : uint8_t {
    kLauncherOnly = 1, // not in the F1 menu (play button, game folder editing)
    kAdvanced = 2,     // under the "Geavanceerd" group header, collapsed by default on small screens
    kExperimental = 4, // "experimenteel" badge
    kDanger = 8,       // asks for confirmation (reset all, disable F1 menu)
    kPercent = 16,     // float slider shown x100 with "%" (0.8 -> 80%)
    // REVLAUNCH19: never touched by "Standaard" / reset tab / reset all. The save folder: its catalog default ("" =
    // <exe>\..\nand) is NOT the folder SkyBreak Setup / Bouw wrote ([nand] root = %APPDATA%/SkywardSwordPC/nand), so a
    // reset would hide the player's save and put new progress in the install folder.
    kNoReset = 32,
};

struct Text {
    const char* nl;
    const char* en;
};
struct Choice {
    const char* toml; // TOML literal written for this choice; "" = remove the key (runtime default)
    Text label;
    int ival;         // value of an int field (ignored for string fields)
};
using Ref = std::variant<std::monostate, bool*, int*, float*, std::string*>;

// Facts the availability rules need (filled by ui_app.cpp each frame).
struct Ctx {
    bool in_game = false;
    bool hd_pack_found = false;
    bool gyro_pad = false;      // a connected pad has a gyro (DualSense/DS4/Switch Pro)
    bool disc_ok = false;
    const char* (*env)(const char*) = nullptr; // getenv; a set env var locks the row
};
struct Avail {
    bool visible = true;
    bool enabled = true;
    Text why{nullptr, nullptr}; // shown under the description when !enabled
};

struct Desc {
    const char* id;    // = Settings field name / autotest id
    Tab tab;
    Tab also;          // second tab that shows the SAME row (Tab::Count = none)
    Text group;        // group header inside the tab
    const char* section;
    const char* key;
    Text label;
    Text help;         // one paragraph for the description pane
    Kind kind;
    float min, max, step;
    const char* fmt;   // Slider: printf format of the value ("%d%%", "%.2f"); nullptr = plain
    const char* def;   // TOML literal of the default
    std::span<const Choice> choices;
    Apply apply;
    const char* env;   // NWII_* that overrides the key (row locked + note while it is set)
    Status status;
    const char* reader; // who reads it at run time
    Ref (*ref)(Settings&);
    Avail (*avail)(const Settings&, const Ctx&);
    uint8_t flags;
    Text also_group{nullptr, nullptr}; // group header when shown in `also` (nullptr = same group)
};

// ----------------------------------------------------------------------------------------------- choices
inline constexpr Choice kLangGame[] = {{"\"en\"", {"English", "English"}, 0},
                                       {"\"fr\"", {"Fran\xC3\xA7" "ais", "Fran\xC3\xA7" "ais"}, 1},
                                       {"\"es\"", {"Espa\xC3\xB1ol", "Espa\xC3\xB1ol"}, 2}};
inline constexpr Choice kLangUi[] = {{"\"nl\"", {"Nederlands", "Nederlands"}, 0}, {"\"en\"", {"English", "English"}, 1}};
inline constexpr Choice kBackups[] = {{"0", {"Uit", "Off"}, 0}, {"3", {"Laatste 3", "Last 3"}, 3},
                                      {"5", {"Laatste 5", "Last 5"}, 5}, {"10", {"Laatste 10", "Last 10"}, 10}};
inline constexpr Choice kWindowMode[] = {{"false", {"Venster", "Window"}, 0}, {"true", {"Volledig scherm", "Fullscreen"}, 1}};
inline constexpr Choice kScale[] = {{"\"auto\"", {"Automatisch", "Automatic"}, 0},
                                    {"1", {"1x  640 x 528 (origineel)", "1x  640 x 528 (original)"}, 1},
                                    {"2", {"2x  1280 x 1056", "2x  1280 x 1056"}, 2},
                                    {"3", {"3x  1920 x 1584", "3x  1920 x 1584"}, 3},
                                    {"4", {"4x  2560 x 2112", "4x  2560 x 2112"}, 4},
                                    {"5", {"5x  3200 x 2640", "5x  3200 x 2640"}, 5},
                                    {"6", {"6x  3840 x 3168", "6x  3840 x 3168"}, 6}};
inline constexpr Choice kAspect[] = {{"\"auto\"", {"Automatisch", "Automatic"}, 0}, {"\"4:3\"", {"4:3", "4:3"}, 1},
                                     {"\"16:9\"", {"16:9", "16:9"}, 2},
                                     {"\"stretch\"", {"Uitrekken tot het venster", "Stretch to window"}, 3}};
inline constexpr Choice kWide[] = {{"", {"Standaard (16:9)", "Default (16:9)"}, -1}, {"true", {"16:9", "16:9"}, 1},
                                   {"false", {"4:3", "4:3"}, 0}};
inline constexpr Choice kPacing[] = {{"\"auto\"", {"Automatisch (aanbevolen)", "Automatic (recommended)"}, 0},
                                     {"\"dwm\"", {"Compositor (DWM)", "Compositor (DWM)"}, 0},
                                     {"\"vsync\"", {"VSync van de driver", "Driver VSync"}, 0},
                                     {"\"none\"", {"Geen limiet", "Unlimited"}, 0}};
inline constexpr Choice kLatency[] = {{"0", {"Standaard (80 ms)", "Default (80 ms)"}, 0}, {"40", {"40 ms (krap)", "40 ms (tight)"}, 40},
                                      {"60", {"60 ms", "60 ms"}, 60}, {"100", {"100 ms", "100 ms"}, 100},
                                      {"150", {"150 ms", "150 ms"}, 150}, {"200", {"200 ms (ruim)", "200 ms (safe)"}, 200}};
inline constexpr Choice kAudioDev[] = {{"\"\"", {"Windows-standaard", "Windows default"}, 0}};
inline constexpr Choice kPresetPad[] = {{"\"switch-hd\"", {"Switch HD (knoppen)", "Switch HD (buttons)"}, 0},
                                        {"\"classic\"", {"Klassiek", "Classic"}, 0},
                                        {"\"wii-motion\"", {"Wii (beweging/gyro)", "Wii (motion/gyro)"}, 0}};
inline constexpr Choice kPresetKb[] = {{"\"kbm\"", {"Toetsenbord + muis", "Keyboard + mouse"}, 0},
                                       {"\"classic\"", {"Klassiek", "Classic"}, 0}};
inline constexpr Choice kGlyphs[] = {{"\"auto\"", {"Automatisch", "Automatic"}, 0}, {"\"ps\"", {"PlayStation", "PlayStation"}, 0},
                                     {"\"xbox\"", {"Xbox", "Xbox"}, 0}, {"\"switch\"", {"Nintendo Switch", "Nintendo Switch"}, 0},
                                     {"\"kbm\"", {"Toetsenbord + muis", "Keyboard + mouse"}, 0},
                                     {"\"wii\"", {"Wii (origineel)", "Wii (original)"}, 0}};
inline constexpr Choice kUiScale[] = {{"0", {"Automatisch", "Automatic"}, 0}, {"75", {"75%", "75%"}, 75},
                                      {"100", {"100%", "100%"}, 100}, {"125", {"125%", "125%"}, 125},
                                      {"150", {"150%", "150%"}, 150}, {"200", {"200%", "200%"}, 200}};
inline constexpr Choice kAccent[] = {{"\"gold\"", {"Goud", "Gold"}, 0}, {"\"red\"", {"Loftwing-rood", "Loftwing red"}, 0},
                                     {"\"sky\"", {"Hemelsblauw", "Sky blue"}, 0}};
// LAUNCH19 impl: "\"\"" (not "" = remove the key) so the write-back stays byte-identical to THPVID19's hand-written edit.
inline constexpr Choice kBgVideo[] = {{"\"\"", {"Openingsfilm (Demo01_02)", "Opening movie (Demo01_02)"}, 0},
                                      {"\"Demo01_01.thp\"", {"Proloog (Demo01_01)", "Prologue (Demo01_01)"}, 0}};
inline constexpr Choice kPerf[] = {{"\"off\"", {"Uit", "Off"}, 0}, {"\"fps\"", {"FPS", "FPS"}, 0},
                                   {"\"frametime\"", {"FPS + beeldtijd", "FPS + frame time"}, 0},
                                   {"\"full\"", {"Uitgebreid (spel-fps, tussenbeelden)", "Detailed (game fps, in-between frames)"}, 0}}; // REVLAUNCH19
inline constexpr Choice kConfirm[] = {{"\"east\"", {"Rechter knop (zoals Switch)", "Right button (like Switch)"}, 0},
                                      {"\"south\"", {"Onderste knop (zoals Xbox/PlayStation)", "Bottom button (like Xbox/PlayStation)"}, 0}};
inline constexpr Choice kClock[] = {{"\"host\"", {"Klok van Windows", "Windows clock"}, 0},
                                    {"\"fixed\"", {"Vast: 1 januari 2000", "Fixed: 1 January 2000"}, 0}};
// LAUNCH19 impl: a Toggle row with exactly two choices writes their literals (off, on) instead of false/true.
inline constexpr Choice kGyroLits[] = {{"\"off\"", {"Uit", "Off"}, 0}, {"\"gyro\"", {"Aan", "On"}, 1}};
// MVGEN21: [graphics] frame_interpolation_mode (runtime/interp_config.h ParseInterpMode)
inline constexpr Choice kInterpMode[] = {{"\"interp\"", {"Interpolatie", "Interpolation"}, 0},
                                         {"\"dup\"", {"Vorig beeld herhalen (vergelijken)", "Repeat (A/B test)"}, 0}};
inline constexpr std::span<const Choice> kNone{};

// ----------------------------------------------------------------------------------------------- helpers
#define L19_REF(f) +[](Settings& s) -> Ref { return &s.f; }
#define L19_NOREF nullptr
#define L19_G(nl, en) Text{nl, en}
inline constexpr Tab kNoTab = Tab::Count;

inline Avail Locked(const Ctx& c, const char* env) {
    if (env && c.env && c.env(env) && *c.env(env))
        return {true, false, {"Vastgezet door een omgevingsvariabele (zie hieronder).", "Locked by an environment variable."}};
    return {};
}

// ----------------------------------------------------------------------------------------------- the table
inline std::span<const Desc> All() {
    static const Desc d[] = {
        // ================================================================== START (Dusklight: PRELAUNCH)
        {"act_play", Tab::Start, kNoTab, L19_G("Spel", "Game"), nullptr, nullptr,
         {"Spelen", "Play"},
         {"Start The Legend of Zelda: Skyward Sword met de instellingen hieronder. Ook met F5, of met de "
          "Start-/Options-knop op je controller, vanaf elk tabblad.", // REVLAUNCH19: was "Enter" (= only this row)
          "Start The Legend of Zelda: Skyward Sword with the settings below. Also F5 or the Start/Options button."},
         Kind::Action, 0, 0, 0, nullptr, "", kNone, Apply::None, nullptr, Status::Exists, "ui_app.cpp", L19_NOREF,
         +[](const Settings&, const Ctx& c) -> Avail {
             if (!c.disc_ok) return {true, false, {"Eerst moeten de spelbestanden kloppen (zie hieronder).", "Game files must check out first."}};
             return {};
         },
         kLauncherOnly},
        // LAUNCH19 impl: an Info row (not a [game] dir key): SkyBreak.exe passes the game folder on the command line
        // (INSTALL19 stub: <install>\game or hulp\spelmap.txt); another folder is chosen in SkyBreak Setup.
        {"game_dir", Tab::Start, kNoTab, L19_G("Spel", "Game"), nullptr, nullptr,
         {"Spelbestanden (schijf)", "Game files (disc)"},
         {"De map met de bestanden van JOUW schijf (The Legend of Zelda: Skyward Sword, USA 1.00, SOUE01), "
          "uitgepakt door SkyBreak Setup. SkyBreak controleert of de bestanden precies kloppen. Kiezen opent de map. "
          "Een andere schijf of map kies je in SkyBreak Setup (Herstellen); SkyBreak leest zelf geen schijfkopie "
          "(.iso, .rvz, .wbfs).",
          "The folder with the files of YOUR disc (SOUE01, USA 1.00), extracted by SkyBreak Setup. Confirm opens it. "
          "Another disc or folder is chosen in SkyBreak Setup (Repair)."},
         Kind::Info, 0, 0, 0, nullptr, "", kNone, Apply::None, nullptr, Status::New, "main.cpp (argv[1])",
         L19_NOREF, nullptr, 0},
        {"game_language", Tab::Start, kNoTab, L19_G("Spel", "Game"), "nand", "language",
         {"Taal van het spel", "Game language"},
         {"De taal van de teksten en menu's in het spel. Jouw schijf (USA) bevat Engels, Frans en Spaans. "
          "SkyBreak zet dit in de Wii-systeeminstellingen (SYSCONF) voordat het spel start, net als de taal op een "
          "Wii. Je save blijft gewoon werken.",
          "The language of the game's texts. The USA disc has English, French and Spanish (Wii system setting)."},
         Kind::Choice, 0, 0, 0, nullptr, "\"en\"", kLangGame, Apply::AtBoot, nullptr, Status::New,
         "nand_fs.cpp ensure_system_files (IPL.LNG, like IPL.AR)", L19_REF(game_language), nullptr, 0},
        {"ui_language", Tab::Start, Tab::Interface, L19_G("Spel", "Game"), "ui", "language",
         {"Taal van SkyBreak", "SkyBreak language"},
         {"De taal van deze launcher, het F1-menu en de meldingen. Verandert meteen.",
          "The language of this launcher, the F1 menu and notices. Changes at once."},
         Kind::Choice, 0, 0, 0, nullptr, "\"nl\"", kLangUi, Apply::Live, nullptr, Status::New, "ui_app.cpp (Text lookup)",
         L19_REF(ui_language), nullptr, 0, L19_G("Weergave", "Appearance")},
        {"renderer", Tab::Start, kNoTab, L19_G("Spel", "Game"), "graphics", "backend",
         {"Grafische weergave", "Graphics backend"},
         {"SkyBreak tekent met OpenGL (4.3 of nieuwer). Hier zie je welke videokaart en driver gebruikt worden. "
          "Andere weergaven (Vulkan, Direct3D) zijn er nog niet.",
          "SkyBreak renders with OpenGL 4.3+. Shows the GPU and driver in use. Vulkan/Direct3D are not available."},
         Kind::Info, 0, 0, 0, nullptr, "\"opengl\"", kNone, Apply::None, nullptr, Status::Exists,
         "renderer_gl.cpp (GL_RENDERER)", L19_NOREF, nullptr, 0},
        {"nand_root", Tab::Start, kNoTab, L19_G("Opslag", "Saves"), "nand", "root",
         {"Opslagmap (saves)", "Data folder (saves)"},
         {"Waar je opgeslagen spel staat: een Wii-NAND-map met dezelfde indeling als Dolphin "
          "(title/00010000/534f5545/data). Standaard %APPDATA%\\SkywardSwordPC\\nand. SkyBreak verplaatst of kopieert "
          "je save niet: kies je een andere map, dan begin je daar zonder save (kopieer hem zelf via 'Opslagmap "
          "openen'); je oude map blijft gewoon staan. 'Standaard' en terugzetten laten deze map met rust.",
          "Where your saves live: a Wii NAND folder in Dolphin's layout. Your save is not moved or copied: another "
          "folder starts without a save; the old folder is kept. Resetting never changes this folder."},
         Kind::Path, 0, 0, 0, nullptr, "\"\"", kNone, Apply::AtBoot, "NWII_NAND_ROOT", Status::ExistsHidden,
         "Config::nand_root (nand_fs.cpp)", L19_REF(nand_root),
         +[](const Settings&, const Ctx& c) -> Avail {
             if (c.in_game) return {true, false, {"Wijzigen kan alleen in de launcher.", "Only in the launcher."}};
             return Locked(c, "NWII_NAND_ROOT");
         },
         kNoReset}, // REVLAUNCH19
        {"save_type", Tab::Start, kNoTab, L19_G("Opslag", "Saves"), nullptr, nullptr,
         {"Soort opslag", "Save file type"},
         {"SkyBreak bewaart je spel zoals een Wii dat doet: als bestanden in een NAND-map (indeling van Dolphin). "
          "Daardoor kun je saves uitwisselen met Dolphin. Een Wii-SD-kaartbestand (data.bin) kan niet: dat is "
          "versleuteld met sleutels van Nintendo.",
          "Saves are stored like a Wii does, as files in a NAND folder (Dolphin layout). data.bin is not supported."},
         Kind::Info, 0, 0, 0, nullptr, "", kNone, Apply::None, nullptr, Status::Exists, "nand_fs.cpp", L19_NOREF, nullptr, 0},
        {"act_import_dolphin", Tab::Start, kNoTab, L19_G("Opslag", "Saves"), nullptr, nullptr,
         {"Save uit Dolphin importeren...", "Import save from Dolphin..."},
         {"Kies je Dolphin-gebruikersmap (bijvoorbeeld Documenten\\Dolphin Emulator) of de map Wii daarin. SkyBreak "
          "kopieert de save van Skyward Sword (USA) naar je opslagmap. Je huidige save gaat eerst naar een "
          "reservekopie met datum.",
          "Pick your Dolphin user folder. The Skyward Sword (USA) save is copied in; your current save is backed up first."},
         Kind::Action, 0, 0, 0, nullptr, "", kNone, Apply::None, nullptr, Status::New, "ui_saves.cpp (new)", L19_NOREF,
         +[](const Settings&, const Ctx& c) -> Avail {
             if (c.in_game) return {true, false, {"Alleen in de launcher (het spel gebruikt de save nu).", "Only in the launcher."}};
             return {};
         },
         kLauncherOnly},
        {"act_export_dolphin", Tab::Start, kNoTab, L19_G("Opslag", "Saves"), nullptr, nullptr,
         {"Save naar Dolphin exporteren...", "Export save to Dolphin..."},
         {"Kopieert je save naar een Dolphin-gebruikersmap die je kiest, zodat je in Dolphin verder kunt spelen. "
          "Een save die daar al staat gaat eerst naar een reservekopie.",
          "Copies your save into a Dolphin user folder you pick; an existing save there is backed up first."},
         Kind::Action, 0, 0, 0, nullptr, "", kNone, Apply::None, nullptr, Status::New, "ui_saves.cpp (new)", L19_NOREF,
         nullptr, kLauncherOnly},
        {"save_backups", Tab::Start, kNoTab, L19_G("Opslag", "Saves"), "nand", "backups",
         {"Reservekopie bij elke start", "Backup at every start"},
         {"Voordat het spel start kopieert SkyBreak je save naar de map reservekopie naast je opslagmap "
          "(standaard %APPDATA%\\SkywardSwordPC\\reservekopie\\<datum>). De oudste kopie verdwijnt als er "
          "meer zijn dan je hier kiest. Kost een paar kilobyte.", // REVLAUNCH19: next to the data folder (BackupRoot)
          "Before the game starts, your save is copied to the folder reservekopie next to your data folder; keeps "
          "the newest N."},
         Kind::Choice, 0, 0, 0, nullptr, "5", kBackups, Apply::AtBoot, nullptr, Status::New, "ui_saves.cpp (new)",
         L19_REF(save_backups), nullptr, 0},
        {"act_open_saves", Tab::Start, kNoTab, L19_G("Opslag", "Saves"), nullptr, nullptr,
         {"Opslagmap openen", "Open data folder"},
         {"Opent de map met je save (de Wii-NAND-map) in Verkenner. Handig om zelf een kopie te maken of een "
          "save van een vriend neer te zetten; doe dat alleen terwijl het spel niet draait.",
          "Opens the save folder (the Wii NAND folder) in Explorer. Only copy files while the game is not running."},
         Kind::Action, 0, 0, 0, nullptr, "", kNone, Apply::None, nullptr, Status::Exists, "ui_app.cpp OpenFolder",
         L19_NOREF, nullptr, 0},

        // ================================================================== BEELD (Dusklight: VIDEO)
        {"fullscreen", Tab::Beeld, kNoTab, L19_G("Scherm", "Display"), "graphics", "fullscreen",
         {"Weergave", "Display mode"},
         {"Venster of volledig scherm. Wisselen kan ook altijd met F11 of Alt+Enter.",
          "Window or fullscreen. F11 or Alt+Enter switch at any time."},
         Kind::Choice, 0, 0, 0, nullptr, "false", kWindowMode, Apply::Live, "NWII_FULLSCREEN", Status::Exists,
         "main.cpp (SDL_SetWindowFullscreen)", L19_REF(fullscreen), nullptr, 0},
        {"aspect", Tab::Beeld, kNoTab, L19_G("Scherm", "Display"), "graphics", "aspect",
         {"Beeldverhouding", "Aspect ratio"},
         {"Hoe het spelbeeld in het venster past. 'Automatisch' volgt het spel (16:9 of 4:3, zie 'Breedbeeld in "
          "het spel'), met zwarte balken waar nodig. 'Uitrekken' vult het hele venster.",
          "How the picture fits the window. Automatic follows the game (16:9 or 4:3); Stretch fills the window."},
         Kind::Choice, 0, 0, 0, nullptr, "\"auto\"", kAspect, Apply::Live, nullptr, Status::Exists, "main.cpp (RES9)",
         L19_REF(aspect), nullptr, 0},
        {"widescreen", Tab::Beeld, kNoTab, L19_G("Scherm", "Display"), "nand", "widescreen",
         {"Breedbeeld in het spel", "Game widescreen"},
         {"Of het spel zelf in 16:9 tekent (meer beeld links en rechts) of in 4:3, zoals de breedbeeldinstelling "
          "van een Wii. Geldt bij de volgende start van het spel.",
          "Whether the game itself renders 16:9 or 4:3 (Wii widescreen setting). Applies when the game starts."},
         Kind::Choice, 0, 0, 0, nullptr, "", kWide, Apply::AtBoot, nullptr, Status::ExistsHidden,
         "nand_fs.cpp ensure_system_files (IPL.AR)", L19_REF(widescreen), nullptr, 0},
        {"internal_scale", Tab::Beeld, kNoTab, L19_G("Scherm", "Display"), "graphics", "internal_scale",
         {"Interne resolutie", "Internal resolution"},
         {"Op welke resolutie het spel getekend wordt voordat het naar je scherm gaat. Hoger = scherper, maar "
          "zwaarder voor de videokaart. 'Automatisch' kiest 2x als je videokaart dat aankan.",
          "Resolution the game is rendered at. Higher is sharper but heavier. Automatic picks 2x when the GPU allows."},
         Kind::Choice, 0, 0, 0, nullptr, "\"auto\"", kScale, Apply::AtBoot, "NWII_INTERNAL_SCALE", Status::Exists,
         "main.cpp (EFB size at boot)", L19_REF(internal_scale), nullptr, 0},
        {"vsync", Tab::Beeld, kNoTab, L19_G("Beeldtempo", "Frame pacing"), "graphics", "vsync",
         {"VSync", "VSync"},
         {"Wacht met elk beeld op je scherm: geen 'scheuren' in het beeld. Uit = iets minder vertraging. Geldt na "
          "een herstart van SkyBreak.",
          "Waits for the display on every frame: no tearing. Off = a little less latency. Needs a restart."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Restart, "NWII_SWAP_INTERVAL", Status::Exists,
         "main.cpp (swap interval at start)", L19_REF(vsync), nullptr, 0},
        {"frame_interpolation", Tab::Beeld, Tab::Gameplay, L19_G("Beeldtempo", "Frame pacing"), "graphics", "frame_interpolation",
         {"60 fps (tussenbeelden)", "60 fps (interpolated, experimental)"},
         {"Het spel loopt op 30 beelden per seconde. Dit tekent tussen elk paar beelden een berekend tussenbeeld: "
          "vloeiend 60 fps. Kost ongeveer 17 ms extra vertraging en dubbel zoveel werk voor de videokaart; bij "
          "camerasprongen en laadschermen wordt het tussenbeeld overgeslagen. Werkt meteen.",
          "Adds computed frames for up to 60-fps presentation. Gameplay stays at 30 Hz. Uses more GPU time, "
          "adds latency and may show artifacts. Busy scenes may run below 60 fps. Applies immediately."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Live, "NWII_FRAME_INTERP", Status::Exists,
         "gx.cpp / renderer_gl.cpp (every frame)", L19_REF(frame_interpolation), nullptr, 0, L19_G("Experimenteel", "Experimental")},
        {"interp_2d", Tab::Beeld, kNoTab, L19_G("Beeldtempo", "Frame pacing"), "graphics", "frame_interpolation_2d",
         {"Ook HUD en tekst vloeiend", "Interpolate HUD and text too"},
         {"Bij 60 fps ook de HUD, tekstvakken en menu's tussen twee beelden in tekenen. Uit = die komen uit het "
          "nieuwste echte beeld (iets minder vloeiend, nooit een halve tekst).",
          "With 60 fps, also interpolate HUD, text boxes and menus. Off = 2D from the newest real frame."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, "NWII_INTERP_2D", Status::ExistsHidden,
         "gx.cpp (interp_config().interp_2d)", L19_REF(interp_2d),
         +[](const Settings& s, const Ctx& c) -> Avail {
             if (!s.frame_interpolation) return {true, false, {"Alleen met '60 fps (tussenbeelden)' aan.", "Only with 60 fps on."}};
             return Locked(c, "NWII_INTERP_2D");
         },
         kAdvanced},
        // MVGEN21: how the in-between frame is made (frame_motion.h / interp_live.h)
        {"interp_mode", Tab::Beeld, kNoTab, L19_G("Beeldtempo", "Frame pacing"), "graphics", "frame_interpolation_mode",
         {"Tussenbeelden maken", "In-between frames"},
         {"Interpolatie tekent scherpe tussenbeelden wanneer de processor en scene dit toelaten. Op drukke plekken "
          "kan het beeldtempo terugvallen. 'Vorig beeld herhalen' herhaalt alleen een frame om te vergelijken. Werkt meteen.",
          "Interpolation draws sharp in-between frames when the CPU and scene permit it. Busy scenes can fall back "
          "to a lower frame rate. Repeat only duplicates a frame for comparison. Applies immediately."},
         Kind::Choice, 0, 0, 0, nullptr, "\"interp\"", kInterpMode, Apply::Live, "", Status::New,
         "interp_live.h (interp_config().dup_mode, every frame)", L19_REF(interp_mode),
         +[](const Settings& s, const Ctx& c) -> Avail {
             if (!s.frame_interpolation) return {true, false, {"Alleen met '60 fps (tussenbeelden)' aan.", "Only with 60 fps on."}};
             return Locked(c, "NWII_INTERP_MODE");
         },
         kAdvanced},
        {"present_pacing", Tab::Beeld, kNoTab, L19_G("Beeldtempo", "Frame pacing"), "ui", "present_pacing",
         {"Beeldtempo", "Present pacing"},
         {"Hoe klaarstaande beelden naar het scherm gaan. 'Automatisch' gebruikt de Windows-compositor in een "
          "venster en gewone VSync op volledig scherm. Alleen veranderen als het beeld hapert.",
          "How finished frames reach the display. Automatic = compositor in a window, VSync in fullscreen."},
         Kind::Choice, 0, 0, 0, nullptr, "\"auto\"", kPacing, Apply::AtBoot, "NWII_PRESENT_PACING", Status::Exists,
         "main.cpp (env at boot)", L19_REF(present_pacing), nullptr, kAdvanced},
        {"hd_textures", Tab::Beeld, kNoTab, L19_G("Texturen", "Textures"), "graphics", "hd_textures",
         {"HD-texturen", "HD textures"},
         {"Vervangt de texturen van het spel door HD-versies uit een texturenpakket (Dolphin-namen; Dolphin-packs "
          "werken ook). Het pakket maak je zelf van je eigen schijf met SkyBreak Setup, of je zet een pakket in de "
          "map textures\\SOUE01. Zonder pakket verandert er niets.",
          "Replaces the game's textures with an HD pack (Dolphin names). Without a pack nothing changes."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::AtBoot, "NWII_HD_TEXTURES", Status::Exists,
         "hd_textures.cpp (at boot)", L19_REF(hd_textures),
         +[](const Settings&, const Ctx& c) -> Avail {
             if (!c.hd_pack_found) return {true, true, {"Geen texturenpakket gevonden: deze instelling doet nu niets.", "No pack found."}};
             return Locked(c, "NWII_HD_TEXTURES");
         },
         0},
        {"hd_preload", Tab::Beeld, kNoTab, L19_G("Texturen", "Textures"), "graphics", "hd_textures_preload",
         {"HD-texturen vooraf laden", "Preload HD textures"},
         {"Laadt het pakket op de achtergrond vooraf in het werkgeheugen (tot een kwart van je RAM, hoogstens 8 GB): "
          "minder texturen die pas even later scherp worden, wel meer geheugengebruik.",
          "Decodes the pack into RAM in the background (up to 1/4 of RAM, max 8 GB): less pop-in."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::AtBoot, "NWII_HDTEX_PRELOAD", Status::Exists,
         "hd_textures.cpp (at boot)", L19_REF(hd_preload),
         +[](const Settings& s, const Ctx& c) -> Avail {
             if (!s.hd_textures) return {true, false, {"Alleen met HD-texturen aan.", "Only with HD textures on."}};
             return Locked(c, "NWII_HDTEX_PRELOAD");
         },
         0},
        {"texture_pack_dir", Tab::Beeld, kNoTab, L19_G("Texturen", "Textures"), "graphics", "texture_pack_dir",
         {"Extra texturenmap", "Extra texture folder"},
         {"Een extra map met een texturenpakket, die SkyBreak als eerste doorzoekt. Leeg = alleen de standaardmap "
          "textures\\SOUE01 naast SkyBreak.",
          "An extra texture pack folder, searched first. Empty = only textures\\SOUE01."},
         Kind::Path, 0, 0, 0, nullptr, "\"\"", kNone, Apply::AtBoot, "NWII_TEXTURE_PACK_DIR", Status::ExistsHidden,
         "hd_textures.cpp (at boot)", L19_REF(texture_pack_dir), nullptr, kAdvanced},

        // ================================================================== GELUID (Dusklight: AUDIO)
        {"master_volume", Tab::Geluid, kNoTab, L19_G("Volume", "Volume"), "audio", "master_volume",
         {"Hoofdvolume", "Master volume"},
         {"Het totale volume van het spel, los van het Windows-volume. Muziek en effecten apart regelen kan niet: "
          "het spel mengt ze zelf.",
          "Overall game volume, separate from Windows. Music/effects cannot be split: the game mixes them itself."},
         Kind::Slider, 0, 100, 5, "%d%%", "100", kNone, Apply::Live, nullptr, Status::New,
         "audio_out.cpp (gain, new)", L19_REF(master_volume), nullptr, 0},
        {"speaker_volume", Tab::Geluid, kNoTab, L19_G("Volume", "Volume"), "audio", "remote_speaker_volume",
         {"Wii Remote-speaker", "Wii Remote speaker"},
         {"Geluiden die op de Wii uit de Wii Remote zelf komen (zwaard, voorwerpen). SkyBreak speelt ze af via je "
          "speakers. Bij 'uit' speelt het spel ze via de gewone mix.",
          "Sounds the Wii plays from the remote's speaker (sword, items), played through your speakers."},
         Kind::Slider, 0, 2, 0.05f, "%.0f%%", "0.8", kNone, Apply::AtBoot, "NWII_SPK_VOLUME", Status::Exists,
         "pad.cpp SPK11 (at boot)", L19_REF(speaker_volume), nullptr, kPercent},
        {"mute_unfocused", Tab::Geluid, kNoTab, L19_G("Volume", "Volume"), "audio", "mute_when_unfocused",
         {"Stil als SkyBreak niet actief is", "Mute when in background"},
         {"Zet het geluid uit zolang een ander venster actief is (bijvoorbeeld als je naar je browser gaat).",
          "Mutes the sound while another window is active."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Live, nullptr, Status::New, "audio_out.cpp (new)",
         L19_REF(mute_unfocused), nullptr, 0},
        {"audio_latency_ms", Tab::Geluid, kNoTab, L19_G("Uitvoer", "Output"), "ui", "audio_latency_ms",
         {"Audiovertraging", "Audio latency"},
         {"Meer vertraging = minder kans op haperend of krakend geluid; minder = geluid sneller bij het beeld.",
          "More latency = fewer crackles; less = sound closer to the picture."},
         Kind::Choice, 0, 0, 0, nullptr, "0", kLatency, Apply::AtBoot, "NWII_AUDIO_LATENCY_MS", Status::Exists,
         "audio_out.cpp (env at boot)", L19_REF(audio_latency_ms), nullptr, 0},
        {"audio_device", Tab::Geluid, kNoTab, L19_G("Uitvoer", "Output"), "audio", "device",
         {"Geluidsapparaat", "Audio device"},
         {"Naar welk apparaat het geluid gaat. 'Windows-standaard' volgt de instelling van Windows.",
          "Which device plays the sound. Windows default follows Windows."},
         Kind::Choice, 0, 0, 0, nullptr, "\"\"", kAudioDev, Apply::AtBoot, nullptr, Status::New,
         "audio_out.cpp (SDL_OpenAudioDevice name, new)", L19_REF(audio_device), nullptr, kAdvanced},

        // ================================================================== BESTURING (Dusklight: INPUT) - CTRL18-UI rows 1:1
        {"preset_gamepad", Tab::Besturing, kNoTab, L19_G("Indeling", "Layout"), "input", "preset_gamepad",
         {"Indeling controller", "Controller layout"},
         {"Hoe je controller het spel bestuurt. 'Switch HD (knoppen)' is de indeling van Skyward Sword HD zonder "
          "bewegingsbesturing: rechterstick = zwaard, ZR = voorwerp, L + rechterstick = camera. 'Klassiek' is de "
          "indeling van voor deze versie. 'Wii (beweging/gyro)' gebruikt de bewegingssensoren van een DualSense of "
          "Switch Pro als Wii Remote; de rechterstick bestuurt dan de camera. De knoppen staan op dezelfde plek als "
          "op een Switch: de rechter knop (Cirkel / B) is A.",
          "How your controller plays the game. Switch HD (buttons) is Skyward Sword HD's button-only layout."},
         Kind::Choice, 0, 0, 0, nullptr, "\"switch-hd\"", kPresetPad, Apply::Live, "NWII_INPUT_PRESET_GAMEPAD", Status::Exists,
         "CTRL18-INPUT (request_input_bindings_reload)", L19_REF(preset_gamepad), nullptr, 0},
        {"preset_keyboard", Tab::Besturing, kNoTab, L19_G("Indeling", "Layout"), "input", "preset_keyboard",
         {"Indeling toetsenbord + muis", "Keyboard + mouse layout"},
         {"'Toetsenbord + muis' is een moderne indeling: WASD lopen, muis = camera, linkermuisknop = zwaard (slepen = "
          "gerichte slag), rechtermuisknop = schild, E = actie, R = voorwerp, Esc = uitrusting. 'Klassiek' is de "
          "indeling van voor deze versie (muis = Wii-wijzer, numeriek toetsenblok = zwaardslagen).",
          "Keyboard + mouse: WASD, mouse camera, left click = sword. Classic = the old layout."},
         Kind::Choice, 0, 0, 0, nullptr, "\"kbm\"", kPresetKb, Apply::Live, "NWII_INPUT_PRESET_KEYBOARD", Status::Exists,
         "CTRL18-INPUT", L19_REF(preset_keyboard), nullptr, 0},
        {"bind_pad_hd", Tab::Besturing, kNoTab, L19_G("Indeling", "Layout"), "input.gamepad_hd", "<actie>",
         {"Knoppen aanpassen...", "Remap buttons..."},
         {"Opent de knoppentabel van de gekozen indeling: kies een actie, druk dan op de knop of toets die je wilt. "
          "Botsingen zie je meteen; 'Standaard' per regel of voor alles. Gezichtsknoppen worden op hun PLEK "
          "bewaard, zodat dezelfde instelling op elke controller werkt.",
          "Opens the remap table of the chosen layout: pick an action, then press the new button or key."},
         Kind::Bindings, 0, 0, 0, nullptr, "", kNone, Apply::Live, nullptr, Status::Exists, "CTRL18-UI ui_controls_tab.cpp",
         L19_NOREF, nullptr, 0},
        {"act_gyro_calibration", Tab::Besturing, kNoTab, L19_G("Indeling", "Layout"), nullptr, nullptr,
         {"Gyro kalibreren...", "Calibrate gyro..."},
         {"Meet stilstand, corrigeer drift en test je richtwijzer. Voor controllers met bewegingssensoren.",
          "Measure stillness, correct drift and test your pointer. For controllers with motion sensors."},
         Kind::Action, 0, 0, 0, nullptr, "", kNone, Apply::Live, nullptr, Status::Exists, "GYRO28", {}, nullptr, 0},
        {"act_test_controls", Tab::Besturing, kNoTab, L19_G("Indeling", "Layout"), nullptr, nullptr,
         {"Besturing testen...", "Test controls..."},
         {"Laat live zien wat je controller doet: sticks, knoppen, de herkende zwaardslag ('Slag rechts-onder', "
          "'Draaiaanval') en, in het spel, de situatie ('Vliegen', 'Richten', 'Wiel').",
          "Live view of sticks, buttons, the recognised sword move and the game context."},
         Kind::Action, 0, 0, 0, nullptr, "", kNone, Apply::None, nullptr, Status::Exists, "CTRL18-UI (ctrl18_live_status)",
         L19_NOREF, nullptr, 0},
        {"free_camera", Tab::Besturing, Tab::Gameplay, L19_G("Camera", "Camera"), "camera", "free_camera",
         {"Vrije camera", "Free camera"},
         {"Draai de camera vrij rond Link: met de controller L vasthouden + rechterstick, met de muis gewoon bewegen. "
          "Het spel doet zelf de botsingen met muren en het terugveren. Uit = precies de camera van de Wii. In de "
          "Wii-indeling (beweging) staat de vrije camera altijd uit.",
          "Turn the camera freely around Link (L + right stick, or the mouse). Off = the exact Wii camera."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, "NWII_FREECAM", Status::Exists, "CTRL18-CAM",
         L19_REF(free_camera),
         +[](const Settings& s, const Ctx& c) -> Avail {
             if (s.preset_gamepad == "wii-motion" && s.preset_keyboard != "kbm")
                 return {true, false, {"Staat altijd uit in de Wii-indeling (beweging).", "Always off in the Wii layout."}};
             return Locked(c, "NWII_FREECAM");
         },
         0, L19_G("Experimenteel", "Experimental")},
        {"camera_speed", Tab::Besturing, kNoTab, L19_G("Camera", "Camera"), "camera", "speed",
         {"Camerasnelheid", "Camera speed"},
         {"Hoe snel de vrije camera draait bij een volledig ingedrukte stick, in 5 stappen zoals in Skyward Sword HD "
          "(1 = rustig, 5 = snel).",
          "How fast the free camera turns at full stick, 5 steps like Skyward Sword HD."},
         Kind::Slider, 1, 5, 1, "%d", "3", kNone, Apply::Live, "NWII_CAM_SPEED", Status::Exists, "CTRL18-CAM",
         L19_REF(camera_speed),
         +[](const Settings& s, const Ctx&) -> Avail {
             if (!s.free_camera) return {true, false, {"Alleen met de vrije camera aan.", "Only with the free camera on."}};
             return {};
         },
         0},
        {"camera_invert_x", Tab::Besturing, kNoTab, L19_G("Camera", "Camera"), "camera", "invert_x",
         {"Camera horizontaal omkeren", "Invert camera X"}, {"Stick of muis naar rechts draait de camera naar links.", "Right turns the camera left."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Live, "NWII_CAM_INVERT", Status::Exists, "CTRL18-CAM",
         L19_REF(camera_invert_x), nullptr, 0},
        {"camera_invert_y", Tab::Besturing, kNoTab, L19_G("Camera", "Camera"), "camera", "invert_y",
         {"Camera verticaal omkeren", "Invert camera Y"}, {"Stick of muis omhoog laat de camera omlaag kijken.", "Up looks down."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Live, "NWII_CAM_INVERT", Status::Exists, "CTRL18-CAM",
         L19_REF(camera_invert_y), nullptr, 0},
        {"flight_invert_y", Tab::Besturing, kNoTab, L19_G("Camera", "Camera"), "camera", "flight_invert_y",
         {"Vliegen: omhoog/omlaag omkeren", "Flying: invert up/down"},
         {"Alleen voor de Loftwing: stick naar je toe = stijgen (zoals een vliegtuig). Staat los van de camera.",
          "Loftwing only: stick towards you = climb. Separate from the camera."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Live, nullptr, Status::Exists, "CTRL18-INPUT",
         L19_REF(flight_invert_y), nullptr, 0},
        {"auto_recentre_s", Tab::Besturing, kNoTab, L19_G("Camera", "Camera"), "camera", "auto_recentre_s",
         {"Camera vanzelf terug achter Link", "Auto re-centre camera"},
         {"Na zoveel seconden zonder camerabeweging draait de camera vanzelf terug achter Link. 0 = nooit.",
          "After this many seconds without camera input the camera returns behind Link. 0 = never."},
         Kind::Slider, 0, 10, 1, "%d s", "0", kNone, Apply::Live, "NWII_CAM_RECENTRE", Status::ExistsHidden, "CTRL18-CAM",
         L19_REF(auto_recentre_s), nullptr, kAdvanced},
        {"mouse_camera_sensitivity", Tab::Besturing, kNoTab, L19_G("Muis", "Mouse"), "mouse", "sensitivity_camera",
         {"Muisgevoeligheid camera", "Mouse camera sensitivity"},
         {"Hoeveel de camera draait per centimeter muisbeweging (1,00 = standaard).", "Camera turn per mouse distance."},
         Kind::Slider, 0.1f, 5.0f, 0.05f, "%.2f", "1.0", kNone, Apply::Live, nullptr, Status::Exists, "CTRL18-CAM",
         L19_REF(mouse_camera_sensitivity),
         +[](const Settings& s, const Ctx&) -> Avail { return {s.preset_keyboard == "kbm", true, {}}; }, 0},
        {"mouse_aim_sensitivity", Tab::Besturing, kNoTab, L19_G("Muis", "Mouse"), "mouse", "sensitivity_aim",
         {"Muisgevoeligheid richten", "Mouse aim sensitivity"},
         {"Hoe snel je richt met de muis als je een voorwerp vasthoudt of in eerste persoon kijkt (1,00 = standaard).",
          "Aim speed with the mouse (items, first person)."},
         Kind::Slider, 0.1f, 5.0f, 0.05f, "%.2f", "1.0", kNone, Apply::Live, nullptr, Status::Exists, "CTRL18-INPUT",
         L19_REF(mouse_aim_sensitivity),
         +[](const Settings& s, const Ctx&) -> Avail { return {s.preset_keyboard == "kbm", true, {}}; }, 0},
        {"mouse_invert_y", Tab::Besturing, kNoTab, L19_G("Muis", "Mouse"), "mouse", "invert_y",
         {"Richten met muis omkeren", "Invert mouse aim"}, {"Bij richten met de muis (katapult, boog, kever): muis omhoog laat Link omlaag richten, zoals in "
          "vliegsimulators. De camera-omkering staat apart.",
          "When aiming with the mouse (slingshot, bow, beetle): mouse up aims down. Camera invert is separate."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Live, nullptr, Status::ExistsHidden, "CTRL18-INPUT",
         L19_REF(mouse_invert_y),
         +[](const Settings& s, const Ctx&) -> Avail { return {s.preset_keyboard == "kbm", true, {}}; }, kAdvanced},
        {"rumble", Tab::Besturing, kNoTab, L19_G("Controller", "Controller"), "input", "rumble",
         {"Trillen", "Rumble"},
         {"Hoe sterk de controller trilt als het spel de Wii Remote laat trillen (zwaardslagen, schild, schatten). "
          "0 = uit. Toetsenbord en muis trillen nooit.",
          "Controller rumble strength when the game rumbles the Wii Remote. 0 = off."},
         Kind::Slider, 0, 100, 5, "%d%%", "100", kNone, Apply::Live, "NWII_RUMBLE", Status::Exists, "CTRL18-CAM",
         L19_REF(rumble), nullptr, 0},
        {"gyro_aim", Tab::Besturing, kNoTab, L19_G("Controller", "Controller"), "input", "gyro_aim",
         {"Richten met gyro", "Gyro aiming"},
         {"Alleen in 'Switch HD (knoppen)' met een DualSense, DUALSHOCK 4 of Switch Pro: als je richt (boog, katapult, "
          "eerste persoon), menu's bedient of een grote sleutel draait, beweegt de controller mee. Zwaardslagen blijven "
          "op de rechterstick.",
          "Switch HD with a gyro pad: motion aims items and menu pointers, and rotates boss keys. Sword swings stay on the right stick."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Live, nullptr, Status::Exists, "CTRL18-INPUT",
         L19_REF(gyro_aim),
         +[](const Settings& s, const Ctx& c) -> Avail {
             if (s.preset_gamepad != "switch-hd") return {true, false, {"Alleen in de indeling 'Switch HD (knoppen)'.", "Switch HD layout only."}};
             if (!c.gyro_pad) return {true, true, {"Er is nu geen controller met gyro aangesloten.", "No gyro controller connected."}};
             return {};
         },
         0},
        {"gyro_player_space", Tab::Besturing, kNoTab, L19_G("Controller", "Controller"), "input", "gyro_player_space",
         {"Mikken volgt je houding", "Gravity-relative aiming"},
         {"Gyro-mikken houdt rekening met hoe je de controller kantelt. Alleen Switch HD-mikken en menu's; draaipuzzels behouden alle drie assen.",
          "Gyro aim follows your grip using measured gravity. Switch HD aiming and menus only; puzzles retain all three axes."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Live, nullptr, Status::Exists, "GYRO28",
         L19_REF(gyro_player_space), nullptr, 0},
        {"glyph_set", Tab::Besturing, Tab::Interface, L19_G("Controller", "Controller"), "graphics", "glyph_set",
         {"Knopiconen", "Button icons"},
         {"Welke knopplaatjes het spel toont (HUD, uitleg in teksten, menu's). 'Automatisch' volgt het apparaat dat je "
          "het laatst gebruikte en de indeling die daarbij hoort. Plaatjes: Kenney 'Input Prompts' (CC0). "
          "'Wii (origineel)' toont de Wii Remote-knoppen van het spel.",
          "Which button pictures the game shows. Automatic follows the last used device."},
         Kind::Choice, 0, 0, 0, nullptr, "\"auto\"", kGlyphs, Apply::Live, "NWII_GLYPH_SET", Status::Exists,
         "CTRL18-UI (hd_textures.cpp HdTexSetGlyphSet)", L19_REF(glyph_set), nullptr, 0, L19_G("In het spel", "In game")},
        // Klassiek / Wii (motion) / sticks - today in the 'Geavanceerd' collapsing header of the Besturing tab
        {"gyro", Tab::Besturing, kNoTab, L19_G("Klassiek en sticks", "Classic and sticks"), "input", "gamepad_motion",
         {"Gyro als Wii Remote (Klassiek)", "Gyro as Wii Remote (Classic)"},
         {"Aan: de bewegingssensoren van je controller sturen de MotionPlus-remote. In het spel ook: touchpad of "
          "Create+Options 1 s vasthouden.",
          "The controller's motion sensors drive the MotionPlus remote (Classic layout)."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kGyroLits, Apply::Live, nullptr, Status::Exists, "gamepad_source.cpp",
         L19_REF(gyro),
         +[](const Settings& s, const Ctx&) -> Avail { return {true, s.preset_gamepad == "classic", {"Alleen in de indeling 'Klassiek'.", "Classic only."}}; },
         kAdvanced},
        {"grip_tilt", Tab::Besturing, kNoTab, L19_G("Klassiek en sticks", "Classic and sticks"), "input", "gyro_grip_tilt",
         {"Greephoek", "Grip angle"}, {"Hoeveel graden je de controller kantelt als je hem 'recht' vasthoudt.", "Degrees of tilt in your neutral grip."},
         Kind::Slider, -30, 30, 1, "%.0f graden", "0.0", kNone, Apply::AtBoot, nullptr, Status::Exists, "gamepad_source.cpp",
         L19_REF(grip_tilt), nullptr, kAdvanced},
        {"mouse_sensitivity", Tab::Besturing, kNoTab, L19_G("Klassiek en sticks", "Classic and sticks"), "input", "mouse_sensitivity",
         {"Muisgevoeligheid (Klassiek)", "Mouse sensitivity (Classic)"}, {"Shift + muis draaien in de klassieke indeling.", "Shift + mouse turn (Classic)."},
         Kind::Slider, 0.1f, 5.0f, 0.05f, "%.2f", "1.0", kNone, Apply::AtBoot, nullptr, Status::Exists, "mouse_keyboard_source.cpp",
         L19_REF(mouse_sensitivity),
         +[](const Settings& s, const Ctx&) -> Avail { return {s.preset_keyboard == "classic", true, {}}; }, kAdvanced},
        {"aim_stick_speed", Tab::Besturing, kNoTab, L19_G("Klassiek en sticks", "Classic and sticks"), "input", "aim_stick_speed",
         {"Richtsnelheid rechterstick", "Right stick aim speed"}, {"Hoe snel de rechterstick richt en de menuwijzer beweegt.", "Right-stick aim / pointer speed."},
         Kind::Slider, 0.1f, 2.0f, 0.05f, "%.2f", "1.0", kNone, Apply::AtBoot, nullptr, Status::Exists, "gamepad_source.cpp",
         L19_REF(aim_stick_speed), nullptr, kAdvanced},
        {"left_deadzone", Tab::Besturing, kNoTab, L19_G("Klassiek en sticks", "Classic and sticks"), "input", "left_stick_deadzone",
         {"Dode zone linkerstick", "Left stick deadzone"}, {"Kleine stickbewegingen rond het midden tellen niet (tegen 'drift').", "Ignores small movements (drift)."},
         Kind::Slider, 0, 0.9f, 0.01f, "%.2f", "", kNone, Apply::AtBoot, nullptr, Status::Exists, "input_bindings.h",
         L19_REF(left_deadzone), nullptr, kAdvanced},
        {"right_deadzone", Tab::Besturing, kNoTab, L19_G("Klassiek en sticks", "Classic and sticks"), "input", "right_stick_deadzone",
         {"Dode zone rechterstick", "Right stick deadzone"}, {"Idem voor de rechterstick (zwaard, camera).", "Same for the right stick."},
         Kind::Slider, 0, 0.9f, 0.01f, "%.2f", "", kNone, Apply::AtBoot, nullptr, Status::Exists, "input_bindings.h",
         L19_REF(right_deadzone), nullptr, kAdvanced},
        {"trigger_threshold", Tab::Besturing, kNoTab, L19_G("Klassiek en sticks", "Classic and sticks"), "input", "trigger_threshold",
         {"Drempel ZL/ZR", "Trigger threshold"}, {"Hoe ver je een trekker indrukt voordat hij telt.", "How far a trigger must travel."},
         Kind::Slider, 0.05f, 0.95f, 0.05f, "%.2f", "", kNone, Apply::AtBoot, nullptr, Status::ExistsHidden, "input_bindings.h",
         L19_REF(trigger_threshold), nullptr, kAdvanced},
        {"gyro_deadband", Tab::Besturing, kNoTab, L19_G("Gyro", "Gyro"), "input", "gyro_deadband",
         {"Gyro rustdrempel", "Gyro idle threshold"},
         {"Negeert kleine sensorruis. Kalibreer voor nauwkeurig richten.", "Ignores small sensor offsets in every motion mode. Guided calibration reduces the threshold for finer aiming."},
         Kind::Slider, 0, 0.05f, 0.001f, "%.3f rad/s", "0.02", kNone, Apply::Live, nullptr, Status::Exists, "gamepad_source.cpp",
         L19_REF(gyro_deadband), nullptr, 0},

        // ================================================================== GAMEPLAY
        {"a_does_not_dash", Tab::Gameplay, kNoTab, L19_G("Experimenteel", "Experimental"), "input", "a_does_not_dash",
         {"A laat Link niet sprinten", "A does not sprint"},
         {"Zoals in Skyward Sword HD: sprinten doe je met B (controller) of Shift (toetsenbord), zodat je niet per "
          "ongeluk gaat rennen als je iets wilt oppakken.",
          "Like HD: sprint with B / Shift, so A never makes Link run by accident."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, nullptr, Status::ExistsHidden, "CTRL18-INPUT translator",
         L19_REF(a_does_not_dash), nullptr, 0},
        {"quick_items", Tab::Gameplay, kNoTab, L19_G("Experimenteel", "Experimental"), "input", "quick_items",
         {"Snelkeuze voorwerpen (1-8)", "Quick items (1-8)"},
         {"Met de toetsen 1 tot en met 8 kies je direct een vak van het voorwerpenwiel, zonder het wiel te openen.",
          "Keys 1-8 pick an item-wheel slot directly."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, nullptr, Status::ExistsHidden, "CTRL18-INPUT KbmTranslator",
         L19_REF(quick_items), nullptr, 0},
        {"fire_on_release", Tab::Gameplay, kNoTab, L19_G("Experimenteel", "Experimental"), "input", "fire_on_release",
         {"Zwaardslag bij loslaten stick", "Swing on stick release"},
         {"Uit: de slag gaat zodra de stick de rand raakt (zoals HD). Aan: pas als je de stick loslaat, zodat je de "
          "richting nog kunt bijsturen.",
          "Off: swing when the stick hits the rim (HD). On: swing when you let go."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Live, nullptr, Status::ExistsHidden, "CTRL18-INPUT stick_sword.h",
         L19_REF(fire_on_release), nullptr, 0},
        {"pause_unfocused", Tab::Gameplay, kNoTab, L19_G("Comfort", "Comfort"), "game", "pause_when_unfocused",
         {"Pauzeren als SkyBreak niet actief is", "Pause in background"},
         {"Zet het spel stil zolang een ander venster actief is, en gaat verder als je terugkomt. Het spel merkt er "
          "niets van (ook de klok van het spel staat stil).",
          "Freezes the game while another window is active; resumes when you come back."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Live, nullptr, Status::New,
         "main.cpp host loop (guest pause, proof run needed)", L19_REF(pause_unfocused), nullptr, kExperimental},

        // ================================================================== INTERFACE
        {"ui_scale", Tab::Interface, kNoTab, L19_G("Weergave", "Appearance"), "ui", "scale",
         {"Grootte van de interface", "Interface size"},
         {"Hoe groot de launcher, het F1-menu en de meldingen zijn. 'Automatisch' past zich aan het venster aan "
          "(100% bij 1280 x 720).",
          "Size of the launcher, F1 menu and notices. Automatic follows the window (100% at 1280x720)."},
         Kind::Choice, 0, 0, 0, nullptr, "0", kUiScale, Apply::Live, nullptr, Status::New, "ui_app.cpp UiScale",
         L19_REF(ui_scale), nullptr, 0},
        {"accent", Tab::Interface, kNoTab, L19_G("Weergave", "Appearance"), "ui", "accent",
         {"Accentkleur", "Accent colour"},
         {"De kleur van de focusrand, schuifjes en kopjes. Goud past bij het spel, rood bij het SkyBreak-logo.",
          "Colour of the focus outline, sliders and headers."},
         Kind::Choice, 0, 0, 0, nullptr, "\"gold\"", kAccent, Apply::Live, nullptr, Status::New, "ui_app.cpp ApplyStyle",
         L19_REF(accent), nullptr, 0},
        {"game_font", Tab::Interface, kNoTab, L19_G("Weergave", "Appearance"), "ui", "game_font",
         {"Lettertype van het spel", "Game font for titles"},
         {"Titels en menu's in het lettertype van Skyward Sword (van je eigen schijf). Uit = Segoe UI.",
          "Titles in Skyward Sword's own font (from your disc). Off = Segoe UI."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, nullptr, Status::ExistsHidden, "ui_app.cpp BuildFonts",
         L19_REF(game_font), nullptr, 0},
        {"confirm_button", Tab::Interface, kNoTab, L19_G("Weergave", "Appearance"), "ui", "confirm_button",
         {"Bevestigen in menu's", "Confirm button in menus"},
         {"Met welke knop je in de menu's van SkyBreak kiest. Standaard zoals op de Switch: de rechter knop "
          "(Cirkel / Xbox B) kiest, de onderste (Kruis / Xbox A) gaat terug. Het spel zelf volgt je "
          "besturingsindeling.",
          "Which face button confirms in SkyBreak's menus. Default like Switch: the right button."},
         Kind::Choice, 0, 0, 0, nullptr, "\"east\"", kConfirm, Apply::Live, nullptr, Status::New,
         "ui_app.cpp (io.ConfigNavSwapGamepadButtons)", L19_REF(confirm_button), nullptr, 0},
        // THPVID19 owns the keys + texts (THPVID19-design.md 5.4 BackgroundVideoSettings()); LAUNCH19 shows them.
        {"background_video", Tab::Interface, kNoTab, L19_G("Launcher", "Launcher"), "ui", "background_video",
         {"Achtergrondvideo", "Background video"},
         {"Speelt de demofilm van je eigen schijf (THP/Demo01_02.thp) in een lus achter het startscherm. Uit: "
          "\xC3\xA9\xC3\xA9n stilstaand beeld uit dezelfde film. De video draait op een eigen thread (ongeveer 2% van "
          "\xC3\xA9\xC3\xA9n processorkern), pauzeert als het venster geminimaliseerd is en stopt zodra het spel start.",
          "Plays the demo movie from your own disc (THP/Demo01_02.thp) in a loop behind the start screen. Off: one "
          "still frame of the same movie. Stops as soon as the game starts."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, "NWII_UI_VIDEO", Status::New,
         "ui_thp_player.cpp (THPVID19)", L19_REF(background_video), nullptr, kLauncherOnly},
        {"background_video_file", Tab::Interface, kNoTab, L19_G("Launcher", "Launcher"), "ui", "background_video_file",
         {"Welke film", "Which movie"},
         {"De film achter het startscherm. Alle films komen van je eigen schijf.",
          "The movie behind the start screen (from your own disc)."},
         Kind::Choice, 0, 0, 0, nullptr, "\"\"", kBgVideo, Apply::Live, nullptr, Status::New,
         "ui_thp_player.cpp (THPVID19)", L19_REF(background_video_file),
         +[](const Settings& s, const Ctx&) -> Avail {
             if (!s.background_video) return {true, false, {"Alleen met de achtergrondvideo aan.", "Only with the video on."}};
             return {};
         },
         kLauncherOnly | kAdvanced},
        {"background_music", Tab::Interface, Tab::Geluid, L19_G("Launcher", "Launcher"), "ui", "background_music",
         {"Muziek in het startscherm", "Start screen music"},
         {"De muziek van de demofilm. Zwijgt als het venster niet actief of geminimaliseerd is, en stopt zodra het "
          "spel start. Heeft geen invloed op het geluid in het spel.",
          "The demo movie's music. Silent while the window is inactive or minimised; stops when the game starts. "
          "Does not affect the game's sound."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, nullptr, Status::New,
         "ui_thp_player.cpp (THPVID19)", L19_REF(background_music),
         +[](const Settings& s, const Ctx&) -> Avail {
             if (!s.background_video) return {true, false, {"Alleen met de achtergrondvideo aan.", "Only with the video on."}};
             return {};
         },
         kLauncherOnly, L19_G("Startscherm", "Start screen")},
        {"background_music_volume", Tab::Interface, Tab::Geluid, L19_G("Launcher", "Launcher"), "ui",
         "background_music_volume",
         {"Volume muziek startscherm", "Start screen music volume"},
         {"Hoe hard de muziek van het startscherm klinkt. Het geluid in het spel stel je apart in.",
          "How loud the start screen music is. The game's sound is set separately."},
         Kind::Slider, 0, 100, 5, "%d%%", "60", kNone, Apply::Live, nullptr, Status::New,
         "ui_thp_player.cpp (THPVID19)", L19_REF(background_music_volume),
         +[](const Settings& s, const Ctx&) -> Avail {
             if (!s.background_video || !s.background_music)
                 return {true, false, {"Alleen met de achtergrondvideo en de muziek aan.", "Only with video and music on."}};
             return {};
         },
         kLauncherOnly, L19_G("Startscherm", "Start screen")},
        {"launcher", Tab::Interface, kNoTab, L19_G("Launcher", "Launcher"), "ui", "launcher",
         {"Launcher tonen bij het starten", "Show launcher at start"},
         {"Uit = SkyBreak start meteen het spel. De launcher zet je terug in het F1-menu (INSTELLINGEN > INTERFACE) "
          "of met launcher = true in instellingen.toml.", // REVLAUNCH19: there is no Shift-at-start override
          "Off = start the game directly. Turn it back on in the F1 menu (SETTINGS > INTERFACE) or with "
          "launcher = true in instellingen.toml."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, "NWII_NO_LAUNCHER", Status::Exists, "ui_app.cpp Start",
         L19_REF(launcher), nullptr, 0},
        {"discord_presence", Tab::Interface, kNoTab, L19_G("Discord", "Discord"), "discord", "enabled",
         {"Discord Rich Presence", "Discord Rich Presence"},
         {"Toont SkyBreak, je sessietijd en optioneel je gebied op je Discord-profiel. Discord moet op deze pc draaien. Uit wist de activiteit.",
          "Shows SkyBreak, session time and optionally your area on your Discord profile. Discord must run on this PC. Off clears the activity."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, "NWII_DISCORD", Status::New,
         "discord_presence.cpp local IPC worker", L19_REF(discord_presence), nullptr, 0},
        {"discord_show_location", Tab::Interface, kNoTab, L19_G("Discord", "Discord"), "discord", "show_location",
         {"Gebied tonen in Discord", "Show area in Discord"},
         {"Toont herkende gebiedsnamen; geen savebestanden, spelersnamen of pc-gegevens. Uit toont alleen dat je speelt. Updates kunnen 15 seconden duren.",
          "Shows recognised area names; no saves, player names or PC details. Off shows only that you are playing. Updates can take 15 seconds."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, nullptr, Status::New,
         "discord_presence.cpp MakeActivity", L19_REF(discord_show_location), nullptr, 0},
        {"remember_window", Tab::Interface, kNoTab, L19_G("Launcher", "Launcher"), "ui", "remember_window",
         {"Venster onthouden", "Remember window"},
         {"Bewaart grootte, plek en volledig scherm van het venster bij het afsluiten en gebruikt die de volgende keer.",
          "Keeps the window's size, position and fullscreen state for next time."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, nullptr, Status::New, "ui_app.cpp TrackWindowSize",
         L19_REF(remember_window), nullptr, 0},
        {"perf_overlay", Tab::Interface, kNoTab, L19_G("In het spel", "In game"), "ui", "perf_overlay",
         {"Prestatie-overlay", "Performance overlay"},
         {"Kleine teller rechtsboven in beeld: beelden per seconde, en desgewenst de beeldtijd, het tempo van het "
          "spel zelf en of de tussenbeelden aan staan. Handig om instellingen te vergelijken.", // REVLAUNCH19
          "Small counter top right: FPS, optionally frame time, the game's own rate and in-between frames on/off."},
         Kind::Choice, 0, 0, 0, nullptr, "\"off\"", kPerf, Apply::Live, nullptr, Status::New, "ui_app.cpp ComposeFrame",
         L19_REF(perf_overlay), nullptr, 0},
        {"toasts", Tab::Interface, kNoTab, L19_G("In het spel", "In game"), "ui", "toasts",
         {"Meldingen over controllers", "Controller notices"},
         {"Een korte melding als een controller wordt aangesloten of losgekoppeld.", "A short notice when a controller connects."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::Live, nullptr, Status::Exists, "ui_app.cpp PushToast",
         L19_REF(toasts), nullptr, 0},

        // ================================================================== GEAVANCEERD
        {"settings_file", Tab::Geavanceerd, kNoTab, L19_G("Bestanden", "Files"), "ui", "settings_file",
         {"Instellingenbestand", "Settings file"},
         {"Al deze instellingen staan in dit bestand (instellingen.toml). Je mag het met de hand aanpassen; "
          "SkyBreak bewaart je opmerkingen. Is het bestand kapot, dan zie je hier waar, en bij opslaan maakt "
          "SkyBreak eerst een kopie (.bak).",
          "All settings live in this file (instellingen.toml). Hand edits and comments are kept."},
         Kind::Info, 0, 0, 0, nullptr, "\"\"", kNone, Apply::None, "NWII_UI_SETTINGS_FILE", Status::Exists,
         "ui_settings.cpp UserSettingsPath", L19_NOREF, nullptr, 0},
        {"screenshot_dir", Tab::Geavanceerd, kNoTab, L19_G("Bestanden", "Files"), "graphics", "screenshot_dir",
         {"Map voor screenshots (F12)", "Screenshot folder (F12)"},
         {"Waar F12 zijn screenshots bewaart. Leeg = de map screenshots naast SkyBreak.", "Where F12 saves screenshots."},
         Kind::Path, 0, 0, 0, nullptr, "\"\"", kNone, Apply::AtBoot, nullptr, Status::ExistsHidden, "main.cpp (F12)",
         L19_REF(screenshot_dir), nullptr, 0},
        {"act_open_logs", Tab::Geavanceerd, kNoTab, L19_G("Bestanden", "Files"), nullptr, nullptr,
         {"Logmap openen", "Open log folder"}, {"Opent de map met de logbestanden van SkyBreak.", "Opens the log folder."},
         Kind::Action, 0, 0, 0, nullptr, "", kNone, Apply::None, nullptr, Status::New, "ui_app.cpp OpenFolder", L19_NOREF, nullptr, 0},
        {"texture_dump", Tab::Geavanceerd, kNoTab, L19_G("Bestanden", "Files"), "graphics", "texture_dump",
         {"Texturen dumpen (voor makers van packs)", "Dump textures (pack makers)"},
         {"Schrijft elke textuur die het spel laadt als PNG naar dump\\textures\\SOUE01, met dezelfde namen als "
          "Dolphin. Kost schijfruimte; zet het daarna weer uit.",
          "Writes every loaded texture as PNG (Dolphin names). Uses disk space."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::AtBoot, "NWII_TEX_DUMP", Status::Exists, "hd_textures.cpp",
         L19_REF(texture_dump), nullptr, 0},
        {"crash_report", Tab::Geavanceerd, kNoTab, L19_G("Diagnose", "Diagnostics"), "diag", "crash_report",
         {"Crashrapport", "Crash report"},
         {"Schrijft bij een vastloper of crash extra gegevens (welke functie, welke thread) naar het logbestand. "
          "Handig als je een fout meldt; kost niets zolang er niets misgaat.",
          "Writes extra data on a crash into the log. Free until something goes wrong."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::AtBoot, "NWII_CRASH_REPORT", Status::EnvOnly,
         "ui_crashreport.h / gx.cpp", L19_REF(crash_report), nullptr, 0},
        {"freeze_watchdog", Tab::Geavanceerd, kNoTab, L19_G("Diagnose", "Diagnostics"), "diag", "freeze_watchdog",
         {"Bevriezingen herkennen", "Freeze detector"},
         {"Merkt het als het spel langer dan een paar seconden niet reageert en schrijft dan een rapport. "
          "Standaard aan.",
          "Notices when the game stops responding and writes a report. On by default."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::AtBoot, "NWII_FREEZE_WATCHDOG", Status::EnvOnly,
         "freeze_watchdog.cpp", L19_REF(freeze_watchdog), nullptr, 0},
        {"act_diag_zip", Tab::Geavanceerd, kNoTab, L19_G("Diagnose", "Diagnostics"), nullptr, nullptr,
         {"Diagnosepakket maken...", "Create diagnostics package..."},
         {"Maakt een zip met de logbestanden, je instellingen en gegevens over je pc (Windows, videokaart, "
          "controllers) om mee te sturen bij een foutmelding. Er gaat niets van het spel of je save in.",
          "Zips logs, settings and system info for a bug report. No game data or saves."},
         Kind::Action, 0, 0, 0, nullptr, "", kNone, Apply::None, nullptr, Status::New, "ui_diag.cpp (new)", L19_NOREF, nullptr, 0},
        {"directinput", Tab::Geavanceerd, kNoTab, L19_G("Diagnose", "Diagnostics"), "input", "directinput",
         {"Oude DirectInput-controllers", "Legacy DirectInput pads"},
         {"Alleen nodig voor heel oude of exotische controllers die Windows niet als gewone gamepad ziet. Kan dubbele "
          "controllers geven.",
          "Only for old/exotic DirectInput controllers. May show pads twice."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::Restart, "NWII_DIRECTINPUT", Status::EnvOnly,
         "main.cpp (SDL hint at start)", L19_REF(directinput), nullptr, kAdvanced},
        {"clock", Tab::Geavanceerd, kNoTab, L19_G("Diagnose", "Diagnostics"), "game", "clock",
         {"Klok van het spel", "Game clock"},
         {"Het spel krijgt de tijd van Windows (voor de datum bij je save). 'Vast' zet de klok op 1 januari 2000, "
          "zoals bij tests.",
          "The game gets the Windows time. Fixed = 1 January 2000 (tests)."},
         Kind::Choice, 0, 0, 0, nullptr, "\"host\"", kClock, Apply::AtBoot, "NWII_RTC", Status::EnvOnly,
         "main.cpp guest_clock", L19_REF(clock), nullptr, kAdvanced},
        {"color_screenlinear", Tab::Geavanceerd, kNoTab, L19_G("Weergave (experimenteel)", "Rendering (experimental)"), "graphics", "color_screenlinear",
         {"Kleuren zoals Dolphin-software", "Dolphin software colours"},
         {"Kleurverloop over driehoeken zoals de Dolphin-softwarerenderer (en exacte clipping). In de meeste sc\xC3\xA8nes "
          "net iets echter, in sommige slechter; kost ongeveer 9% meer werk.",
          "Screen-linear colours like Dolphin's software renderer. Mostly closer, sometimes worse; +9% GPU."},
         Kind::Toggle, 0, 0, 0, nullptr, "false", kNone, Apply::AtBoot, "NWII_COLOR_SCREENLINEAR", Status::EnvOnly,
         "tev_shader_gen.cpp / renderer_gl.cpp", L19_REF(color_screenlinear), nullptr, kExperimental},
        {"copy_filter", Tab::Geavanceerd, kNoTab, L19_G("Weergave (experimenteel)", "Rendering (experimental)"), "graphics", "copy_filter",
         {"Vervagingsfilter van de Wii", "Wii copy blur filter"},
         {"Het spel vervaagt sommige schermkopieen (gloed, onscherpte) met een filter van de Wii. Uit = scherper, maar "
          "anders dan bedoeld (zoals 'Disable Copy Filter' in Dolphin).",
          "The game blurs some screen copies with the Wii filter. Off = sharper (Dolphin's Disable Copy Filter)."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::AtBoot, "NWII_COPY_FILTER", Status::EnvOnly,
         "renderer_gl.cpp CurrentCopyFilter", L19_REF(copy_filter), nullptr, kExperimental},
        {"overlay", Tab::Geavanceerd, kNoTab, L19_G("Herstellen", "Reset"), "ui", "overlay",
         {"F1-menu in het spel", "In-game F1 menu"},
         {"Uit = F1 en de menuknop van je controller doen niets meer in het spel. Terugzetten kan dan alleen hier in "
          "de launcher of in instellingen.toml.",
          "Off = F1 and the controller menu button do nothing in game."},
         Kind::Toggle, 0, 0, 0, nullptr, "true", kNone, Apply::AtBoot, nullptr, Status::ExistsHidden, "ui_capture.h",
         L19_REF(overlay), nullptr, kDanger},
        {"act_reset_all", Tab::Geavanceerd, kNoTab, L19_G("Herstellen", "Reset"), nullptr, nullptr,
         {"Alle instellingen terugzetten...", "Reset all settings..."},
         {"Zet alles op de standaardwaarden. Je huidige instellingenbestand gaat eerst naar een kopie "
          "(instellingen.toml.bak). Je save en je knoppen-indelingen blijven bewaard tenzij je dat aanvinkt.",
          "Everything back to defaults; the current file is kept as .bak first. Saves are untouched."},
         Kind::Action, 0, 0, 0, nullptr, "", kNone, Apply::None, nullptr, Status::New, "ui_settings.cpp", L19_NOREF, nullptr, kDanger},
    };
    return d;
}

inline const Desc* Find(std::string_view id) {
    for (const Desc& s : All())
        if (id == s.id) return &s;
    return nullptr;
}
inline const char* T(const Text& t, bool en) { return en && t.en ? t.en : t.nl; }
inline constexpr Text kTabNames[] = {{"START", "START"},     {"BEELD", "GRAPHICS"},   {"GELUID", "AUDIO"},
                                     {"BESTURING", "CONTROLS"}, {"GAMEPLAY", "GAMEPLAY"}, {"INTERFACE", "INTERFACE"},
                                     {"GEAVANCEERD", "ADVANCED"}};

// ------------------------------------------------------------------------------- generic values (LAUNCH19 impl)
// One implementation for the TOML write-back (ui_settings.cpp DiffEdits/AllEdits/NeedsRestart), the reset to defaults
// and the settings page (value text, choice cycling). The unit test compares it with the old hand-written edits_for.
inline bool IsValueKind(Kind k) { return k == Kind::Toggle || k == Kind::Slider || k == Kind::Choice || k == Kind::Path; }
inline bool IsValueRow(const Desc& d) { return d.ref && d.section && d.key && IsValueKind(d.kind); }
// REVLAUNCH19: may "Standaard" (row), reset tab and reset all change this row? (kNoReset: the save folder)
inline bool Resettable(const Desc& d) { return IsValueRow(d) && !(d.flags & kNoReset); }
inline Ref RefOf(const Desc& d, const Settings& s) { return d.ref ? d.ref(const_cast<Settings&>(s)) : Ref{}; }
inline std::string Unquote(std::string_view v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return std::string(v.substr(1, v.size() - 2));
    return std::string(v);
}
inline std::string QuoteToml(const std::string& v) { // TOML basic string (same escaping as ui_settings.cpp toml_str)
    std::string o = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}
inline std::string FloatToml(float v) { // = ui_settings.cpp fnum: byte-compatible with the UI13 write-back
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}
// Index of the choice the field holds now (-1 = a value outside the list, e.g. a hand-written glyph set folder).
inline int ChoiceIndex(const Desc& d, const Settings& s) {
    const Ref r = RefOf(d, s);
    for (size_t i = 0; i < d.choices.size(); ++i) {
        const Choice& c = d.choices[i];
        if (auto* p = std::get_if<int*>(&r); p && **p == c.ival) return int(i);
        if (auto* p = std::get_if<bool*>(&r); p && (**p ? 1 : 0) == c.ival) return int(i);
        if (auto* p = std::get_if<std::string*>(&r); p && **p == Unquote(c.toml ? c.toml : "")) return int(i);
    }
    return -1;
}
inline void SetChoice(const Desc& d, Settings& s, int i) {
    if (i < 0 || i >= int(d.choices.size())) return;
    const Choice& c = d.choices[size_t(i)];
    const Ref r = d.ref ? d.ref(s) : Ref{};
    if (auto* p = std::get_if<int*>(&r)) **p = c.ival;
    else if (auto* p = std::get_if<bool*>(&r)) **p = c.ival != 0;
    else if (auto* p = std::get_if<std::string*>(&r)) **p = Unquote(c.toml ? c.toml : "");
}
// Same value (floats with the UI13 tolerance 0.0005).
inline bool SameValue(const Desc& d, const Settings& a, const Settings& b) {
    const Ref ra = RefOf(d, a), rb = RefOf(d, b);
    if (auto* p = std::get_if<bool*>(&ra)) return **p == **std::get_if<bool*>(&rb);
    if (auto* p = std::get_if<int*>(&ra)) return **p == **std::get_if<int*>(&rb);
    if (auto* p = std::get_if<float*>(&ra)) return std::fabs(**p - **std::get_if<float*>(&rb)) < 0.0005f;
    if (auto* p = std::get_if<std::string*>(&ra)) return **p == **std::get_if<std::string*>(&rb);
    return true;
}
// The TOML literal the row writes for `s`. "" = remove the key (the runtime default applies again). *skip = nothing
// to write (a float slider whose field holds the built-in sentinel < 0 and whose key is absent).
inline std::string Literal(const Desc& d, const Settings& s, bool* skip = nullptr) {
    if (skip) *skip = false;
    const Ref r = RefOf(d, s);
    if (d.kind == Kind::Choice || (d.kind == Kind::Toggle && d.choices.size() == 2)) {
        const int i = ChoiceIndex(d, s);
        if (i >= 0) return d.choices[size_t(i)].toml ? d.choices[size_t(i)].toml : "";
    }
    if (auto* p = std::get_if<bool*>(&r)) return **p ? "true" : "false";
    if (auto* p = std::get_if<int*>(&r)) return std::to_string(**p);
    if (auto* p = std::get_if<float*>(&r)) {
        if (**p < 0 && d.def && !*d.def) { // built-in default (deadzones, trigger threshold): no TOML form
            if (skip) *skip = true;
            return "";
        }
        return FloatToml(**p);
    }
    if (auto* p = std::get_if<std::string*>(&r)) return QuoteToml(**p);
    if (skip) *skip = true;
    return "";
}
// ui_settings.cpp: the write-back and the restart rules of a table (production: All(); the unit test also runs them on
// mutated copies as negative controls).
std::vector<runtime::SettingEdit> EditsFromTable(std::span<const Desc> table, const Settings& a, const Settings* before);
RestartInfo RestartFromTable(std::span<const Desc> table, const Settings& a, const Settings& b, bool in_game, bool en);

// The row's default into `s` (reset row / tab / all).
inline void ApplyDefault(const Desc& d, Settings& s) {
    if (!IsValueRow(d)) return;
    const std::string def = d.def ? d.def : "";
    const Ref r = d.ref(s);
    if (d.kind == Kind::Choice) {
        for (size_t i = 0; i < d.choices.size(); ++i)
            if (d.choices[i].toml && def == d.choices[i].toml) { SetChoice(d, s, int(i)); return; }
    }
    if (auto* p = std::get_if<bool*>(&r)) **p = def == "true";
    else if (auto* p = std::get_if<int*>(&r)) **p = def.empty() ? 0 : std::atoi(def.c_str());
    else if (auto* p = std::get_if<float*>(&r)) **p = def.empty() ? -1.0f : float(std::atof(def.c_str()));
    else if (auto* p = std::get_if<std::string*>(&r)) **p = Unquote(def);
}

} // namespace nwii::ui::catalog
