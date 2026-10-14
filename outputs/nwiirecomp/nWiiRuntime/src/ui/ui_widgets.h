#pragma once
// UIT14: small ImGui helpers of the launcher / in-game menu, header-only so outputs/pc-tests/uit14 drives them in a
// headless ImGui context.
//
// MenuHitArea: the hit area of a big menu entry (launcher SPELEN / INSTELLINGEN / AFSLUITEN and the in-game menu).
// Dear ImGui 1.91 leaves an InvisibleButton out of keyboard/gamepad navigation unless ImGuiButtonFlags_EnableNav
// is passed: before UIT14 the arrows, Enter, the stick and A did nothing there, so a controller-only player could
// not start the game from the launcher.
#include "imgui.h"
#include "imgui_internal.h" // NavWindow (NavInputWithoutFocus)

namespace nwii::ui {

// `show_nav`: set by the caller when it moved the keyboard focus to the menu (SetKeyboardFocusHere). Once this
// entry holds the focus the nav cursor is made visible, so Enter / A confirm it at once; ImGui otherwise ignores
// Enter and A until an arrow key or the stick has been used (the focus request keeps the cursor hidden).
// `selected` (optional, REVUIT14): this entry is the one the menu shows as selected. Since the entries hold the
// keyboard/controller focus (UIT14), a mouse resting on another entry lit up two entries (the focused one and the
// hovered one) and Enter / A confirmed the focused one, not the one under the mouse. Now a mouse that MOVES onto an
// entry takes the focus there (keys and stick go on from it), and the selected entry is the focused one; only
// while no item of the menu holds the focus (after a click beside the panel) does the hovered entry light up.
inline bool MenuHitArea(const char* id, const ImVec2& size, bool* show_nav, bool* selected = nullptr) {
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    if (show_nav && *show_nav && ImGui::IsItemFocused()) {
        ImGui::SetNavCursorVisible(true);
        *show_nav = false;
    }
    const ImGuiIO& io = ImGui::GetIO(); // REVUIT14 (see `selected`)
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && !ImGui::IsItemFocused()) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true); // Enter / A confirm the entry under the mouse
    }
    if (selected) {
        const ImGuiContext& c = *ImGui::GetCurrentContext();
        const bool menu_focused = c.NavId != 0 && c.NavWindow == ImGui::GetCurrentWindow();
        *selected = ImGui::IsItemFocused() || (hovered && !menu_focused);
    }
    return pressed;
}

// The in-game menu sees input only while it is open, so the release of a key or button that was still down when it
// closed (B, Enter or A closing it) never reached ImGui, which then still held it at the next opening: with ImGui's
// key repeat a "held" B closed the menu again ~0.3 s after every later opening (UIT14). Every opening therefore
// starts from a clean input state (no context = nothing to do).
inline void ResetMenuInput() {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
}

// UIT14: a mouse click beside the menu panel (on the launcher backdrop or the dimmed game picture: draw lists, not
// windows) makes ImGui drop the window focus (FocusWindow(NULL): NavWindow = 0, NavId = 0). Before UIT14 the arrows,
// Enter, the stick and A then did nothing until the mouse clicked inside the panel again or the menu was reopened.
// True when such a navigation input arrives while no window holds the focus; the caller then focuses its menu again.
// That press only brings the (visible) cursor back on the first entry, like ImGui's first arrow press; it does not
// also confirm anything.
inline bool NavInputWithoutFocus() {
    const ImGuiContext* c = ImGui::GetCurrentContext();
    if (!c || c->NavWindow) return false;
    static const ImGuiKey kNavKeys[] = {
        ImGuiKey_UpArrow,         ImGuiKey_DownArrow,         ImGuiKey_LeftArrow,         ImGuiKey_RightArrow,
        ImGuiKey_Enter,           ImGuiKey_KeypadEnter,       ImGuiKey_Space,             ImGuiKey_Tab,
        ImGuiKey_GamepadDpadUp,   ImGuiKey_GamepadDpadDown,   ImGuiKey_GamepadDpadLeft,   ImGuiKey_GamepadDpadRight,
        ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadLStickUp,   ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickLeft,
        ImGuiKey_GamepadLStickRight};
    for (const ImGuiKey k : kNavKeys)
        if (ImGui::IsKeyPressed(k, false)) return true;
    return false;
}

} // namespace nwii::ui
