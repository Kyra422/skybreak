#pragma once
#include "pad_hub.h"
#include "input/input_source.h"
#include <SDL.h>
#include <cstring>

namespace nwii::runtime::input {
inline bool is_wii_remote_name(const char* name) {
    // SDL's Wii HIDAPI names; avoid routing Wii U Pro / Classic Controllers here.
    return name && (std::strcmp(name, "Nintendo Wii Remote") == 0 ||
                    std::strcmp(name, "Nintendo Wii Remote with Nunchuk") == 0);
}
inline uint32_t wii_remote_buttons(uint32_t buttons) {
    // SDL 2.28.5 HIDAPI maps physical Wii A to SDL B and physical B to SDL A.
    // These are labels, not the position-based Switch HD actions.
    static constexpr struct { int button; uint32_t mask; } map[] = {
        {SDL_CONTROLLER_BUTTON_B,0x0800},{SDL_CONTROLLER_BUTTON_A,0x0400},
        {SDL_CONTROLLER_BUTTON_X,0x0200},{SDL_CONTROLLER_BUTTON_Y,0x0100},
        {SDL_CONTROLLER_BUTTON_START,0x0010},{SDL_CONTROLLER_BUTTON_BACK,0x1000},
        {SDL_CONTROLLER_BUTTON_GUIDE,0x8000},{SDL_CONTROLLER_BUTTON_DPAD_UP,0x0008},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN,0x0004},{SDL_CONTROLLER_BUTTON_DPAD_LEFT,0x0001},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT,0x0002}};
    uint32_t out=0;
    for(const auto& m:map) if(buttons & (1u << m.button)) out |= m.mask;
    return out;
}
inline NunchukState wii_remote_nunchuk(const PadSnapshot& p) {
    NunchukState n;
    if(!p.has_nunchuk) return n;
    n.available=true;
    if(p.buttons & (1u << SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) n.buttons |= 0x4000;
    if(p.axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] > 16383) n.buttons |= 0x2000;
    // SDL already calibrates the physical stick. Retain its analog range.
    n.stick_x=std::clamp(float(p.axes[SDL_CONTROLLER_AXIS_LEFTX])/32767.f,-1.f,1.f);
    n.stick_y=std::clamp(-float(p.axes[SDL_CONTROLLER_AXIS_LEFTY])/32767.f,-1.f,1.f);
    return n;
}
} // namespace nwii::runtime::input
