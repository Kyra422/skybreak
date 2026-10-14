#pragma once
#include <cstdint>
#include <cstring>

namespace nwii::runtime::input {
struct MessageContext {
    bool known = false, modal = false;
};

// SOUE01 dLytMsgWindow_c: instance at 80575488, state manager at +4,
// method at manager+18, current state ID at method+1C (window+38).
// sStateID_c stores its name at +4. Explain* windows are passive tutorial
// overlays; OutputText/WaitKey* windows consume confirmation button presses.
template<class Read32, class Read8, class Valid>
MessageContext read_message_context(Read32 read32, Read8 read8, Valid valid) {
    MessageContext result;
    const uint32_t window = read32(0x80575488);
    if (!valid(window, 0x3C)) return result;
    const uint32_t state = read32(window + 0x38);
    if (!valid(state, 8)) return result;
    const uint32_t name = read32(state + 4);
    char text[96]{};
    for (uint32_t i = 0; i < sizeof(text); ++i) {
        if (!valid(name + i, 1)) return result;
        text[i] = char(read8(name + i));
        if (!text[i]) break;
        if (i + 1 == sizeof(text)) return result;
    }
    constexpr char prefix[] = "dLytMsgWindow_c::StateID_";
    if (std::strncmp(text, prefix, sizeof(prefix)-1) != 0) return result;
    const char* label = text + sizeof(prefix)-1;
    if (std::strcmp(label, "Invisible") == 0 || std::strncmp(label, "Explain", 7) == 0) {
        result.known = true;
    } else if (std::strcmp(label, "In") == 0 || std::strcmp(label, "Out") == 0 ||
               std::strcmp(label, "OutputText") == 0 || std::strncmp(label, "WaitKey", 7) == 0 ||
               std::strncmp(label, "Map", 3) == 0 || std::strncmp(label, "Demo", 4) == 0) {
        result.known = result.modal = true;
    }
    return result;
}
} // namespace nwii::runtime::input
