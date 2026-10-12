#include "mouse_keyboard_source.h"
#include "../virtual_motion.h"
#include "../motion_gesture.h"
#include "../input_bindings.h"
#include "runtime/config.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nwii::runtime::input {

// CTRL18-INPUT: a key is down, a mouse button is pressed or the mouse moved: keyboard/mouse drive Wii Remote 1.
static bool keyboard_or_mouse_used(int mx, int my, int last_mx, int last_my, uint32_t mouse_state) {
    if (mouse_state) return true;
    if (last_mx >= 0 && std::abs(mx - last_mx) + std::abs(my - last_my) > 2) return true;
    int n = 0;
    const Uint8* k = SDL_GetKeyboardState(&n);
    for (int i = 0; k && i < n; ++i)
        if (k[i]) return true;
    return false;
}

void MouseKeyboardSource::update(GameCubePadState pads[4], WiimoteState motes[4]) {
    // CTRL18-INPUT: preset swap / rebind requests from the UI are applied here, on the InputManager thread; the
    // "kbm" preset has its own path, "classic" continues below unchanged.
    apply_pending_input_bindings_reload();
    if (last_key_preset != int(input_bindings().key_preset)) {
        if (last_key_preset >= 0) {
            kbm.reset();
            if (kbm_captured || owns_relative_mouse) SDL_SetRelativeMouseMode(SDL_FALSE);
            kbm_captured = owns_relative_mouse = false;
            std::printf("[Input] Keyboard preset \"%s\"\n", key_preset_name(input_bindings().key_preset));
            std::fflush(stdout);
        }
        last_key_preset = int(input_bindings().key_preset);
    }
    if (input_bindings().key_preset == KeyPreset::Kbm) {
        update_kbm(pads, motes);
        return;
    }
    // INPUT10: Wii Remote / Nunchuk keys come from input_bindings.h ([input.keyboard]; the defaults are the
    // fixed keys this source had before). The GameCube pad keys below stay fixed.
    const InputBindings& bind = input_bindings();
    int mx, my;
    uint32_t mouse_state = SDL_GetMouseState(&mx, &my);
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    const bool focused = SDL_GetKeyboardFocus() != nullptr;
    const bool motion_modifier = focused && key_action_held(kKeyAimRelative, keys, mouse_state);
    if (motion_modifier && !owns_relative_mouse) {
        owns_relative_mouse = SDL_SetRelativeMouseMode(SDL_TRUE) == 0;
    } else if (!motion_modifier && owns_relative_mouse) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        owns_relative_mouse = false;
    }
    // Drain motion even without Shift, after focus loss, and after a producer
    // stall. Only relative movement while the explicit modifier is held is a
    // relative turn; without it the absolute mouse position may aim a MotionPlus pointer.
    int relative_x = 0, relative_y = 0;
    SDL_GetRelativeMouseState(&relative_x, &relative_y);
    const float dt = motes[0].motion.dt_seconds;
    int w = nwii::runtime::Config::get().window_width;
    int h = nwii::runtime::Config::get().window_height;
    if (SDL_Window* win = SDL_GetMouseFocus() ? SDL_GetMouseFocus() : SDL_GetKeyboardFocus())
        SDL_GetWindowSize(win, &w, &h); // the window is resizable/fullscreen (RES9)
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    {
        // Map the mouse onto the game picture (RES9 black bars), back into 0..w / 0..h units.
        const float rx = g_present_rect[0].load(), ry = g_present_rect[1].load();
        const float rw = g_present_rect[2].load(), rh = g_present_rect[3].load();
        if (rw > 0.0f && rh > 0.0f) {
            mx = int(std::clamp((float(mx) / w - rx) / rw, 0.0f, 1.0f) * w);
            my = int(std::clamp((float(my) / h - ry) / rh, 0.0f, 1.0f) * h);
        }
    }
    // A game adapter reports when this remote drives a MotionPlus pointer (SS).
    const bool motionplus_pointer = g_guest_motionplus_pointer[0].load();
    // The mouse only aims after it is actually used (moved > 2 px or a button pressed);
    // otherwise a resting mouse would pull the pointer away from a gamepad player.
    if (focused && last_mx >= 0 &&
        (std::abs(mx - last_mx) + std::abs(my - last_my) > 2 ||
         (mouse_state & (SDL_BUTTON(SDL_BUTTON_LEFT) | SDL_BUTTON(SDL_BUTTON_RIGHT))) != 0))
        g_aim_owner.store(0);
    if (focused && keyboard_or_mouse_used(mx, my, last_mx, last_my, mouse_state))
        g_active_device[0].store(int(ActiveDevice::KeyboardMouse)); // CTRL18-INPUT (rumble routing, camera_stick.h)
    last_mx = mx;
    last_my = my;
    const bool mouse_aims = g_aim_owner.load() == 0;
    const bool point_center = g_guest_point_center.load();
    float pitch = 0.0f, yaw = 0.0f, level_roll = 0.0f; // SOAK14: level_roll from rates_toward
    if (point_center && focused && dt > 0.0f && dt <= 0.1f) {
        // "Point at the screen" step: hold the virtual remote level at the screen centre so
        // the game centres its MotionPlus pointer on the identity orientation (the mouse
        // mapping below assumes that reference).
        double target[3];
        pointer_direction(0.0, 0.0, target);
        float rates[3];
        g_virtual_orientation[0].rates_toward(target, dt, kPointerMotionMaxRateRadS, rates);
        pitch = rates[0];
        yaw = rates[1];
        level_roll = rates[2]; // SOAK14
    } else if (motion_modifier && dt > 0.0f && dt <= 0.1f) {
        // Shift: free relative turn. Guest axes (virtual_motion.h): +pitch = nose
        // down, +yaw = turn left; mouse right turns right, mouse down tilts down.
        const float radians_per_pixel = 0.008f * bind.mouse_sensitivity; // [input] mouse_sensitivity
        pitch = std::clamp(relative_y * radians_per_pixel / dt, -kPointerMotionMaxRateRadS, kPointerMotionMaxRateRadS);
        yaw = std::clamp(-relative_x * radians_per_pixel / dt, -kPointerMotionMaxRateRadS, kPointerMotionMaxRateRadS);
    } else if (motionplus_pointer && mouse_aims && focused && dt > 0.0f && dt <= 0.1f) {
        // Plain mouse with a MotionPlus pointer: turn the virtual remote (gyro
        // rates only) so that the game's own pointer lands under the mouse,
        // assuming the game centred its pointer with the remote aimed at the
        // screen centre (virtual_motion.h pointer_direction).
        double target[3];
        // SOAK14: closed loop (virtual_motion.h PointerAimLoop): the game's pointer, not our model's, under the mouse.
        g_pointer_aim[0].aim(std::clamp(2.0 * mx / w - 1.0, -1.0, 1.0),
                             std::clamp(2.0 * my / h - 1.0, -1.0, 1.0), target);
        float rates[3];
        g_virtual_orientation[0].rates_toward(target, dt, kPointerMotionMaxRateRadS, rates);
        pitch = rates[0];
        yaw = rates[1];
        level_roll = rates[2]; // SOAK14
    }
    float roll = (float(key_action_held(kKeyRollRight, keys, mouse_state)) -
                        float(key_action_held(kKeyRollLeft, keys, mouse_state))) * 4.0f;
    if (roll == 0.0f) roll = level_roll; // SOAK14: an explicit roll key wins
    merge_virtual_motion(motes[0].motion, focused, pitch, yaw, roll);

    motes[0].err = 0;
    if (point_center) {
        // The point-at-screen step needs a visible sensor bar: centred, valid IR (as the boot
        // replay reports), consistent with the level remote steered above.
        motes[0].ir_x = 0.5f;
        motes[0].ir_y = 0.5f;
    } else if (!motion_modifier) {
        // With a MotionPlus pointer the virtual sensor bar is reported as not
        // visible: a valid IR position at the mouse would make the original
        // KMPLS DPD revise pull the remote toward a different direction
        // (outputs/pc-tests/pointer-mpls T7). Otherwise the mouse is the IR pointer.
        motes[0].ir_x = motionplus_pointer ? -1.0f : (float)mx / (float)w;
        motes[0].ir_y = motionplus_pointer ? -1.0f : (float)my / (float)h;
    }

    sword_controls(keys, focused, mouse_state, mx, my, dt);

    // Wii Remote buttons (defaults: LMB A, RMB B, Return +, Backspace -, Home, 1, 2, arrows = D-pad).
    // Alt+Enter toggles fullscreen (RES9) and must not also press the Wii + button (key_combo_held).
    static const std::pair<KeyAction, uint32_t> kWiiButtons[] = {
        {kKeyA, 0x0800}, {kKeyB, 0x0400}, {kKeyPlus, 0x0010}, {kKeyMinus, 0x1000}, {kKeyHome, 0x8000},
        {kKeyOne, 0x0200}, {kKeyTwo, 0x0100}, {kKeyUp, 0x0008}, {kKeyDown, 0x0004}, {kKeyLeft, 0x0001},
        {kKeyRight, 0x0002}};
    for (const auto& [action, mask] : kWiiButtons)
        if (key_action_held(action, keys, mouse_state)) motes[0].buttons |= mask;
    const bool alt = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];

    NunchukState nunchuk;
    nunchuk.available = true;
    nunchuk.stick_x = float(key_action_held(kKeyStickRight, keys, mouse_state)) -
                      float(key_action_held(kKeyStickLeft, keys, mouse_state));
    nunchuk.stick_y = float(key_action_held(kKeyStickUp, keys, mouse_state)) -
                      float(key_action_held(kKeyStickDown, keys, mouse_state));
    const float length = std::sqrt(nunchuk.stick_x * nunchuk.stick_x +
                                   nunchuk.stick_y * nunchuk.stick_y);
    if (length > 1.0f) {
        nunchuk.stick_x /= length;
        nunchuk.stick_y /= length;
    }
    if (key_action_held(kKeyC, keys, mouse_state)) nunchuk.buttons |= 0x4000;
    if (key_action_held(kKeyZ, keys, mouse_state)) nunchuk.buttons |= 0x2000;
    merge_nunchuk(motes[0].nunchuk, nunchuk);

    pads[0].err = 0;
    if (keys[SDL_SCANCODE_SPACE]) pads[0].buttons |= 0x0100;
    if (keys[SDL_SCANCODE_RETURN] && !alt) pads[0].buttons |= 0x1000;
    if (keys[SDL_SCANCODE_UP]) pads[0].buttons |= 0x0008;
    if (keys[SDL_SCANCODE_DOWN]) pads[0].buttons |= 0x0004;
    if (keys[SDL_SCANCODE_LEFT]) pads[0].buttons |= 0x0001;
    if (keys[SDL_SCANCODE_RIGHT]) pads[0].buttons |= 0x0002;
    if (keys[SDL_SCANCODE_W]) pads[0].stick_y = 127;
    if (keys[SDL_SCANCODE_S]) pads[0].stick_y = -128;
    if (keys[SDL_SCANCODE_D]) pads[0].stick_x = 127;
    if (keys[SDL_SCANCODE_A]) pads[0].stick_x = -128;
}

