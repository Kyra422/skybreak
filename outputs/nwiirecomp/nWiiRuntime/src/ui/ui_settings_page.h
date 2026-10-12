#pragma once
// LAUNCH19: the SkyBreak settings page (design: work/claude-notes/LAUNCH19-design.md s2): Dusklight-style tabs
// START / BEELD / GELUID / BESTURING / GAMEPLAY / INTERFACE / GEAVANCEERD, a list of rows on the left (from the ONE
// settings catalog, ui_settings_catalog.h) and a description pane on the right, a gold focus outline, restart chips,
// "unsaved" dots, the unsaved-changes / reset / message dialogs, and CTRL18-UI's controls tab as a sub page.
//
// The same page is the launcher's INSTELLINGEN and the in-game F1 menu's INSTELLINGEN (kLauncherOnly rows hidden,
// AtBoot rows get "na herstart"). ui_app.cpp owns the window, fonts, the Settings copies and what saving / applying /
// folder picking / save import do; it passes them in Host. Input: one own focus model (no ImGui nav in the list):
//   row up/down      D-pad / left stick / arrows (Home/End, PgUp/PgDn, L2/R2 = page)
//   change value     left/right (Shift = 5 steps); confirm cycles a choice, flips a toggle, opens a path/action
//   confirm / back   BY POSITION: east / south (Switch layout, user decision CTRL18; [ui] confirm_button = "south"
//                    swaps them); keyboard Enter/Space and Esc/Backspace
//   default          west / Delete / right mouse button        save   north / Ctrl+S
//   tabs             L1 / R1, Q / E, click                       play   Start (Options) / F5 (in game: resume)
//   reset tab        Select (Create) / Ctrl+Delete               mouse  hover moves the focus when the mouse MOVES
// UIT14 autotest: every row is a click target by its label, its editor as "<label>::value" (choice chevrons
// "<label>::prev" / "::next"), tabs by their name, footer and dialog buttons by their text; Query() answers
// tab / focus / dirty / restart_count / dialog / sub / avail.<id> / row.<id>.
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "ui_controls_tab.h"
#include "ui_settings.h"
#include "ui_settings_catalog.h"

