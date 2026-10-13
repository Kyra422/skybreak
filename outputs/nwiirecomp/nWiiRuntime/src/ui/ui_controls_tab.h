#pragma once
// CTRL18-UI (T8): the "Besturing" settings tab (launcher + in-game F1 menu), the rebind capture, the live test panel
// and the "last used device" -> glyph set switch. Kept out of ui_app.cpp so a later launcher redesign can host it
// anywhere: ui_app.cpp passes its looks/helpers in ControlsTabHost and forwards SDL events.
//
//   RenderControlsTab   the tab body (ImGui, inside the settings page child)
//   ControlsEvent       every SDL event of the launcher loop / in-game event loop, before the menu routing:
//                       true = consumed (a rebind capture took it). Also feeds the live test panel and the
//                       "last used device" tracker (glyph set "auto").
//   ApplyGlyphSet       pick the glyph set folder for the saved settings + last used device (HdTexSetGlyphSet)
//   ControlsQuery       autotest queries ("capture", "bind.<table>.<action>", "glyph_active", "last_device", ...)
#include <SDL.h>

#include <functional>
#include <string>

#include "ui_settings.h"

struct ImFont;

namespace nwii::ui {

struct ControlsTabHost {
    ImFont* f_small = nullptr;
    ImFont* f_bold = nullptr;
    float s = 1.0f;          // UI scale
    bool in_game = false;    // in-game F1 menu (live status of the game) vs launcher
    bool selftest = false;   // NWII_UI_SELFTEST: open every collapsible part
    std::function<void(const std::string&)> track;      // autotest click target = the last item
    std::function<void(const char*)> section, hint;     // ui_app.cpp looks
};

void RenderGyroCalibration(Settings& edit,const ControlsTabHost& host);
void RenderControlsTab(Settings& edit, const Settings& saved, const ControlsTabHost& host);
bool ControlsEvent(const SDL_Event& e, Settings& edit, const Settings& saved, bool* binding_changed = nullptr);
bool ControlsCapturing();
void ControlsCancelCapture();
void ApplyGlyphSet(const Settings& saved);
std::string ControlsQuery(const std::string& q, const Settings& edit, const Settings& saved); // "?" = unknown
// One-time hint (gyro pad connected while gyro aiming is off): returns the toast text once, then "".
std::string ControlsTakeGyroHint(const Settings& saved);

} // namespace nwii::ui
