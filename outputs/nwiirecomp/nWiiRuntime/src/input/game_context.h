#pragma once
// CTRL18-INPUT (work/claude-notes/CTRL18-design.md section 3.2): the game context the control translator
// (hd_translator.h) needs, read READ-ONLY from guest memory once per game frame by the game adapter (SOUE01:
// ss_boot.cpp publish_game_observation, channel-0 KPAD read) and published as one snapshot. Unlike the full
// GameObservation (replay/trace runs only) this is always published: a handful of reads per frame.
// Nothing here is ever written back to the guest. Header-only on purpose: no build-list change is needed.
//
// SOUE01 sources (outputs/skyward-sword decomp):
// - dAcPy_c::LINK 0x8057578C: mRidingActorType u8 +0x339 (2 Loftwing, 1 minecart, 3 boat), mSwordAndMoreStates
//   +0x348, mActionFlags +0x364, +0x368 (aim/first-person bits 0x5D7), mCurrentAction +0x36C (d_player_act.h),
//   equipped-item state u16 +0x43CA; dTitleMgr_c::sInstance 0x805756B4 (title scene).
// - EventManager::sInstance 0x80575378 mState +0x184 (0 = no event).
// - A prompt: game_observation.h ss_a_button_prompt (LytDoButtonRelated 0x80575478 + dLytDobutton_c 0x80575468).
// - Game camera: dScGame_c::CAMERAS[0] 0x80575788 -> dCamera_c +0x2B4 -> dCameraGame_c mode +0x6C, handler +0x74.
// - HUD prompts other than A (research 2026-10-09, d_lyt_meter*.cpp): LytDoButtonRelated is reset to NONE after the meter
//   copied it, so between frames only the parts' own latched copies are valid (one frame old): M = dLytMeter_c::sInstance
//   0x80575448: Z +0x1252C (has +0x12530), NUN_STK +0x126B4, C/dowsing +0x12188, REMOCON_BG +0x127A8, NUN_BG +0x1296C,
//   B +0x757C, cross T/D/L/R +0x7A5C/+0x7ADC/+0x7B5C/+0x7BDC, PLUS +0x7DC8; cutscene skip shown: *(M+0x374) +0x160.
//   Link's own requests (s16 cur/prev, -1 = none, ActorLink__update resets cur each frame): A +0x43FE/+0x4400, NUN_BG
//   +0x4402/+0x4404, CROSS_T +0x4406/+0x4408, CROSS_D +0x440A/+0x440C, B +0x440E/+0x4410, Z +0x4412/+0x4414.
//   dLytDobutton_c (0x80575468) latched action +0x47C (Act_Do) / icon +0x480 (motion pictograms: 2 THROW, 3 ROLL,
//   0x26 FATAL_BLOW, 0x27 THRUST, ...).
// - Item wheels (calcItemWheelSelection 0x801E3E70): highlighted slot LINK +0x43B1 (s8), equipped B item +0x43CE;
//   item in hand +0x43CA (0 bomb, 1 bow, 2 clawshots, 3 beetle, 4 slingshot, 5 gust bellows, 6 whip, 8 bug net,
//   9 harp, 0xB none, 0x11 sword); carried actor *(LINK +0x1368) (profile +0x8; 0x1AA = bomb).
// - The game's own remote direction: dPad::ex_c::m_current_ex (*0x80571C60) mMPLS.mZ +0x2274 (the wheels select by it).
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace nwii::runtime::input {

// mSwordAndMoreStates (+0x348) bits, d_player_act.h.
enum : uint32_t {
    kSwordSprinting = 1u << 6, kSwordBWheelOpen = 1u << 7, kSwordPouchOpen = 1u << 8, kSwordFirstPerson = 1u << 18,
    kSwordSkywardStrike = 1u << 20, kSwordCMenuOpen = 1u << 23, kSwordShieldOut = 1u << 27, kSwordUsingItem = 1u << 31,
};
// mActionFlags (+0x364) bits, d_player_act.h.
enum : uint32_t {
    kActInAir = 0x2, kActHangingLedge = 0x40, kActOnVines = 0x10000, kActInWater = 0x40000, kActSwingRope = 0x400000,
    kActTightRope = 0x800000, kActCrawling = 0x1000000,
};
// Items in hand (LINK +0x43CA).
enum : uint16_t {
    kItemBomb = 0, kItemBow = 1, kItemClaw = 2, kItemBeetle = 3, kItemSlingshot = 4, kItemBellows = 5, kItemWhip = 6,
    kItemNet = 8, kItemHarp = 9, kItemNone = 0xB, kItemSword = 0x11,
};
// Act_IE_e values of the HUD prompts (d_lyt_unknowns.h / d_lyt_do_button.h; Act_Do 0..92 == Act_IE 0..92).
enum : int32_t {
    kPromptJump = 1, kPromptDropDown = 2, kPromptLetGo = 3, kPromptRoll = 8, kPromptSwim = 23, kPromptLeap = 24,
    kPromptDash = 25, kPromptGetOut = 26, kPromptSailcloth = 40, kPromptPutAway = 41, kPromptTurn = 45,
    kPromptDrawBow = 48, kPromptSwing = 52, kPromptBalance = 53, kPromptFatalBlow = 67, kPromptJumpDown = 68,
    kPromptStrum = 69, kPromptRelease = 71, kPromptDive = 85, kPromptEtcDash = 95, kPromptEtcDodge = 101,
    kPromptNone = 152,
};

struct GameContext {
    uint32_t frame = 0;          // channel-0 KPAD reads that published a context
    bool valid = false;          // a game adapter publishes contexts
    bool player = false;         // Link exists outside the title scene (== g_guest_player_present)
    bool title = false;
    uint8_t riding = 0;          // LINK +0x339
    uint32_t sword_states = 0;   // LINK +0x348
    uint32_t action_flags = 0;   // LINK +0x364
    uint32_t aim_flags = 0;      // LINK +0x368
    int32_t action = -1;         // LINK +0x36C
    uint16_t item_state = 0;     // LINK +0x43CA
    int32_t prompt_a = kPromptNone; // what A does now (Act_IE_e, ss_a_button_prompt)
    int32_t event_state = -1;    // EventManager mState (-1 = no manager, 0 = no event)
    int32_t cam_mode = -1, cam_handler = -1; // game camera mode / handler (-1 = unknown)
    // Slots of LytDoButtonRelated other than A, as far as they can be read between frames (see ss_boot.cpp; -1 =
    // unknown, kPromptNone = nothing shown). Index = DO_BUTTON_* slot (A 0, B 1, C 2, Z 3, NUN_STK 4, REMOCON_BG 5,
    // NUN_BG 6, CROSS_T 7, CROSS_D 8, CROSS_L 9, CROSS_R 10).
    int32_t prompt_slot[11] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int32_t prompt_plus = -1;
    int32_t link_request[6] = {-1, -1, -1, -1, -1, -1}; // Link's HUD requests (A, NUN_BG, CROSS_T, CROSS_D, B, Z)
    int32_t dob_act = -1, dob_icon = -1;                 // dLytDobutton_c latched action / icon
    bool skip_shown = false;                             // the cutscene-skip prompt is on screen (hold Wii 2)
    int8_t wheel_index = -1;                             // highlighted wheel slot (-1 none)
    uint16_t equipped_b = 0xB;                           // LINK +0x43CE
    int32_t carried_profile = -1;                        // carried actor's profile (-1 = nothing carried)
    bool have_mz = false;
    float game_mz[3] = {0, 0, 1};                        // the game's remote pointing direction (MotionPlus basis Z)
    int16_t stick_angle = 0, move_angle = 0;             // LINK +0x441E raw stick angle, +0x4420 = camera yaw + stick angle
    bool event() const { return event_state > 0; }
    bool carrying() const { return carried_profile >= 0; }
};

// A steady yaw leaves gravity unchanged, so the sensor's stillness detector
// cannot distinguish a slow intentional turn from drift. Freeze automatic bias
// learning while the game uses a pointing/steering reference. Inspect the raw
// context: a visible cursor can make the UI translator classify flight as Menu.
inline bool gyro_bias_learning_allowed(const GameContext& c) {
    if (!c.valid) return true;
    const bool aiming = (c.sword_states & kSwordFirstPerson) || (c.aim_flags & 0x5D7) ||
                        c.cam_mode == 2 || c.cam_mode == 3 || c.cam_mode == 4 || c.cam_mode == 5 || c.cam_mode == 8;
    const bool wheel = c.sword_states & (kSwordBWheelOpen | kSwordPouchOpen | kSwordCMenuOpen);
    const bool steering = c.riding == 2 || (c.item_state == kItemBeetle && c.cam_handler == 6) ||
                          (c.action_flags & (kActInWater | kActTightRope)) || c.action == 0x12 || c.action == 0x13;
    const bool puzzle = c.prompt_slot[2] == 130 || c.prompt_slot[3] == 129;
    return !(aiming || wheel || steering || puzzle || c.event() || c.carrying());
}

class GameContextChannel {
public:
    void publish(const GameContext& c) {
        std::lock_guard lock(mutex_);
        value_ = c;
        frames_.store(c.frame, std::memory_order_release);
    }
    GameContext get() const {
        std::lock_guard lock(mutex_);
        return value_;
    }
    uint32_t frames() const { return frames_.load(std::memory_order_acquire); }

private:
    mutable std::mutex mutex_;
    GameContext value_{};
    std::atomic<uint32_t> frames_{0};
};

inline GameContextChannel g_guest_context;

// Set by the port UI (ui_app.cpp, CTRL18-UI) while its overlay menu is open: the kbm preset releases its mouse
// capture at once.
inline std::atomic<bool> g_port_menu_open{false};

} // namespace nwii::runtime::input