namespace nwii::ui::page {

struct Fonts {
    ImFont *body = nullptr, *bold = nullptr, *small = nullptr, *head = nullptr, *caps = nullptr, *tab = nullptr,
           *menu = nullptr, *word = nullptr;
};

// Glyph style of the footer hints = the last used device (CTRL18-UI tracker): 0 keyboard, 1 Xbox, 2 PlayStation,
// 3 Switch.
enum class Pad : int { Keyboard = 0, Xbox = 1, PlayStation = 2, Switch = 3 };

enum class Dialog { None, Unsaved, ResetTab, ResetAll, Message, Restart, Danger };
enum class Result { None, Back, Play };

struct State {
    int tab = 0;
    std::string focus[int(catalog::Tab::Count)];
    float scroll[int(catalog::Tab::Count)] = {};
    bool scroll_to_focus = true;
    bool sub_calibration = false;
    bool sub_controls = false;     // CTRL18-UI's controls tab as a sub page
    Dialog dialog = Dialog::None;
    int dlg_sel = 0;               // focused dialog button
    bool dlg_binds = false;        // reset all: also the button layouts
    int pending = 0;               // after the unsaved dialog: 1 leave the page, 2 play / resume
    std::string msg_title, msg_body;
    bool msg_error = false;
    std::string danger_id;         // a kDanger toggle waiting for "Ja"
    std::string status;            // footer status line (save result), shown for a few seconds
    bool status_error = false;
    double status_t = -100.0;
    bool dragging = false;         // slider drag (mouse)
    std::string drag_id;
    void Open(bool keep_tab = true); // fresh state when the page opens (dialog/sub page closed)
};

struct Host {
    Fonts f;
    float s = 1.0f;                // UI scale
    bool en = false;               // [ui] language = en
    ImU32 accent = IM_COL32(236, 200, 120, 255);
    bool in_game = false;
    bool selftest = false;         // NWII_UI_SELFTEST: no real folder dialogs, deterministic texts
    double now = 0.0;              // seconds (status timeout)
    Pad pad = Pad::Keyboard;
    bool confirm_south = false;    // [ui] confirm_button = "south"
    Settings* edit = nullptr;
    const Settings* saved = nullptr;
    const Settings* boot = nullptr;     // what the running game booted with (AtBoot rows: "na herstart" in game)
    const Settings* boot_app = nullptr; // what this SkyBreak started with (Restart rows: window, VSync, threads)
    catalog::Ctx ctx;
    // facts shown in rows / the description pane
    std::string game_dir, nand_root, settings_path, settings_problem, renderer, log_dir;
    bool disc_done = false, disc_ok = false;
    std::string disc_message, disc_detail;
    std::vector<std::string> disc_checks; // "sys/boot.bin: SOUE01, revisie 0", ... (check mark per line)
    std::vector<bool> disc_checks_ok;
    std::vector<std::string> audio_devices; // names for [audio] device (beyond "Windows-standaard")
    std::string save_info;                  // "Save gevonden ... laatste reservekopie ..." (START, saves rows)
    std::string version_line;               // "SkyBreak 0.19 - build ..." (GEAVANCEERD description of the info rows)
    ImTextureID wing = 0, disc_logo = 0;
    ImVec2 wing_sz, disc_logo_sz;
    // what the page asks the host to do
    std::function<void(const std::string& label, ImVec2 a, ImVec2 b)> track; // autotest click target
    std::function<void(const Settings& edit)> preview;   // a value changed: apply the live rows now (not saved)
    std::function<bool(std::string& status)> save;        // write the file + apply; false = failed (status = why)
    std::function<void()> revert;                         // "Niet opslaan": edit = saved, live rows re-applied
    std::function<void(const std::string& path)> open_folder;
    std::function<std::string(const std::string& title, const std::string& start)> pick_folder; // "" = cancelled
    // actions: "act_import_dolphin", "act_export_dolphin", "act_diag_zip", "restart_now", "reset_all_backup".
    // Returns a message for the message dialog ("" = none); *error marks it as a problem.
    std::function<std::string(const std::string& id, std::string& title, bool& error)> action;
    ControlsTabHost controls;
};

// Draws the page into the area a..b (launcher window or the in-game picture rectangle) inside its own ImGui window.
Result Draw(State& st, Host& h, ImVec2 a, ImVec2 b);
// Number of rows whose value differs from saved / that need a restart (footer, dialogs, autotest).
int DirtyCount(const Host& h);
int RestartCount(const Host& h);
bool NeedsRestartChip(const catalog::Desc& d, const Host& h);
// Autotest queries; "?" = not a page query.
std::string Query(const State& st, const Host& h, const std::string& q);
// Raw value of a catalog row for the autotest ("1"/"0", numbers, strings).
std::string RawValue(const catalog::Desc& d, const Settings& s);

// ---- shared drawing (the main screen in ui_app.cpp uses these too)
ImU32 AccentHi(ImU32 accent);
// SkyBreak identity: the wing + "SkyBreak" in the game font + the tagline (PC port ..., built from your own disc).
void DrawBrand(ImDrawList* dl, const Host& h, ImVec2 p, float wing_h, float word_size, bool tagline);
// A face button by POSITION (0 south, 1 east, 2 west, 3 north) in the style of `pad`; keyboard draws a key cap.
void DrawPadGlyph(ImDrawList* dl, ImFont* f, ImVec2 c, float r, Pad pad, int position);
// A glyph + label hint; returns its width. `key` = keyboard key text when pad == Keyboard.
float DrawHint(ImDrawList* dl, const Host& h, ImVec2 p, float hgt, int position, const char* key, const char* label);
float HintWidth(const Host& h, float hgt, int position, const char* key, const char* label);
// The play/confirm/back key texts for the current device and confirm setting.
int ConfirmPos(const Host& h);
int BackPos(const Host& h);

} // namespace nwii::ui::page