// Sword and Nunchuk gestures (motion_gesture.h). Keys fire on their press edge only (per bound input).
// Defaults ([input.keyboard], input_bindings.h):
//   Numpad 8/2/4/6 and 7/9/1/3: swing up/down/left/right and diagonals; Numpad 5: thrust;
//   Numpad 0 (hold): raise the remote (Skyward Strike pose).
//   Main-keyboard grid U I O / J K L / M , . : the same 8 swings with K = thrust; X (hold): raise.
//   F: Nunchuk flick (shield); R: spin attack (horizontal); T: spin attack (vertical).
//   Middle mouse button held + mouse flick (> 40 px): swing in the flick direction, re-armed every 0.3 s.
void MouseKeyboardSource::sword_controls(const uint8_t* keys, bool focused, uint32_t mouse_state, int mx, int my,
                                         float dt) {
    struct Bind { KeyAction action; int kind; float dx, dy; };
    enum { kSwing, kThrust, kFlick, kSpinH, kSpinV };
    static const Bind binds[] = {
        {kKeySwingUp, kSwing, 0, 1}, {kKeySwingDown, kSwing, 0, -1},
        {kKeySwingLeft, kSwing, -1, 0}, {kKeySwingRight, kSwing, 1, 0},
        {kKeySwingUpLeft, kSwing, -1, 1}, {kKeySwingUpRight, kSwing, 1, 1},
        {kKeySwingDownLeft, kSwing, -1, -1}, {kKeySwingDownRight, kSwing, 1, -1},
        {kKeyThrust, kThrust, 0, 0}, {kKeyNunchukFlick, kFlick, 0, 0},
        {kKeySpinHorizontal, kSpinH, 0, 0}, {kKeySpinVertical, kSpinV, 0, 0},
    };
    auto& gesture = g_virtual_gesture[0];
    const InputBindings& bind = input_bindings();
    for (const auto& b : binds) {
        const auto& alternatives = bind.keyboard[size_t(b.action)];
        auto& previous = previous_alternative[size_t(b.action)];
        previous.resize(alternatives.size(), 0);
        for (size_t k = 0; k < alternatives.size(); ++k) {
            const bool down = focused && key_combo_held(alternatives[k], keys, mouse_state);
            const bool edge = down && !previous[k];
            previous[k] = down;
            if (!edge) continue;
            switch (b.kind) {
            case kSwing: gesture.swing(b.dx, b.dy); break;
            case kThrust: gesture.thrust(); break;
            case kFlick: gesture.nunchuk_flick(); break;
            case kSpinH: gesture.spin(false); break;
            case kSpinV: gesture.spin(true); break;
            }
        }
    }
    const bool raise = focused && key_action_held(kKeyRaise, keys, mouse_state);
    if (raise != raised) { raised = raise; gesture.set_raised(raise); }
    // Middle-button mouse flick.
    const bool middle = focused && key_action_held(kKeyMouseSwing, keys, mouse_state);
    if (!middle) { flick_armed = false; middle_held = false; return; }
    if (!middle_held) { middle_held = true; flick_armed = true; flick_x = mx; flick_y = my; flick_rearm = 0; }
    if (!flick_armed) {
        if (dt > 0 && dt < 1) flick_rearm -= dt;
        if (flick_rearm <= 0) { flick_armed = true; flick_x = mx; flick_y = my; }
        return;
    }
    const float dx = float(mx - flick_x), dy = float(my - flick_y);
    if (dx * dx + dy * dy > 40.0f * 40.0f) {
        gesture.swing(dx, -dy); // screen y grows downward
        flick_armed = false;
        flick_rearm = 0.3f;
    }
}

