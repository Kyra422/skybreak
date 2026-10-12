#pragma once
// UI13: input capture for the in-game menu. Header-only, testable without a window.
//  - RouteEvent: which SDL events open/close the menu and which the menu takes while it is open.
//    The game itself polls SDL state (input sources), so "taking" an event only keeps it away
//    from main.cpp's own key handling (F11/F12 stay with main.cpp, window/quit events too).
//  - CaptureSource: an input source registered after all others; while the menu is open it
//    replaces the merged frame with a neutral one (no buttons, sticks or motion; the pointer and
//    accelerometer stay where they were when the menu opened), so nothing reaches the virtual
//    Wii Remote / Nunchuk / GameCube pads. Connection state (err) is kept: no "remote lost" screen.
#include <SDL.h>

#include <atomic>

#include "input/input_source.h"

namespace nwii::ui {

enum class Route {
    Pass,   // not for the menu: main.cpp handles it as before
    Toggle, // opens/closes the menu (consumed)
    Menu,   // the open menu takes it (consumed)
    Both,   // the menu sees it and main.cpp still handles it
};

// Menu keys: F1 (not bound by default in [input.keyboard]); controller: Misc1 = the
// DualSense microphone button / Xbox Series Share / Switch Capture (no Wii mapping uses it).
// CTRL18-UI (user decision): Esc is the GAME's gear/pause menu (CTRL18-INPUT binds it to Wii 1), so Esc no
// longer OPENS the port menu; while the menu is open Esc still closes it (or goes back one page).
inline bool IsMenuKey(SDL_Keycode k) { return k == SDLK_F1; }
inline bool IsMenuCloseKey(SDL_Keycode k) { return k == SDLK_ESCAPE; } // CTRL18-UI
inline constexpr int kMenuPadButton = SDL_CONTROLLER_BUTTON_MISC1;

// RES9 fullscreen keys: F11 and Alt+Enter (main.cpp and the launcher toggle on these).
inline bool IsFullscreenKey(const SDL_Keysym& k) {
    return k.sym == SDLK_F11 || ((k.sym == SDLK_RETURN || k.sym == SDLK_KP_ENTER) && (k.mod & KMOD_ALT) != 0);
}

inline Route RouteEvent(const SDL_Event& e, bool menu_open) {
    switch (e.type) {
    case SDL_KEYDOWN:
        if (IsMenuKey(e.key.keysym.sym)) return e.key.repeat ? (menu_open ? Route::Menu : Route::Pass) : Route::Toggle;
        if (IsMenuCloseKey(e.key.keysym.sym)) // CTRL18-UI: closed menu -> the game (gear menu)
            return menu_open ? (e.key.repeat ? Route::Menu : Route::Toggle) : Route::Pass;
        // UIT14: Alt+Enter is fullscreen for main.cpp, never menu input (the menu took it: no toggle, and the
        // Enter activated the focused entry, e.g. VERDER SPELEN). Its key-up still reaches the menu below.
        if (e.key.keysym.sym != SDLK_F11 && IsFullscreenKey(e.key.keysym)) return Route::Pass;
        if (e.key.keysym.sym == SDLK_F11 || e.key.keysym.sym == SDLK_F12) return menu_open ? Route::Both : Route::Pass;
        return menu_open ? Route::Menu : Route::Pass;
    case SDL_KEYUP:
        if (e.key.keysym.sym == SDLK_F11 || e.key.keysym.sym == SDLK_F12) return menu_open ? Route::Both : Route::Pass;
        return menu_open ? Route::Menu : Route::Pass;
    case SDL_TEXTINPUT:
    case SDL_TEXTEDITING:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
        return menu_open ? Route::Menu : Route::Pass;
    case SDL_CONTROLLERBUTTONDOWN:
        if (e.cbutton.button == kMenuPadButton) return Route::Toggle;
        return menu_open ? Route::Menu : Route::Pass;
    case SDL_CONTROLLERBUTTONUP:
    case SDL_CONTROLLERAXISMOTION:
        return menu_open ? Route::Menu : Route::Pass;
    case SDL_CONTROLLERDEVICEADDED:
    case SDL_CONTROLLERDEVICEREMOVED:
        return Route::Both; // toasts + the menu's controller list; main.cpp ignores them anyway
    default:
        return Route::Pass; // SDL_QUIT, window events, audio/joystick-level events, ...
    }
}

// Neutral frame while the menu is open. `held` = the pointer/accelerometer of the last frame
// before the menu opened.
struct HeldPose {
    float ir_x[4] = {0.5f, 0.5f, 0.5f, 0.5f}, ir_y[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float ax[4] = {0, 0, 0, 0}, ay[4] = {0, 0, 0, 0}, az[4] = {0, 0, 0, 0};
    float nax[4] = {0, 0, 0, 0}, nay[4] = {0, 0, 0, 0}, naz[4] = {1, 1, 1, 1};
};
inline void RecordPose(const runtime::input::WiimoteState motes[4], HeldPose& h) {
    for (int i = 0; i < 4; ++i) {
        h.ir_x[i] = motes[i].ir_x; h.ir_y[i] = motes[i].ir_y;
        h.ax[i] = motes[i].accel_x; h.ay[i] = motes[i].accel_y; h.az[i] = motes[i].accel_z;
        h.nax[i] = motes[i].nunchuk.accel_x; h.nay[i] = motes[i].nunchuk.accel_y; h.naz[i] = motes[i].nunchuk.accel_z;
    }
}
inline void NeutralizeFrame(runtime::input::GameCubePadState pads[4], runtime::input::WiimoteState motes[4],
                            const HeldPose& h) {
    for (int i = 0; i < 4; ++i) {
        auto& p = pads[i];
        p.buttons = 0;
        p.stick_x = p.stick_y = p.substick_x = p.substick_y = 0;
        p.trigger_l = p.trigger_r = p.analog_a = p.analog_b = 0;
        auto& m = motes[i];
        m.buttons = 0;
        m.ir_x = h.ir_x[i]; m.ir_y = h.ir_y[i];
        m.accel_x = h.ax[i]; m.accel_y = h.ay[i]; m.accel_z = h.az[i];
        m.gyro_pitch = m.gyro_yaw = m.gyro_roll = 0.0f;
        m.nunchuk.buttons = 0;
        m.nunchuk.stick_x = m.nunchuk.stick_y = 0.0f;
        m.nunchuk.accel_x = h.nax[i]; m.nunchuk.accel_y = h.nay[i]; m.nunchuk.accel_z = h.naz[i];
        m.nunchuk.accel_speed = 0.0f;
        // Keep the frame's sequence/timestamp (InputManager owns them); no rotation.
        m.motion.gyro_x_rad_s = m.motion.gyro_y_rad_s = m.motion.gyro_z_rad_s = 0.0f;
    }
}

inline bool AnyButtonHeld(const runtime::input::GameCubePadState pads[4], const runtime::input::WiimoteState motes[4]) {
    for (int i = 0; i < 4; ++i)
        if (pads[i].buttons || motes[i].buttons || motes[i].nunchuk.buttons) return true;
    return false;
}

// Release guard: the key/button that closed the menu (Enter = Wii +, controller A = Wii A) is
// usually still down when the menu closes; the game only gets input again once every button is
// up (or after kReleaseGuardNs, so a stuck key cannot block it).
inline constexpr uint64_t kReleaseGuardNs = 1'500'000'000ull;

class CaptureSource : public runtime::input::IInputSource {
public:
    explicit CaptureSource(const std::atomic<bool>* open) : open_(open) {}
    void update(runtime::input::GameCubePadState pads[4], runtime::input::WiimoteState motes[4]) override {
        const bool open = open_ && open_->load(std::memory_order_relaxed);
        const uint64_t now = motes[0].motion.monotonic_ns;
        if (open) {
            was_open_ = true;
            NeutralizeFrame(pads, motes, held_);
            return;
        }
        if (was_open_) { // just closed
            was_open_ = false;
            guard_until_ = now + kReleaseGuardNs;
            guarding_ = true;
        }
        if (guarding_) {
            if (AnyButtonHeld(pads, motes) && now < guard_until_) {
                NeutralizeFrame(pads, motes, held_);
                return;
            }
            guarding_ = false;
        }
        RecordPose(motes, held_);
    }

private:
    const std::atomic<bool>* open_;
    HeldPose held_;
    bool was_open_ = false, guarding_ = false;
    uint64_t guard_until_ = 0;
};

} // namespace nwii::ui
