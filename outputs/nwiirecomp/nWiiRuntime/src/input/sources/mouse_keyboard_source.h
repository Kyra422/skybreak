#pragma once
#include "input/input_source.h"
#include "../input_bindings.h"
#include "../hd_translator.h" // CTRL18-INPUT
#include <vector>
#include <cstdint>

namespace nwii::runtime::input {

class MouseKeyboardSource : public IInputSource {
public:
    MouseKeyboardSource() = default;
    ~MouseKeyboardSource() override = default;

    void update(GameCubePadState pads[4], WiimoteState motes[4]) override;

private:
    void sword_controls(const uint8_t* keys, bool focused, uint32_t mouse_state, int mx, int my, float dt);

    bool owns_relative_mouse = false;
    int last_mx = -1, last_my = -1; // to detect real mouse use (claims g_aim_owner)
    // Sword gesture edges per bound input (input_bindings.h KeyAction) and the middle-button flick.
    std::vector<uint8_t> previous_alternative[kKeyActionCount];
    bool raised = false, middle_held = false, flick_armed = false;
    int flick_x = 0, flick_y = 0;
    float flick_rearm = 0;
    // CTRL18-INPUT: the "kbm" preset (hd_translator.h KbmTranslator): automatic mouse capture, modes by situation.
    void update_kbm(GameCubePadState pads[4], WiimoteState motes[4]);
    KbmTranslator kbm;
    bool kbm_captured = false;
    int last_key_preset = -1;
};

} 