// CTRL18-INPUT: the "kbm" keyboard+mouse preset (design section 4). Keys -> intents ([input.kbm]) -> KbmTranslator
// (hd_translator.h). The mouse is captured (SDL relative mode, cursor hidden) in play / aim / wheels and released at
// once in menus, events, without keyboard focus and while the port menu (F1) is open.
void MouseKeyboardSource::update_kbm(GameCubePadState pads[4], WiimoteState motes[4]) {
    const InputBindings& bind = input_bindings();
    int mx, my;
    const uint32_t mouse_state = SDL_GetMouseState(&mx, &my);
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    const bool focused = SDL_GetKeyboardFocus() != nullptr;
    int rel_x = 0, rel_y = 0;
    SDL_GetRelativeMouseState(&rel_x, &rel_y); // always drained (no spike after a mode change)
    const float dt = motes[0].motion.dt_seconds;
    int w = nwii::runtime::Config::get().window_width;
    int h = nwii::runtime::Config::get().window_height;
    if (SDL_Window* win = SDL_GetMouseFocus() ? SDL_GetMouseFocus() : SDL_GetKeyboardFocus())
        SDL_GetWindowSize(win, &w, &h);
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    {
        const float rx = g_present_rect[0].load(), ry = g_present_rect[1].load();
        const float rw = g_present_rect[2].load(), rh = g_present_rect[3].load();
        if (rw > 0.0f && rh > 0.0f) {
            mx = int(std::clamp((float(mx) / w - rx) / rw, 0.0f, 1.0f) * w);
            my = int(std::clamp((float(my) / h - ry) / rh, 0.0f, 1.0f) * h);
        }
    }
    // The mouse owns the game's pointer once it is used (moved > 2 px or clicked), as in the classic preset.
    if (focused && last_mx >= 0 &&
        (std::abs(mx - last_mx) + std::abs(my - last_my) > 2 || (kbm_captured && (rel_x || rel_y)) ||
         (mouse_state & (SDL_BUTTON(SDL_BUTTON_LEFT) | SDL_BUTTON(SDL_BUTTON_RIGHT))) != 0))
        g_aim_owner.store(0);
    if (focused && (keyboard_or_mouse_used(mx, my, last_mx, last_my, mouse_state) || (kbm_captured && (rel_x || rel_y))))
        g_active_device[0].store(int(ActiveDevice::KeyboardMouse));
    last_mx = mx;
    last_my = my;

    KbmIntents in;
    if (focused)
        for (int a = 0; a < kKbmActionCount; ++a) in.held[a] = kbm_action_held(KbmAction(a), keys, mouse_state);
    in.captured = kbm_captured && focused;
    in.mouse_dx = in.captured ? float(rel_x) : 0.0f;
    in.mouse_dy = in.captured ? float(rel_y) : 0.0f;
    in.abs_x = std::clamp(2.0f * mx / w - 1.0f, -1.0f, 1.0f);
    in.abs_y = std::clamp(2.0f * my / h - 1.0f, -1.0f, 1.0f);
    in.mouse_owns_pointer = g_aim_owner.load() == 0;
    in.dt = dt > 0.0f && dt <= 0.1f ? dt : 0.0;
    kbm.a_does_not_dash = bind.a_does_not_dash;
    kbm.quick_items = bind.quick_items;
    kbm.aim_sensitivity = bind.mouse_aim_sensitivity;
    kbm.invert_y = bind.mouse_invert_y;
    kbm.sword().tuning.fire_on_release = bind.fire_on_release;
    const GameContext ctx = g_guest_context.get();
    const LiveFlags live = LiveFlags::read(0);
    g_ctrl18_flight_invert_y.store(bind.flight_invert_y);
    KbmOut out;
    kbm.step(in, ctx, live, g_virtual_gesture[0], g_virtual_orientation[0], g_pointer_aim[0], out);

    // Capture for the next update.
    const bool want = out.want_capture && focused && !g_port_menu_open.load();
    if (want != kbm_captured) {
        const bool ok = SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE) == 0;
        kbm_captured = want && ok;
    }
    if (focused) ctrl18_feed_camera_mouse(out.cam_dx, out.cam_dy); // CTRL18-CAM applies [mouse] sensitivity_camera
    if (!g_guest_player_present.load() || out.situation != Situation::Menu) // the pad source reports when present
        ctrl18_set_status("kbm", out.situation, false);
    merge_virtual_motion(motes[0].motion, focused, out.pitch, out.yaw, out.roll);
    motes[0].err = 0;
    if (g_guest_point_center.load()) {
        motes[0].ir_x = 0.5f;
        motes[0].ir_y = 0.5f;
    } else {
        // With a MotionPlus pointer the virtual sensor bar is reported as not visible (classic rule, pointer-mpls T7).
        const bool motionplus_pointer = g_guest_motionplus_pointer[0].load();
        motes[0].ir_x = motionplus_pointer ? -1.0f : (float)mx / (float)w;
        motes[0].ir_y = motionplus_pointer ? -1.0f : (float)my / (float)h;
    }
    motes[0].buttons |= out.buttons;
    NunchukState nunchuk;
    nunchuk.available = true;
    nunchuk.stick_x = out.stick_x;
    nunchuk.stick_y = out.stick_y;
    nunchuk.buttons = out.nunchuk_buttons;
    merge_nunchuk(motes[0].nunchuk, nunchuk);
    pads[0].err = 0;
}

}
