#pragma once
// Read-only gameplay observation published by a game adapter (SOUE01: ss_boot.cpp,
// SS_KPADReadEx, once per game frame on channel 0) for logging and for the explicit
// test replay (boot_replay_source.h). It is a copy of guest memory, never written back.
// Header-only on purpose: no build-list change is needed.
//
// SOUE01 sources (outputs/skyward-sword, symbols.txt / headers):
// - dScGame_c::currentSpawnInfo 0x805B3860: stageName +0x00 (char[32]), room +0x22, layer +0x23,
//   entrance +0x24 (d_sc_game.h SpawnInfo).
// - dAcPy_c::LINK 0x8057578C -> player: mRotation +0xB8 (s16 x,y,z), mPosition +0xC0 (f32 x,y,z)
//   (d_a_base.h), mAttackDirection u8 +0x338, mSpecificAttackDirection u16 +0x33A,
//   mSwordAndMoreStates +0x348, mActionFlags +0x364, mCurrentAction +0x36C (d_player_act.h),
//   swing vector f32 +0x4608/+0x460C (computed from the MotionPlus basis by fn_8023BDE0),
//   remote/Nunchuk motion trigger bytes +0x4365/+0x4364 (handlesInputMaybe 0x8023BF30);
//   attack angle s16 +0x444C (ActorLink__applyAttackDirection 0x801C8EB0 stores it on every swing start;
//   mAttackDirection +0x338 is only stored while +0x43CA == 0x11 or for special attacks);
//   daPlayerActBase_c::sCurrentSword u8 0x80571C74.
// - EventManager::sInstance 0x80575378 -> mState +0x184, mCurrentEvent.eventName +0x94
//   (toBeSorted/event_manager.h, event.h).
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace nwii::runtime::input {

// LOAD14 (roadmap M6.3): one save slot as the title's file select sees it, read from FileManager::sInstance
// (.sbss 0x80574FFC) -> mpSavedSaveFiles (+0x0, SavedSaveFiles: saveFiles[3] at +0x20, 0x53C0 each), the buffer
// the game reads from / writes to wiiking2.sav (toBeSorted/file_manager.h, save_file.h). Read-only.
struct SaveSlotObservation {
    bool read = false;            // mpSavedSaveFiles valid
    uint8_t new_file = 1;         // SaveFile +0x53AD (1 = empty slot, "New Adventure")
    int64_t play_time = 0;        // +0x0 (OSTime ticks)
    int64_t saved_time = 0;       // +0x8 (OSTime ticks of the last save)
    char name[9] = {};            // player_name +0x8D4 (UTF-16BE; ASCII subset)
    char stage[16] = {};          // area_t1 +0x531C
    int room = -1, layer = -1, entrance = -1; // room_id_t1 +0x5308, forced_layer_t1 +0x53A5, entrance_t1 +0x53A8
    float pos[3] = {0, 0, 0};     // pos_t1 +0x10
    int16_t angle = 0;            // angle_t1 +0x530E
    int health = -1, health_capacity = -1; // quarter hearts, +0x5306 / +0x5302
};

struct GameObservation {
    uint32_t frame = 0;          // game frames observed (KPAD reads on channel 0)
    char stage[16] = {};         // currentSpawnInfo stage name
    int room = -1, layer = -1, entrance = -1;
    bool link_present = false;   // dAcPy_c::LINK != 0
    float pos[3] = {0, 0, 0};    // LINK mPosition
    int16_t rot_y = 0;           // LINK mRotation.y
    uint8_t attack_direction = 0;
    uint16_t specific_attack_direction = 0;
    uint32_t sword_states = 0, action_flags = 0;
    int32_t current_action = -1;
    float swing[2] = {0, 0};     // LINK +0x4608/+0x460C
    uint8_t remote_motion = 0, nunchuk_motion = 0; // LINK +0x4365/+0x4364
    int16_t attack_angle = 0;    // LINK +0x444C (atan2s(v.y, -v.x) of the swing vector at swing start)
    uint16_t item_state = 0;     // LINK +0x43CA (equipped-item state; applyAttackDirection stores mAttackDirection when 0x11)
    uint8_t current_sword = 0;   // daPlayerActBase_c::sCurrentSword (.sdata 0x80571C74)
    int32_t event_state = -1;    // EventManager mState, -1 = no manager
    char event_name[32] = {};
    // LOAD14 (read-only, FileManager::sInstance 0x80574FFC): the title's three save slots, the selected file
    // (mSelectedFile +0xA83C) and the current file's hearts/rupees. Outside the title the current file is mFileA
    // (+0x8; FileManager::getCurrentFile). Rupees = item counter 0x1F5 (RupeeCounter) decoded with the game's own
    // ItemflagManager__ITEMFLAG_DEFINITIONS (.data 0x805116F4: u8 word index, u8 shift<<4 | bits-1) from mFileA's
    // item_flags (+0x9E4).
    bool file_manager = false;
    int selected_file = -1;
    int health = -1, health_capacity = -1; // current file, quarter hearts
    int rupees = -1;
    SaveSlotObservation slots[3];
    // SOAK14 (read-only): the A-button prompt as Act_IE_e (d_lyt_unknowns.h: 16 talk, 4 open, 85 dive, 152 none), see
    // ss_a_button_prompt; dLytDobutton_c::sInstance (.sbss 0x80575468) mDoActionToShow +0x470 (Act_Do_e, 94 = none).
    // -1 = layout not present.
    int32_t do_action = -1, do_shown = -1;
};

// SOAK14: the A-button prompt the HUD shows. SS rebuilds LytDoButtonRelated's DO_BUTTON_A (sInstance .sbss 0x80575478,
// +0x04) every frame in dLytMeterABtn_c::execute from dLytDobutton_c's latched request (+0x47C Act_Do_e, +0x480 icon;
// d_lyt_meter_a_btn.cpp) and dLytMeter_c::execute sets it back to NONE afterwards (d_lyt_meter.cpp), so a read between
// frames only ever saw NONE (or the map's zoom actions, set after the reset). The dLytDobutton fields persist. Act_Do
// 0..92 == Act_IE INFO 0..92 (sActIEtoDoConversionTable). related_a is used when the latched request shows nothing.
inline int32_t ss_a_button_prompt(int32_t related_a, int32_t dob_act, int32_t dob_icon) {
    switch (dob_icon) { // the icons for which dLytMeterABtn_c shows the dLytDobutton action on A
    case 0x0: case 0x7: case 0xB: case 0x15: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1F: case 0x21:
    case 0x22: case 0x23: case 0x27: case 0x28:
        if (dob_act >= 0 && dob_act < 93) return dob_act;
        break;
    default: break;
    }
    return related_a >= 0 && related_a < 152 ? related_a : 152;
}

class GameObservationChannel {
public:
    void publish(const GameObservation& o) {
        std::lock_guard lock(mutex_);
        value_ = o;
        frames_.store(o.frame, std::memory_order_release);
    }
    GameObservation get() const {
        std::lock_guard lock(mutex_);
        return value_;
    }
    uint32_t frames() const { return frames_.load(std::memory_order_acquire); }

private:
    mutable std::mutex mutex_;
    GameObservation value_{};
    std::atomic<uint32_t> frames_{0};
};

inline GameObservationChannel g_game_observation;

// Set by the game adapter while a player actor exists outside the title scene (SS: dAcPy_c::LINK != 0
// and dTitleMgr_c::sInstance == 0).
// Live sources use it to enable sword gestures on the right stick; it changes no guest data.
inline std::atomic<bool> g_guest_player_present{false};

}
