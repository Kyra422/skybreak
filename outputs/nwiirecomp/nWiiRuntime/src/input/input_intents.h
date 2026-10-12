#pragma once
// CTRL18-INPUT: the intents of the "switch-hd" pad preset ([input.gamepad_hd]) and of the "kbm" keyboard+mouse preset
// ([input.kbm]), and the preset ids. SDL-free on purpose: the translator (hd_translator.h) and the replay scripts are
// compiled into harnesses without SDL; the input names / defaults / parsing are in input_bindings.h.
namespace nwii::runtime::input {

// CTRL18-INPUT: intents of the "switch-hd" pad preset ([input.gamepad_hd]) and of the "kbm" keyboard+mouse preset
// ([input.kbm]). The translator (hd_translator.h) turns them into Wii Remote / Nunchuk signals by game context.
enum HdAction : int {
    kHdAction, kHdDash, kHdHarp, kHdRecentre, kHdShield, kHdThrust, kHdLockOn, kHdCamera, kHdItem, kHdPouch,
    kHdMap, kHdGear, kHdFirstPerson, kHdFi, kHdHelp, kHdDpadLeft, kHdHome, kHdGyroToggle, kHdActionCount
};
enum KbmAction : int {
    kKbmUp, kKbmDown, kKbmLeft, kKbmRight, kKbmWalk, kKbmDash, kKbmAction, kKbmDodge, kKbmSlash, kKbmThrust,
    kKbmShield, kKbmRaise, kKbmSpin, kKbmLockOn, kKbmItem, kKbmPouch, kKbmHarp, kKbmItem1, kKbmItem2, kKbmItem3,
    kKbmItem4, kKbmItem5, kKbmItem6, kKbmItem7, kKbmItem8, kKbmFirstPerson, kKbmMap, kKbmGear, kKbmFi, kKbmHelp,
    kKbmSkip, kKbmDpadUp, kKbmDpadDown, kKbmDpadLeft, kKbmDpadRight, kKbmHome, kKbmActionCount
};
enum class PadPreset : int { SwitchHd, Classic, WiiMotion };
enum class KeyPreset : int { Kbm, Classic };

} // namespace nwii::runtime::input
