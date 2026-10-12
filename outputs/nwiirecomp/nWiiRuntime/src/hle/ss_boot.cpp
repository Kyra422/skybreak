#include "runtime/ss_boot.h"
#include "runtime/ss_motion.h"
#include "runtime/progress_snapshot.h"

#include "input/input_manager.h"
#include "../input/sources/boot_replay_source.h"
#include "../input/game_observation.h"
#include "../input/game_context.h" // CTRL18-INPUT: control context (read-only)
#include "../input/virtual_motion.h"
#include "runtime/camera_stick.h" // CTRL18-CAM: per-frame tick (free camera sample, rumble)
#include "runtime/cpu_context.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using nwii::runtime::CPUContext;
using nwii::runtime::input::InputManager;
using nwii::runtime::input::WiimoteState;

extern "C" void WPADInit(CPUContext& ctx);
extern "C" void KPADInit(CPUContext& ctx);
// These remain the original translated SOUE01 implementations.
extern "C" void ss_KMPLSInit(CPUContext&);
extern "C" void ss_KMPLSRead(CPUContext&);
extern "C" void ss_KMPLSSetKpadRingBuffer(CPUContext&);
extern "C" void ss_KMPLSReset(CPUContext&);
extern "C" void ss_KMPLSStopCalibration(CPUContext&);
extern "C" void ss_wpad_callback_func(CPUContext&);

// FREEZE8 freeze watchdog (core/freeze_watchdog.cpp), diagnostics only.
namespace nwii::runtime::freeze {
void note_cpu_thread(CPUContext& ctx);
void set_game_info(std::string (*fn)(CPUContext&));
void report_fatal(CPUContext& ctx, const std::string& reason);
}

namespace {

// FREEZE8: SOUE01 game state for the freeze report (read-only; same addresses as publish_game_observation).
std::string freeze_game_info(CPUContext& ctx) {
    const auto valid = [](uint32_t p, uint32_t size) {
        return (p >= 0x80000000 && uint64_t(p) + size <= 0x81800000) ||
               (p >= 0x90000000 && uint64_t(p) + size <= 0x94000000);
    };
    char stage[9] = {};
    for (uint32_t i = 0; i < 8; ++i)
        if (!(stage[i] = static_cast<char>(ctx.mmu.read8(0x805B3860 + i)))) break; // dScGame_c::currentSpawnInfo
    char line[512];
    int n = std::snprintf(line, sizeof line, "[FREEZE8] game: stage=%s room=%u layer=%u entrance=%u", stage,
                          unsigned(ctx.mmu.read8(0x805B3860 + 0x22)), unsigned(ctx.mmu.read8(0x805B3860 + 0x23)),
                          unsigned(ctx.mmu.read8(0x805B3860 + 0x24)));
    const uint32_t link = ctx.mmu.read32(0x8057578C); // dAcPy_c::LINK
    if (valid(link, 0x4620) && n > 0 && n < (int)sizeof line)
        n += std::snprintf(line + n, sizeof line - n, " link action=%d flags=0x%X pos=(%.1f,%.1f,%.1f)",
                           static_cast<int32_t>(ctx.mmu.read32(link + 0x36C)), ctx.mmu.read32(link + 0x364),
                           ctx.mmu.read_f32(link + 0xC0), ctx.mmu.read_f32(link + 0xC4), ctx.mmu.read_f32(link + 0xC8));
    const uint32_t events = ctx.mmu.read32(0x80575378); // EventManager::sInstance
    if (valid(events, 0x188) && n > 0 && n < (int)sizeof line) {
        char name[33] = {};
        for (uint32_t i = 0; i + 1 < sizeof name; ++i)
            if (!(name[i] = static_cast<char>(ctx.mmu.read8(events + 0x94 + i)))) break;
        n += std::snprintf(line + n, sizeof line - n, " event=%d '%s'",
                           static_cast<int32_t>(ctx.mmu.read32(events + 0x184)), name);
    }
    return std::string(line) + "\n";
}

constexpr int32_t kOk = 0;
constexpr int32_t kNoController = -1;
constexpr int32_t kInvalid = -4;
constexpr uint8_t kCoreDevice = 0;
constexpr uint8_t kNunchukDevice = 1;
constexpr uint8_t kNoDevice = 253;
constexpr uint32_t kCoreButtons = 0x9F1F;
constexpr uint32_t kNunchukButtons = 0x6000;
constexpr uint32_t kStatusSize = 0xF0;
// ELF symbol inside_kpads; lis 8060 + signed addi C888 = 805FC888.
constexpr uint32_t kKpadBase = 0x805FC888;
constexpr uint32_t kKpadStride = 0x688;

struct MotionConfiguration {
    uint8_t mode = 0;
    uint8_t requested = 0;
    bool pending = false;
};

struct BootInputState {
    bool wpad_initialized = false;
    bool kpad_initialized = false;
    std::array<uint32_t, 4> callbacks{};
    std::array<uint32_t, 4> previous_buttons{};
    std::array<uint64_t, 4> published_sequence{};
    std::array<uint64_t, 4> published_timestamp{};
    std::array<MotionConfiguration, 4> motion{};
    std::array<std::array<uint8_t, 0x42>, 4> raw{};
    std::array<std::array<uint8_t, kStatusSize>, 4> previous_status{};
    std::array<int32_t, 4> reported_connection{kNoController, kNoController,
                                                kNoController, kNoController};
};

// The game and its callbacks execute on the runtime's single CPU thread.
thread_local BootInputState input_state;

void sample_boot_state(CPUContext& ctx) {
    static const bool enabled = std::getenv("NWII_SS_BOOT_STATE") != nullptr;
    if (!enabled) return;
    using Clock = std::chrono::steady_clock;
    static thread_local auto previous = Clock::time_point{};
    const auto now = Clock::now();
    if (now - previous < std::chrono::seconds(2)) return;
    previous = now;
    const auto valid = [](uint32_t p, uint32_t size) {
        return (p >= 0x80000000 && uint64_t(p) + size <= 0x81800000) ||
               (p >= 0x90000000 && uint64_t(p) + size <= 0x94000000);
    };
    const auto fader = [&](const char* name, uint32_t p) {
        std::cout << " " << name << "=0x" << std::hex << p << std::dec;
        if (valid(p, 0x14)) {
            std::cout << ":status=" << ctx.mmu.read32(p + 4)
                      << ",alpha=" << unsigned(ctx.mmu.read8(p + 0x13))
                      << ",frames=" << ctx.mmu.read16(p + 0xC)
                      << "/" << ctx.mmu.read16(p + 0xA);
        }
    };
    // Read on the guest CPU thread, without modifying registers or memory.
    // Offsets are from the original SOUE01 assembly, not inferred host layout.
    const uint32_t boot = ctx.mmu.read32(0x805756B0);
    std::cout << "[SS boot] instance=0x" << std::hex << boot << std::dec;
    if (valid(boot, 0x5E8)) {
        std::cout << " loadState=" << ctx.mmu.read32(boot + 0x560)
                  << " phaseTable=0x" << std::hex << ctx.mmu.read32(boot + 0x568)
                  << " phaseCurrent=0x" << ctx.mmu.read32(boot + 0x56C) << std::dec
                  << " stage=" << ctx.mmu.read32(boot + 0x5D4)
                  << " strapSeen=" << unsigned(ctx.mmu.read8(boot + 0x5DC))
                  << " calibrated=" << unsigned(ctx.mmu.read8(boot + 0x5DD))
                  << " allData=" << unsigned(ctx.mmu.read8(boot + 0x5DE))
                  << " objectData=" << unsigned(ctx.mmu.read8(boot + 0x5DF))
                  << " visible=" << unsigned(ctx.mmu.read8(boot + 0x5E1));
        fader("bootFader", ctx.mmu.read32(boot + 0x5B0));
    }
    fader("sceneFader", ctx.mmu.read32(0x805A06FC));
    const uint32_t manager = ctx.mmu.read32(0x805751E8);
    if (valid(manager, 0x28)) {
        std::cout << " padMode=" << ctx.mmu.read32(manager + 0x0C)
                  << " padStep=" << ctx.mmu.read32(manager + 0x10)
                  << " padCalibrated=" << unsigned(ctx.mmu.read8(manager + 0x21))
                  << " padSkipped=" << unsigned(ctx.mmu.read8(manager + 0x24));
    }
    const uint32_t ex = 0x80597A80;
    std::cout << " exState=" << ctx.mmu.read32(ex + 0x2280)
              << " probe=" << int32_t(ctx.mmu.read32(ex + 0x24))
              << " device=" << ctx.mmu.read32(ex + 0x30)
              << " enabled=" << unsigned(ctx.mmu.read8(ex + 0x52))
              << " work=" << ctx.mmu.read_f32(ex + 0x40)
              << " request=" << unsigned(ctx.mmu.read8(kKpadBase + 0x679))
              << " configured=" << unsigned(input_state.motion[0].mode)
              << " pending=" << input_state.motion[0].pending;
    std::cout << std::endl;
    const uint32_t title = ctx.mmu.read32(0x805756B4);
    if (valid(title, 0xB0)) {
        // Original dTitleMgr::getCurrentStateID and CoreController::downTrigger.
        const uint32_t state = ctx.mmu.read32(title + 0x9C);
        const uint32_t name = valid(state, 8) ? ctx.mmu.read32(state + 4) : 0;
        std::ostringstream line;
        line << "[SS title] instance=0x" << std::hex << title
             << " state=0x" << state << std::dec << " name=";
        for (uint32_t i = 0; i < 96 && valid(name + i, 1); ++i) {
            const uint8_t c = ctx.mmu.read8(name + i);
            if (!c) break;
            line << ((c >= 32 && c < 127) ? char(c) : '?');
        }
        const uint32_t core = ctx.mmu.read32(0x80575C58);
        line << " substate=" << ctx.mmu.read32(title + 0xAC)
             << " triggered=0x" << std::hex
             << (valid(core, 0x20) ? ctx.mmu.read32(core + 0x1C) : 0)
             << std::dec << '\n';
        std::cout << line.str();
    }
    constexpr uint32_t thp = 0x805CD440; // ELF ActivePlayer
    if (ctx.mmu.read32(thp + 0xA0)) {
        // THPPlayerDrawCurrentFrame loads dispTextureSet at +F8 and its
        // frameNumber at +C. These observations never advance the movie.
        const uint32_t texture = ctx.mmu.read32(thp + 0xF8);
        std::cout << "[SS THP] open=1 state=" << unsigned(ctx.mmu.read8(thp + 0xA4))
                  << " internalState=" << unsigned(ctx.mmu.read8(thp + 0xA5))
                  << " dvdError=" << int32_t(ctx.mmu.read32(thp + 0xA8))
                  << " videoError=" << int32_t(ctx.mmu.read32(thp + 0xAC))
                  << " decodeQueue=" << int32_t(ctx.mmu.read32(thp + 0xD8))
                  << " videoNumber=" << int32_t(ctx.mmu.read32(thp + 0xF0))
                  << " audioNumber=" << int32_t(ctx.mmu.read32(thp + 0xF4))
                  << " displayedFrame=" << (valid(texture, 0x10) ? int32_t(ctx.mmu.read32(texture + 0xC)) : -1)
                  << std::endl;
    }
}

// NWII_SS_INPUT_REPLAY=new-game / new-game-long / load-save (LOAD14) only: publish the dTitleMgr_c phase (same
// read-only observation as sample_boot_state) for the scripted replay source.
void publish_replay_title_phase(CPUContext& ctx) {
    static const bool enabled = [] {
        const char* replay = std::getenv("NWII_SS_INPUT_REPLAY");
        return replay && (std::strcmp(replay, "new-game") == 0 || std::strcmp(replay, "new-game-long") == 0 ||
                          std::strcmp(replay, "load-save") == 0);
    }();
    static const bool load_save = [] {
        const char* replay = std::getenv("NWII_SS_INPUT_REPLAY");
        return replay && std::strcmp(replay, "load-save") == 0;
    }();
    if (!enabled || ctx.gpr[3] != 0) return; // once per frame: channel 0 read
    using nwii::runtime::input::g_replay_title_phase;
    const auto valid = [](uint32_t p, uint32_t size) {
        return (p >= 0x80000000 && uint64_t(p) + size <= 0x81800000) ||
               (p >= 0x90000000 && uint64_t(p) + size <= 0x94000000);
    };
    const uint32_t title = ctx.mmu.read32(0x805756B4);
    const uint32_t state = valid(title, 0xB0) ? ctx.mmu.read32(title + 0x9C) : 0;
    const uint32_t name = valid(state, 8) ? ctx.mmu.read32(state + 4) : 0;
    char text[48] = {};
    for (uint32_t i = 0; i + 1 < sizeof(text) && valid(name + i, 1); ++i)
        if (!(text[i] = static_cast<char>(ctx.mmu.read8(name + i)))) break;
    int phase = nwii::runtime::input::kPhaseUnknown;
    if (std::strcmp(text, "dTitleMgr_c::StateID_Input") == 0) phase = nwii::runtime::input::kPhaseInput;
    else if (std::strcmp(text, "dTitleMgr_c::StateID_FileSelect") == 0) phase = nwii::runtime::input::kPhaseFileSelect;
    else if (text[0]) phase = nwii::runtime::input::kPhaseOther;
    g_replay_title_phase.store(phase);
    // dPad::ex_c::m_current_ex (.sdata 0x80571C60) -> pos.x +0, pos.y +4, as
    // read by the keyboard REL (d_SoftwareKeyboardNP fn_2_BB0).
    const uint32_t ex = ctx.mmu.read32(0x80571C60);
    if (valid(ex, 0x2290)) {
        nwii::runtime::input::g_replay_pointer_x.store(ctx.mmu.read_f32(ex));
        nwii::runtime::input::g_replay_pointer_y.store(ctx.mmu.read_f32(ex + 4));
        // Every 16th frame in name entry: the MotionPlus pointer inputs of
        // dPad::ex_c (d_pad.h): field_0x50 (MPLS pointer mode), field_0x5C
        // (MPLS pointer pos), mMPLSVelocity +0x2250, mMPLS (+0x225C) mZ at +0x2274.
        if ((phase == nwii::runtime::input::kPhaseInput ||
             (load_save && phase == nwii::runtime::input::kPhaseFileSelect)) &&
            (nwii::runtime::input::g_replay_pointer_frames.load() & 15) == 0) {
            std::printf("[SS pointer] dpd=(%.3f,%.3f) mode50=%u p5C=(%.3f,%.3f) vel=(%.3f,%.3f,%.3f) "
                        "mZ=(%.3f,%.3f,%.3f) state=%u\n",
                        ctx.mmu.read_f32(ex), ctx.mmu.read_f32(ex + 4), unsigned(ctx.mmu.read8(ex + 0x50)),
                        ctx.mmu.read_f32(ex + 0x5C), ctx.mmu.read_f32(ex + 0x60), ctx.mmu.read_f32(ex + 0x2250),
                        ctx.mmu.read_f32(ex + 0x2254), ctx.mmu.read_f32(ex + 0x2258), ctx.mmu.read_f32(ex + 0x2274),
                        ctx.mmu.read_f32(ex + 0x2278), ctx.mmu.read_f32(ex + 0x227C), ctx.mmu.read32(ex + 0x2280));
            std::fflush(stdout);
        }
    }
    nwii::runtime::input::g_replay_pointer_frames.fetch_add(1);
}

// CTRL18-INPUT (read-only): the control translator's game context (src/input/game_context.h), every frame on the
// channel-0 read. Addresses: see game_context.h. NWII_CTRL18_CONTEXT=0 publishes nothing (A/B: the translator then
// treats everything as plain play / the live atomics).
static void publish_game_context(CPUContext& ctx, uint32_t link, bool link_ok, bool title) {
    static const bool on = [] { const char* e = std::getenv("NWII_CTRL18_CONTEXT"); return !(e && e[0] == '0'); }();
    if (!on) return;
    const auto valid = [](uint32_t p, uint32_t size) {
        return (p >= 0x80000000 && uint64_t(p) + size <= 0x81800000) ||
               (p >= 0x90000000 && uint64_t(p) + size <= 0x94000000);
    };
    static uint32_t frame = 0;
    nwii::runtime::input::GameContext c{};
    c.frame = ++frame;
    c.valid = true;
    c.title = title;
    c.player = link_ok && !title;
    if (link_ok) {
        c.riding = ctx.mmu.read8(link + 0x339);
        c.sword_states = ctx.mmu.read32(link + 0x348);
        c.action_flags = ctx.mmu.read32(link + 0x364);
        c.aim_flags = ctx.mmu.read32(link + 0x368);
        c.action = static_cast<int32_t>(ctx.mmu.read32(link + 0x36C));
        c.item_state = ctx.mmu.read16(link + 0x43CA);
    }
    if (const uint32_t events = ctx.mmu.read32(0x80575378); valid(events, 0x188))
        c.event_state = static_cast<int32_t>(ctx.mmu.read32(events + 0x184));
    {
        // The A prompt as SOAK14 reads it (game_observation.h ss_a_button_prompt).
        int32_t related = -1, dob_act = -1, dob_icon = -1;
        bool have = false;
        if (const uint32_t rel = ctx.mmu.read32(0x80575478); valid(rel, 0x40)) {
            related = int32_t(ctx.mmu.read32(rel + 0x04));
            for (int k = 0; k < 11; ++k) c.prompt_slot[k] = int32_t(ctx.mmu.read32(rel + 0x04 + 4 * k));
            have = true;
        }
        if (const uint32_t dob = ctx.mmu.read32(0x80575468); valid(dob, 0x494)) {
            dob_act = int32_t(ctx.mmu.read32(dob + 0x47C));
            dob_icon = int32_t(ctx.mmu.read32(dob + 0x480));
            have = true;
        }
        if (have) c.prompt_a = nwii::runtime::input::ss_a_button_prompt(related, dob_act, dob_icon);
        c.dob_act = dob_act;
        c.dob_icon = dob_icon;
    }
    if (link_ok) {
        // Link's own HUD requests: effective = cur != -1 ? cur : prev.
        for (int k = 0; k < 6; ++k) {
            const int16_t cur = static_cast<int16_t>(ctx.mmu.read16(link + 0x43FE + 4 * k));
            const int16_t prev = static_cast<int16_t>(ctx.mmu.read16(link + 0x4400 + 4 * k));
            c.link_request[k] = cur != -1 ? cur : prev;
        }
        c.wheel_index = static_cast<int8_t>(ctx.mmu.read8(link + 0x43B1));
        c.stick_angle = static_cast<int16_t>(ctx.mmu.read16(link + 0x441E));
        c.move_angle = static_cast<int16_t>(ctx.mmu.read16(link + 0x4420));
        c.equipped_b = ctx.mmu.read16(link + 0x43CE);
        if (const uint32_t carried = ctx.mmu.read32(link + 0x1368); valid(carried, 0x10))
            c.carried_profile = ctx.mmu.read16(carried + 0x8);
    }
    // The meter parts' latched copies of the other HUD slots (one frame old; LytDoButtonRelated itself is NONE here).
    if (const uint32_t m = ctx.mmu.read32(0x80575448); valid(m, 0x13B60)) {
        const int32_t z_has = int32_t(ctx.mmu.read32(m + 0x12530));
        c.prompt_slot[3] = z_has ? int32_t(ctx.mmu.read32(m + 0x1252C)) : nwii::runtime::input::kPromptNone;
        c.prompt_slot[1] = int32_t(ctx.mmu.read32(m + 0x757C));
        c.prompt_slot[2] = int32_t(ctx.mmu.read32(m + 0x12188));
        c.prompt_slot[4] = int32_t(ctx.mmu.read32(m + 0x126B4));
        c.prompt_slot[5] = int32_t(ctx.mmu.read32(m + 0x127A8));
        c.prompt_slot[6] = int32_t(ctx.mmu.read32(m + 0x1296C));
        for (int k = 0; k < 4; ++k) c.prompt_slot[7 + k] = int32_t(ctx.mmu.read32(m + 0x7A5C + 0x80 * k));
        c.prompt_plus = int32_t(ctx.mmu.read32(m + 0x7DC8));
        if (const uint32_t skip = ctx.mmu.read32(m + 0x374); valid(skip, 0x164)) c.skip_shown = ctx.mmu.read8(skip + 0x160) != 0;
    }
    c.prompt_slot[0] = c.prompt_a;
    if (const uint32_t ex = ctx.mmu.read32(0x80571C60); valid(ex, 0x2280)) {
        for (int k = 0; k < 3; ++k) c.game_mz[k] = ctx.mmu.read_f32(ex + 0x2274 + 4 * k);
        c.have_mz = std::isfinite(c.game_mz[0]) && std::isfinite(c.game_mz[1]) && std::isfinite(c.game_mz[2]);
    }
    if (const uint32_t cam = ctx.mmu.read32(0x80575788); valid(cam, 0xDF0)) {
        c.cam_mode = static_cast<int32_t>(ctx.mmu.read32(cam + 0x2B4 + 0x6C));
        c.cam_handler = static_cast<int32_t>(ctx.mmu.read32(cam + 0x2B4 + 0x74));
    }
    nwii::runtime::input::g_guest_context.publish(c);
}

// Read-only gameplay observation (src/input/game_observation.h), once per frame (channel 0 read):
// - always: g_guest_player_present (dAcPy_c::LINK != 0 outside the title scene), used by live sources to
//   enable right-stick sword gestures;
// - NWII_SS_INPUT_REPLAY=new-game / new-game-long or NWII_SS_GAME_TRACE=1 (startup-cached): the full
//   observation for the test replay, plus "[SS game]" (stage/room/layer/event changes) and "[SS player]"
//   (action and attack changes; position every 60 frames) log lines. Nothing is written to guest memory.
void publish_game_observation(CPUContext& ctx) {
    static const bool enabled = [] {
        const char* replay = std::getenv("NWII_SS_INPUT_REPLAY");
        const char* trace = std::getenv("NWII_SS_GAME_TRACE");
        return (replay && (std::strcmp(replay, "new-game") == 0 || std::strcmp(replay, "new-game-long") == 0 ||
                           std::strcmp(replay, "load-save") == 0)) ||
               (trace && trace[0] && trace[0] != '0');
    }();
    if (ctx.gpr[3] != 0) return; // channel 0 read only
    const auto valid = [](uint32_t p, uint32_t size) {
        return (p >= 0x80000000 && uint64_t(p) + size <= 0x81800000) ||
               (p >= 0x90000000 && uint64_t(p) + size <= 0x94000000);
    };
    const uint32_t link = ctx.mmu.read32(0x8057578C); // dAcPy_c::LINK
    const bool link_ok = valid(link, 0x4620);
    // The title screen (dTitleMgr_c::sInstance 0x805756B4 set) also spawns a Link actor; its menus
    // keep the plain pointer behaviour.
    const bool title = valid(ctx.mmu.read32(0x805756B4), 4);
    nwii::runtime::input::g_guest_player_present.store(link_ok && !title);
    publish_game_context(ctx, link, link_ok, title); // CTRL18-INPUT (read-only)
    // Read-only progress and bug-report context, also when verbose game tracing is disabled.
    nwii::runtime::progress::Snapshot progress;
    progress.playing = link_ok && !title && valid(ctx.mmu.read32(0x805753FC), 0x20);
    const uint32_t spawn_progress = 0x805B3860;
    for (uint32_t i = 0; i < 8; ++i)
        if (!(progress.stage[i] = char(ctx.mmu.read8(spawn_progress + i)))) break;
    progress.room = ctx.mmu.read8(spawn_progress + 0x22);
    progress.layer = ctx.mmu.read8(spawn_progress + 0x23);
    progress.entrance = ctx.mmu.read8(spawn_progress + 0x24);
    if (link_ok) for (int k = 0; k < 3; ++k) progress.pos[k] = ctx.mmu.read_f32(link + 0xC0 + 4 * k);
    if (progress.playing) {
        const auto story_flag = [&](uint16_t flag) {
            const uint32_t def = 0x80510D90 + 2 * flag;
            const uint8_t index = ctx.mmu.read8(def), sm = ctx.mmu.read8(def + 1);
            return index < 128 && ((ctx.mmu.read16(0x805A9AD8 + 2 * index) >> (sm >> 4)) & ((2u << (sm & 15)) - 1));
        };
        for (size_t i = 0; i < nwii::runtime::progress::milestones.size(); ++i)
            if (nwii::runtime::progress::attained(nwii::runtime::progress::milestones[i], story_flag)) progress.mask |= 1u << i;
    }
    nwii::runtime::progress::channel.publish(progress);
    if (!enabled) return;
    static nwii::runtime::input::GameObservation last{};
    static uint32_t frame = 0;
    nwii::runtime::input::GameObservation o{};
    o.frame = ++frame;
    const uint32_t spawn = 0x805B3860; // dScGame_c::currentSpawnInfo
    for (uint32_t i = 0; i + 1 < sizeof(o.stage); ++i)
        if (!(o.stage[i] = static_cast<char>(ctx.mmu.read8(spawn + i)))) break;
    o.room = ctx.mmu.read8(spawn + 0x22);
    o.layer = ctx.mmu.read8(spawn + 0x23);
    o.entrance = ctx.mmu.read8(spawn + 0x24);
    o.link_present = link_ok;
    if (link_ok) {
        for (int k = 0; k < 3; ++k) o.pos[k] = ctx.mmu.read_f32(link + 0xC0 + 4 * k);
        o.rot_y = static_cast<int16_t>(ctx.mmu.read16(link + 0xBA));
        o.attack_direction = ctx.mmu.read8(link + 0x338);
        o.specific_attack_direction = ctx.mmu.read16(link + 0x33A);
        o.sword_states = ctx.mmu.read32(link + 0x348);
        o.action_flags = ctx.mmu.read32(link + 0x364);
        o.current_action = static_cast<int32_t>(ctx.mmu.read32(link + 0x36C));
        o.swing[0] = ctx.mmu.read_f32(link + 0x4608);
        o.swing[1] = ctx.mmu.read_f32(link + 0x460C);
        o.nunchuk_motion = ctx.mmu.read8(link + 0x4364);
        o.remote_motion = ctx.mmu.read8(link + 0x4365);
        o.attack_angle = static_cast<int16_t>(ctx.mmu.read16(link + 0x444C));
        o.item_state = ctx.mmu.read16(link + 0x43CA);
    }
    o.current_sword = ctx.mmu.read8(0x80571C74); // daPlayerActBase_c::sCurrentSword
    const uint32_t events = ctx.mmu.read32(0x80575378); // EventManager::sInstance
    if (valid(events, 0x188)) {
        o.event_state = static_cast<int32_t>(ctx.mmu.read32(events + 0x184));
        if (o.event_state != 0)
            for (uint32_t i = 0; i + 1 < sizeof(o.event_name); ++i)
                if (!(o.event_name[i] = static_cast<char>(ctx.mmu.read8(events + 0x94 + i)))) break;
    }
    // SOAK14 (read-only): A-button prompt (game_observation.h ss_a_button_prompt): LytDoButtonRelated::sInstance +0x04
    // (DO_BUTTON_A, reset to NONE every frame by the meter) and dLytDobutton_c::sInstance latched request +0x47C/+0x480;
    // mDoActionToShow +0x470. A/B: NWII_SS_PROMPT_LATCHED=0 = LytDoButtonRelated only (pre-fix: never saw a prompt).
    {
        static const bool latched = [] { const char* e = std::getenv("NWII_SS_PROMPT_LATCHED"); return !(e && e[0] == '0'); }();
        int32_t related = -1, dob_act = -1, dob_icon = -1;
        bool have = false;
        if (const uint32_t rel = ctx.mmu.read32(0x80575478); valid(rel, 0x40)) {
            related = int32_t(ctx.mmu.read32(rel + 0x04));
            have = true;
        }
        if (const uint32_t dob = ctx.mmu.read32(0x80575468); valid(dob, 0x494)) {
            o.do_shown = int32_t(ctx.mmu.read32(dob + 0x470));
            dob_act = int32_t(ctx.mmu.read32(dob + 0x47C));
            dob_icon = int32_t(ctx.mmu.read32(dob + 0x480));
            have = true;
        }
        if (have)
            o.do_action = latched ? nwii::runtime::input::ss_a_button_prompt(related, dob_act, dob_icon) : related;
    }
    // LOAD14 (read-only): FileManager::sInstance (.sbss 0x80574FFC), toBeSorted/file_manager.h / save_file.h.
    const uint32_t files = ctx.mmu.read32(0x80574FFC);
    if (valid(files, 0xA850)) {
        o.file_manager = true;
        o.selected_file = ctx.mmu.read8(files + 0xA83C); // mSelectedFile
        const uint32_t current = files + 0x8;            // mFileA (getCurrentFile outside the title)
        if (!title) {
            o.health = ctx.mmu.read16(current + 0x5306);
            o.health_capacity = ctx.mmu.read16(current + 0x5302);
            // RupeeCounter (item counter 0x1F5) through the game's ItemflagManager__ITEMFLAG_DEFINITIONS (.data
            // 0x805116F4, 0x1FE entries of {u8 index, u8 shift<<4 | bits-1}) on mFileA.item_flags (+0x9E4, 64 words).
            const uint32_t def = 0x805116F4 + 2 * 0x1F5;
            const uint8_t index = ctx.mmu.read8(def), shift_mask = ctx.mmu.read8(def + 1);
            if (index < 64)
                o.rupees = (ctx.mmu.read16(current + 0x9E4 + 2 * index) >> (shift_mask >> 4)) &
                           ((2u << (shift_mask & 0xF)) - 1);
        }
        const uint32_t saved = ctx.mmu.read32(files); // mpSavedSaveFiles
        if (valid(saved, 0xFBE0)) {
            for (int i = 0; i < 3; ++i) {
                const uint32_t f = saved + 0x20 + 0x53C0 * i;
                auto& slot = o.slots[i];
                slot.read = true;
                slot.new_file = ctx.mmu.read8(f + 0x53AD);
                slot.play_time = static_cast<int64_t>((uint64_t(ctx.mmu.read32(f)) << 32) | ctx.mmu.read32(f + 4));
                slot.saved_time = static_cast<int64_t>((uint64_t(ctx.mmu.read32(f + 8)) << 32) | ctx.mmu.read32(f + 12));
                for (uint32_t k = 0; k + 1 < sizeof(slot.name); ++k) {
                    const uint16_t c = ctx.mmu.read16(f + 0x8D4 + 2 * k);
                    if (!c) break;
                    slot.name[k] = c < 0x80 ? static_cast<char>(c) : '?';
                }
                for (uint32_t k = 0; k + 1 < sizeof(slot.stage); ++k)
                    if (!(slot.stage[k] = static_cast<char>(ctx.mmu.read8(f + 0x531C + k)))) break;
                slot.room = ctx.mmu.read16(f + 0x5308);
                slot.layer = ctx.mmu.read8(f + 0x53A5);
                slot.entrance = ctx.mmu.read8(f + 0x53A8);
                for (int k = 0; k < 3; ++k) slot.pos[k] = ctx.mmu.read_f32(f + 0x10 + 4 * k);
                slot.angle = static_cast<int16_t>(ctx.mmu.read16(f + 0x530E));
                slot.health = ctx.mmu.read16(f + 0x5306);
                slot.health_capacity = ctx.mmu.read16(f + 0x5302);
            }
        }
    }
    nwii::runtime::input::g_game_observation.publish(o);
    // LOAD14: "[SS save]" when a slot of the save buffer changes (first read, a load, the game's own save) and
    // "[SS file]" when the current file's hearts/rupees or the selected file change.
    for (int i = 0; i < 3; ++i) {
        const auto& s = o.slots[i];
        const auto& p = last.slots[i];
        if (s.read && (!p.read || s.new_file != p.new_file || s.saved_time != p.saved_time ||
                       s.play_time != p.play_time || std::strcmp(s.stage, p.stage) != 0)) {
            std::printf("[SS save] frame=%u slot=%d new_file=%u name='%s' saved_time=%lld play_s=%.1f stage=%s room=%d "
                        "layer=%d entrance=%d pos=(%.1f,%.1f,%.1f) angle=%d hearts=%d/%d\n", o.frame, i,
                        unsigned(s.new_file), s.name, static_cast<long long>(s.saved_time), s.play_time / 60750000.0,
                        s.stage, s.room, s.layer, s.entrance, s.pos[0], s.pos[1], s.pos[2], int(s.angle), s.health,
                        s.health_capacity);
            std::fflush(stdout);
        }
    }
    if (o.file_manager && (o.health != last.health || o.health_capacity != last.health_capacity ||
                           o.rupees != last.rupees || o.selected_file != last.selected_file)) {
        std::printf("[SS file] frame=%u selected=%d hearts=%d/%d (quarters) rupees=%d sword=%u\n", o.frame,
                    o.selected_file, o.health, o.health_capacity, o.rupees, unsigned(o.current_sword));
        std::fflush(stdout);
    }
    if (std::strcmp(o.stage, last.stage) != 0 || o.room != last.room || o.layer != last.layer ||
        o.link_present != last.link_present || o.event_state != last.event_state ||
        std::strcmp(o.event_name, last.event_name) != 0) {
        std::printf("[SS game] frame=%u stage=%s room=%d layer=%d entrance=%d link=%d event=%d '%s' sword=%u\n", o.frame,
                    o.stage, o.room, o.layer, o.entrance, o.link_present ? 1 : 0, o.event_state, o.event_name, unsigned(o.current_sword));
        std::fflush(stdout);
    }
    if (o.link_present && (o.current_action != last.current_action || o.attack_direction != last.attack_direction ||
                           o.specific_attack_direction != last.specific_attack_direction ||
                           ((o.sword_states ^ last.sword_states) & 0x7) || (o.remote_motion & ~last.remote_motion) ||
                           (o.nunchuk_motion & ~last.nunchuk_motion) || o.attack_angle != last.attack_angle ||
                           o.item_state != last.item_state || o.frame % 60 == 0)) {
        std::printf("[SS player] frame=%u action=%d attack=%u specific=0x%X sword=0x%X flags=0x%X motion=0x%02X/0x%02X "
                    "swing=(%.2f,%.2f) angle=%d item=0x%X pos=(%.1f,%.1f,%.1f) roty=%d\n", o.frame, o.current_action,
                    unsigned(o.attack_direction), unsigned(o.specific_attack_direction), o.sword_states, o.action_flags,
                    unsigned(o.remote_motion), unsigned(o.nunchuk_motion), o.swing[0], o.swing[1], int(o.attack_angle),
                    unsigned(o.item_state), o.pos[0], o.pos[1], o.pos[2], int(o.rot_y));
        std::fflush(stdout);
    }
    // SWING11 diagnostic (NWII_SS_SWING_DIAG=1, startup-cached; read-only): the swing above is the guest
    // mVec3_c LINK+0x4608, written by fn_8023BDE0 as dir * sqrt(x^2+y^2) of LINK+0x45FC (copied from
    // dPad::ex_c +0x2250) via nw4r::math::FrSqrt, which overflows to inf when x^2+y^2 is single-subnormal
    // (Gekko with FPSCR[NI]=1 flushes that to 0). Logs raw bits and FPSCR whenever the state changes between
    // normal / input in the subnormal range / output non-finite; at most 32 lines.
    static const bool swing_diag = [] {
        const char* e = std::getenv("NWII_SS_SWING_DIAG");
        return e && e[0] == '1';
    }();
    if (swing_diag && link_ok) {
        static int swing_state = 0, swing_reports = 0;
        uint32_t in[3], out[3];
        float inf[3];
        for (int k = 0; k < 3; ++k) {
            in[k] = ctx.mmu.read32(link + 0x45FC + 4 * k);
            out[k] = ctx.mmu.read32(link + 0x4608 + 4 * k);
            std::memcpy(&inf[k], &in[k], 4);
        }
        const double m2 = double(inf[0]) * inf[0] + double(inf[1]) * inf[1];
        const int state = (!std::isfinite(o.swing[0]) || !std::isfinite(o.swing[1])) ? 2
                          : (m2 > 0.0 && m2 < 0x1p-126)                               ? 1
                                                                                         : 0;
        if (state != swing_state && swing_reports < 32) {
            ++swing_reports;
            std::printf("[SS swing] frame=%u state=%s fpscr=0x%08X action=%d in=(%08X,%08X,%08X)=(%g,%g,%g) "
                        "x2+y2=%g out=(%08X,%08X,%08X)\n", o.frame,
                        state == 2 ? "out-nonfinite" : state == 1 ? "in-subnormal-range" : "normal",
                        unsigned(ctx.fpscr), o.current_action, in[0], in[1], in[2], inf[0], inf[1], inf[2], m2,
                        out[0], out[1], out[2]);
            std::fflush(stdout);
        }
        swing_state = state;
    }
    // Diagnostic: Link's g3d world matrices (bmdl_c at +0x3E0 -> scnLeaf_c::mpScnLeaf +0x14 ->
    // ScnMdlSimple::mpWorldMtxArray +0xEC); report the first frame on which one contains a NaN.
    if (link_ok) {
        static uint32_t scanned_link = 0;
        static int mtx_reports = 0;
        static bool mtx_was_finite = true;
        if (link != scanned_link) { scanned_link = link; mtx_was_finite = true; }
        const uint32_t leaf = ctx.mmu.read32(link + 0x3E0 + 0x14);
        const uint32_t arr = valid(leaf, 0xF0) ? ctx.mmu.read32(leaf + 0xEC) : 0;
        if (valid(arr, 64 * 48)) {
            int first_bad = -1, bad = 0;
            for (int m = 0; m < 64; ++m) {
                bool sane = true, nan = false;
                for (int e = 0; e < 12; ++e) {
                    const float v = ctx.mmu.read_f32(arr + m * 48 + e * 4);
                    if (std::isnan(v)) nan = true;
                    else if (!(std::fabs(v) < 1e7f)) sane = false;
                }
                if (nan && sane) { ++bad; if (first_bad < 0) first_bad = m; }
            }
            if (bad && mtx_was_finite && mtx_reports < 4) {
                ++mtx_reports;
                std::printf("[SS player NaN] world matrices frame=%u leaf=0x%08X array=0x%08X nan_matrices=%d first=%d "
                            "pos=(%g,%g,%g)\n", o.frame, leaf, arr, bad, first_bad, o.pos[0], o.pos[1], o.pos[2]);
                // ScnObj::mMtxArray (+0xC): local, world (model base matrices); callback +0xD4.
                const auto m12 = [&](uint32_t a) {
                    std::string r;
                    char b[24];
                    for (int e = 0; e < 12; ++e) { std::snprintf(b, sizeof(b), "%s%g", e ? "," : "", ctx.mmu.read_f32(a + e * 4)); r += b; }
                    return r;
                };
                std::printf("[SS player NaN] scnobj local=[%s] world=[%s] callback=0x%08X root=[%s]\n",
                            m12(leaf + 0xC).c_str(), m12(leaf + 0x3C).c_str(), ctx.mmu.read32(leaf + 0xD4),
                            m12(arr).c_str());
                std::fflush(stdout);
            }
            mtx_was_finite = bad == 0;
        }
    }
    // Diagnostic: the first frames on which the player's position is not finite (seen in F000 L20 and
    // F001r: X/Z become NaN, Link then "falls" and FallRestart runs). Logs the actor's motion inputs.
    if (link_ok) {
        static int nan_reports = 0;
        static uint32_t last_link = 0;
        static bool was_finite = true;
        if (link != last_link) { last_link = link; was_finite = true; }
        const bool finite = std::isfinite(o.pos[0]) && std::isfinite(o.pos[1]) && std::isfinite(o.pos[2]);
        if (!finite && was_finite && nan_reports < 6) {
            ++nan_reports;
            const auto f3 = [&](uint32_t off) {
                char b[96];
                std::snprintf(b, sizeof(b), "(%g,%g,%g)", ctx.mmu.read_f32(link + off), ctx.mmu.read_f32(link + off + 4),
                              ctx.mmu.read_f32(link + off + 8));
                return std::string(b);
            };
            std::printf("[SS player NaN] frame=%u link=0x%08X action=%d prev_pos=(%g,%g,%g) pos=%s old=%s copy=%s "
                        "speed=%g accel=%g maxspeed=%g vel=%s angle=(%d,%d,%d) rot=(%d,%d,%d) mtxT=(%g,%g,%g) "
                        "start=%s\n", o.frame, link, o.current_action, last.pos[0], last.pos[1], last.pos[2],
                        f3(0xC0).c_str(), f3(0x118).c_str(), f3(0x9C).c_str(), ctx.mmu.read_f32(link + 0x144),
                        ctx.mmu.read_f32(link + 0x148), ctx.mmu.read_f32(link + 0x14C), f3(0x150).c_str(),
                        int(int16_t(ctx.mmu.read16(link + 0x13C))), int(int16_t(ctx.mmu.read16(link + 0x13E))),
                        int(int16_t(ctx.mmu.read16(link + 0x140))), int(int16_t(ctx.mmu.read16(link + 0xB8))),
                        int(int16_t(ctx.mmu.read16(link + 0xBA))), int(int16_t(ctx.mmu.read16(link + 0xBC))),
                        ctx.mmu.read_f32(link + 0x15C + 12), ctx.mmu.read_f32(link + 0x15C + 28),
                        ctx.mmu.read_f32(link + 0x15C + 44), f3(0x1FC).c_str());
            // Inputs of the player's speed update (d_a_player.s 0x8023DA40: speed = [44A8]*(1-|[44B0]|)
            // -/+ [44B4]*(1-[418])*[44B0]) and of handlesInputMaybe (stick distance 44A4, angle 441E).
            std::printf("[SS player NaN] inputs 44A4=%g 44A8=%g 44AC=%g 44B0=%g 44B4=%g 418=%g 441E=%d 4420=%d 434E=%u\n",
                        ctx.mmu.read_f32(link + 0x44A4), ctx.mmu.read_f32(link + 0x44A8), ctx.mmu.read_f32(link + 0x44AC),
                        ctx.mmu.read_f32(link + 0x44B0), ctx.mmu.read_f32(link + 0x44B4), ctx.mmu.read_f32(link + 0x418),
                        int(int16_t(ctx.mmu.read16(link + 0x441E))), int(int16_t(ctx.mmu.read16(link + 0x4420))),
                        unsigned(ctx.mmu.read8(link + 0x434E)));
            // fn_8023D5B0 derives [44B4] from model nodes 0x15/0x1A (m3d::bmdl_c at +0x3E0) concatenated with
            // the matrix at +0x495C, against the previous node positions at +0x4680/+0x468C.
            std::printf("[SS player NaN] mtx495C=[%s %s %s] prevNodes=%s %s\n",
                        f3(0x495C).c_str(), f3(0x496C).c_str(), f3(0x497C).c_str(), f3(0x4680).c_str(),
                        f3(0x468C).c_str());
            std::fflush(stdout);
        }
        was_finite = finite;
    }
    last = o;
}

// NWII_SS_CAM_TRACE=1 (startup-cached; =N prints every N frames, default 15): read-only "[SS cam]" lines,
// once per frame (channel 0 read), while LINK exists outside the title. Layout (decomp d_camera.h,
// d_camera_base.h, d_camera_game.h): dScGame_c CAMERAS[0] (0x80575788) -> dCamera_c: mView +0x6C,
// mFlags +0x2B0, mGameCam1 +0x2B4, mEventCam +0xB9C, mActiveCameraIdx +0xDA8. dCameraGame_c: mIsActive +2,
// mView +8, mFlags +0x58, CamId +0x60, trend +0x68, mode +0x6C, style +0x70, handler +0x74, actor +0x98,
// player distance +0xB0, last player pos +0x234. g3d camera: m3d l_scnRoot_p (0x80575BD4) +0xF4 index,
// CameraData +0xF8 + i*0x10C: view matrix +0. Also reports any write to .sdata2 (read-only constants). LINK +0xC0 position, +0x130 (read by the camera).
void publish_camera_trace(CPUContext& ctx) {
    static const uint32_t every = [] {
        const char* e = std::getenv("NWII_SS_CAM_TRACE");
        if (!e || !e[0] || e[0] == '0') return 0u;
        const long n = std::strtol(e, nullptr, 10);
        return n > 1 ? static_cast<uint32_t>(n) : 15u;
    }();
    if (!every || ctx.gpr[3] != 0) return;
    static uint32_t frame = 0;
    ++frame;
    // .sdata2 (main.dol section 0x805769C0, 0x95E0 bytes: read-only float/double constants such as
    // d_camera_game's 1.0f at 0x80579258) is snapshotted on the first frame; any later change is
    // reported (first 32), since a guest never writes it.
    static std::vector<uint32_t> sdata2;
    static int sdata2_reports = 0;
    constexpr uint32_t kSdata2 = 0x805769C0, kSdata2Words = 0x95E0 / 4;
    if (sdata2.empty()) {
        sdata2.resize(kSdata2Words);
        for (uint32_t i = 0; i < kSdata2Words; ++i) sdata2[i] = ctx.mmu.read32(kSdata2 + 4 * i);
    } else if (sdata2_reports < 32) {
        for (uint32_t i = 0; i < kSdata2Words && sdata2_reports < 32; ++i)
            if (const uint32_t w = ctx.mmu.read32(kSdata2 + 4 * i); w != sdata2[i]) {
                std::printf("[SS cam] frame=%u .sdata2 0x%08X changed 0x%08X -> 0x%08X\n", frame, kSdata2 + 4 * i,
                            sdata2[i], w);
                sdata2[i] = w;
                ++sdata2_reports;
            }
    }
    const auto valid = [](uint32_t p, uint32_t size) {
        return (p >= 0x80000000 && uint64_t(p) + size <= 0x81800000) ||
               (p >= 0x90000000 && uint64_t(p) + size <= 0x94000000);
    };
    const uint32_t link = ctx.mmu.read32(0x8057578C);
    const uint32_t cam = ctx.mmu.read32(0x80575788);
    if (!valid(link, 0x4620) || !valid(cam, 0xDF0) || valid(ctx.mmu.read32(0x805756B4), 4)) return;
    const auto f = [&](uint32_t a) { return ctx.mmu.read_f32(a); };
    const uint32_t g = cam + 0x2B4;
    const int32_t active = static_cast<int32_t>(ctx.mmu.read32(cam + 0xDA8));
    const uint32_t gflags = ctx.mmu.read32(g + 0x58);
    const int32_t trend = static_cast<int32_t>(ctx.mmu.read32(g + 0x68));
    const int32_t mode = static_cast<int32_t>(ctx.mmu.read32(g + 0x6C));
    const int32_t style = static_cast<int32_t>(ctx.mmu.read32(g + 0x70));
    const uint32_t handler = ctx.mmu.read32(g + 0x74);
    const uint32_t camid = (uint32_t(ctx.mmu.read16(g + 0x60)) << 16) | ctx.mmu.read16(g + 0x62);
    static int32_t last_active = -2, last_trend = -2, last_mode = -2, last_style = -2;
    static uint32_t last_handler = ~0u, last_camid = ~0u;
    const bool changed = active != last_active || trend != last_trend || mode != last_mode || style != last_style ||
                         handler != last_handler || camid != last_camid;
    last_active = active; last_trend = trend; last_mode = mode; last_style = style;
    last_handler = handler; last_camid = camid;
    if (!changed && frame % every != 0) return;
    char tname[17] = {};
    const uint32_t trends = ctx.mmu.read32(0x80575358);
    if (valid(trends, 4) && trend >= 0 && trend < 256)
        for (uint32_t i = 0; i < 16; ++i)
            if (!(tname[i] = static_cast<char>(ctx.mmu.read8(trends + uint32_t(trend) * 0x24 + i)))) break;
    for (char& c : tname) if (c && (c < 0x20 || c > 0x7E)) c = '?';
    float g3d[6] = {};
    const uint32_t root = ctx.mmu.read32(0x80575BD4);
    int cid = -1;
    if (valid(root, 0x400)) {
        cid = ctx.mmu.read8(root + 0xF4);
        const uint32_t b = root + 0xF8 + uint32_t(cid) * 0x10C;
        // CameraData +0 = view matrix (3x4, set directly by EGG::LookAtCamera::setG3DCamera): eye = -R^T t,
        // view direction = -row 2.
        const auto m = [&](int r, int c) { return f(b + 4 * uint32_t(r * 4 + c)); };
        for (int k = 0; k < 3; ++k) {
            g3d[k] = -(m(0, k) * m(0, 3) + m(1, k) * m(1, 3) + m(2, k) * m(2, 3));
            g3d[3 + k] = -m(2, k);
        }
    }
    const uint32_t events = ctx.mmu.read32(0x80575378);
    const int32_t ev = valid(events, 0x188) ? static_cast<int32_t>(ctx.mmu.read32(events + 0x184)) : -1;
    std::printf("[SS cam] frame=%u%s active=%d flags=0x%X ev=%d view eye=(%.1f,%.1f,%.1f) at=(%.1f,%.1f,%.1f) fov=%.1f | "
                "game act=%u flags=0x%X camid=%d/%d trend=%d'%s' mode=%d style=%d handler=%u actor=0x%08X dist=%.1f "
                "eye=(%.1f,%.1f,%.1f) at=(%.1f,%.1f,%.1f) last=(%.1f,%.1f,%.1f) | event act=%u eye=(%.1f,%.1f,%.1f) | "
                "g3d%d eye=(%.1f,%.1f,%.1f) dir=(%.3f,%.3f,%.3f) | link pos=(%.1f,%.1f,%.1f) p130=(%.1f,%.1f,%.1f)\n",
                frame, changed ? "*" : "", active, ctx.mmu.read32(cam + 0x2B0), ev, f(cam + 0x6C), f(cam + 0x70),
                f(cam + 0x74), f(cam + 0x78), f(cam + 0x7C), f(cam + 0x80), f(cam + 0x84), unsigned(ctx.mmu.read8(g + 2)),
                gflags, int16_t(camid >> 16), int16_t(camid & 0xFFFF), trend, tname, mode, style, handler,
                ctx.mmu.read32(g + 0x98), f(g + 0xB0), f(g + 8), f(g + 0xC), f(g + 0x10), f(g + 0x14), f(g + 0x18),
                f(g + 0x1C), f(g + 0x234), f(g + 0x238), f(g + 0x23C), unsigned(ctx.mmu.read8(cam + 0xB9C + 2)),
                f(cam + 0xB9C + 8), f(cam + 0xB9C + 0xC), f(cam + 0xB9C + 0x10), cid, g3d[0], g3d[1], g3d[2], g3d[3],
                g3d[4], g3d[5], f(link + 0xC0), f(link + 0xC4), f(link + 0xC8), f(link + 0x130), f(link + 0x134),
                f(link + 0x138));
    // Second line: dCameraGame_c mView2 (+0x38, the view the handler writes; fn_80080090 then derives mView),
    // polar +0x2C, doExecute result +0x78, and the active handler object (sCamDefs 0x8050E750 + 16*handler:
    // +0 state, +0x24 frame counter, then its private words as floats/hex).
    if (handler < 13) {
        const uint32_t h = ctx.mmu.read32(0x8050E750 + handler * 16);
        char words[1024] = {};
        int n = 0;
        if (valid(h, 0xC0))
            for (uint32_t o = 0x28; o < 0xC0 && n < 900; o += 4) {
                const uint32_t w = ctx.mmu.read32(h + o);
                const float v = ctx.mmu.read_f32(h + o);
                if ((w >> 24) == 0x80 || (w >> 24) == 0x81 || (w >> 24) == 0x90 || w == 0)
                    n += std::snprintf(words + n, sizeof(words) - n, " %X:%X", o, w);
                else
                    n += std::snprintf(words + n, sizeof(words) - n, " %X:%.4g", o, v);
            }
        std::printf("[SS cam2] frame=%u view2 eye=(%.1f,%.1f,%.1f) at=(%.1f,%.1f,%.1f) fov=%.1f polar=(%.4g,%.4g,%.4g) "
                    "exec=%u h=0x%08X state=%u count=%u |%s\n",
                    frame, f(g + 0x38), f(g + 0x3C), f(g + 0x40), f(g + 0x44), f(g + 0x48), f(g + 0x4C), f(g + 0x50),
                    f(g + 0x2C), f(g + 0x30), f(g + 0x34), unsigned(ctx.mmu.read8(g + 0x78)), h,
                    valid(h, 0x28) ? unsigned(ctx.mmu.read8(h)) : 0u, valid(h, 0x28) ? ctx.mmu.read32(h + 0x24) : 0u,
                    words);
    }
    std::fflush(stdout);
}

[[noreturn]] void fail(CPUContext& ctx, const std::string& reason) {
    std::cerr << "[SS fatal] " << reason << " pc=0x" << std::hex << ctx.pc
              << " lr=0x" << ctx.lr << " r3=0x" << ctx.gpr[3]
              << " r4=0x" << ctx.gpr[4] << " r5=0x" << ctx.gpr[5]
              << std::dec << std::endl;
    nwii::runtime::freeze::report_fatal(ctx, reason); // FREEZE8: location report + message box (interactive)
    // Returning from a game assertion continues with failed invariants. Merely
    // clearing is_running would also be reported as a normal user shutdown.
    ctx.is_running = false;
    std::exit(EXIT_FAILURE);
}

void require_ram(CPUContext& ctx, uint32_t address, uint32_t size) {
    const uint32_t physical = address & 0x3FFFFFFF;
    const uint64_t end = static_cast<uint64_t>(physical) + size;
    const bool in_mem1 = physical < ctx.mmu.mem1.size() - 8 &&
                         end <= ctx.mmu.mem1.size() - 8;
    const bool in_mem2 = physical >= 0x10000000 &&
                         end <= 0x10000000ULL + ctx.mmu.mem2.size() - 8;
    if (address == 0 || size == 0 || (!in_mem1 && !in_mem2)) {
        fail(ctx, "input adapter received an invalid guest RAM range");
    }
}

std::string guest_string(CPUContext& ctx, uint32_t address) {
    std::string result;
    for (uint32_t i = 0; i < 191; ++i) {
        if (address + i < address) return result + "<overflow>";
        const uint8_t* p = ctx.mmu.get_ptr(address + i);
        if (p == nullptr || address == 0) return result + "<invalid address>";
        if (*p == 0) return result;
        result += (*p >= 32 && *p < 127) ? static_cast<char>(*p) : '.';
    }
    return result + "...";
}

bool valid_channel(int32_t channel) {
    return channel >= 0 && channel < 4;
}

void dispatch_motion(CPUContext& ctx) {
    switch (ctx.pc) {
    case 0x803F37B0: ss_KMPLSInit(ctx); break;
    case 0x803F5AA0: ss_KMPLSSetKpadRingBuffer(ctx); break;
    case 0x803F5AD0: ss_KMPLSRead(ctx); break;
    case 0x803F3A10: ss_KMPLSReset(ctx); break;
    case 0x803F6000: ss_KMPLSStopCalibration(ctx); break;
    case 0x803F3630: ss_wpad_callback_func(ctx); break;
    default: nwii::runtime::interpret_step(ctx); break;
    }
}

uint8_t device_type(int32_t channel, const WiimoteState& state) {
    if (state.err != 0) return kNoDevice;
    if (input_state.motion[channel].mode == 5 && state.nunchuk.available) return 6;
    return state.nunchuk.available ? kNunchukDevice : kCoreDevice;
}

void update_motion_configuration(CPUContext& ctx, int32_t channel, const WiimoteState& state) {
    auto& config = input_state.motion[channel];
    const uint32_t base = kKpadBase + channel * kKpadStride;
    const uint8_t desired = ctx.mmu.read8(base + 0x679);
    const uint32_t callback = ctx.mmu.read32(base + 0x680);
    const bool connected = state.err == 0;
    if (!connected || desired == 0) {
        if (config.mode || config.pending) nwii::runtime::ss_motion_reset_channel(ctx, channel);
        config = {};
        nwii::runtime::input::g_guest_motionplus_pointer[channel].store(false);
        ctx.mmu.write8(base + 0x677, 0);
        return;
    }
    if (desired == config.mode && !config.pending) return;
    if (config.pending && config.requested == desired) {
        // A configuration acknowledgement occurs on a later read, after the
        // original game has received the separate "started" callback.
        if (!nwii::runtime::input::motion_sample_fresh(state.motion,
                nwii::runtime::input::motion_now_ns())) return;
        config.pending = false;
        const bool supported = desired == 5 && state.nunchuk.available &&
                               nwii::runtime::ss_motion_initialized();
        config.mode = supported ? desired : 0;
        // Tells the live mouse source that this channel feeds a MotionPlus pointer
        // (virtual_motion.h); it never changes what the game receives by itself.
        nwii::runtime::input::g_guest_motionplus_pointer[channel].store(config.mode == 5);
        ctx.mmu.write8(base + 0x677, config.mode);
        if (callback) ctx.queue_callback(callback, channel, supported ? 1 : uint32_t(-2));
        std::cout << "[SS motion] channel=" << channel << " mode=" << unsigned(config.mode)
                  << " configured=" << supported << std::endl;
    } else {
        config.requested = desired;
        config.pending = true;
        if (callback) ctx.queue_callback(callback, channel, 0);
    }
}

WiimoteState read_state(CPUContext& ctx, int32_t channel) {
    if (!input_state.wpad_initialized) {
        fail(ctx, "controller read before WPADInit");
    }
    const WiimoteState state = InputManager::get().get_wiimote_state(channel);
    const int32_t connection = state.err == 0 ? kOk : kNoController;
    if (connection != input_state.reported_connection[channel]) {
        input_state.reported_connection[channel] = connection;
        const uint32_t callback = input_state.callbacks[channel];
        if (callback != 0) {
            ctx.queue_callback(callback, channel, static_cast<uint32_t>(connection));
        }
    }
    return state;
}

void result_code(CPUContext& ctx, uint32_t pointer, int32_t result) {
    if (pointer != 0) {
        require_ram(ctx, pointer, 4);
        ctx.mmu.write32(pointer, static_cast<uint32_t>(result));
    }
}

struct PcTraceSpec {
    uint32_t module = 0;      // REL module id, unused when absolute
    uint32_t offset = 0;      // section offset, or absolute guest PC
    bool absolute = false;
    int filter_register = -1; // log only when this GPR is nonzero
    int memory_register = -1; // '#rN+off': also log 16 bytes at gpr[N]+off
    uint32_t memory_offset = 0;
    uint32_t resolved = 0;    // current absolute PC, 0 while unresolved
    uint32_t hits = 0;
    std::string label;
};

struct PcTraceState {
    std::vector<PcTraceSpec> specs;
    uint32_t list_head = 0xFFFFFFFF, list_tail = 0xFFFFFFFF;
    uint64_t calls = 0;
    uint32_t max_hits = 400;
};

PcTraceState& pc_trace_state() {
    static PcTraceState state = [] {
        PcTraceState s;
        const char* spec = std::getenv("NWII_TRACEPC");
        if (!spec) return s;
        if (const char* max = std::getenv("NWII_TRACEPC_MAX"))
            s.max_hits = static_cast<uint32_t>(std::strtoul(max, nullptr, 0));
        std::string all(spec);
        size_t start = 0;
        while (start <= all.size()) {
            const size_t end = std::min(all.find(',', start), all.size());
            std::string item = all.substr(start, end - start);
            start = end + 1;
            if (item.empty()) continue;
            PcTraceSpec t;
            t.label = item;
            const size_t hash = item.find('#');
            if (hash != std::string::npos) {
                const std::string mem = item.substr(hash + 1);
                if (mem.size() > 1 && (mem[0] == 'r' || mem[0] == 'R')) {
                    char* rest = nullptr;
                    t.memory_register = static_cast<int>(std::strtol(mem.c_str() + 1, &rest, 10)) & 31;
                    if (rest && *rest == '+') t.memory_offset = static_cast<uint32_t>(std::strtoul(rest + 1, nullptr, 0));
                }
                item = item.substr(0, hash);
            }
            const size_t q = item.find('?');
            if (q != std::string::npos) {
                const std::string reg = item.substr(q + 1);
                if (reg.size() > 1 && (reg[0] == 'r' || reg[0] == 'R'))
                    t.filter_register = std::atoi(reg.c_str() + 1) & 31;
                item = item.substr(0, q);
            }
            const size_t colon = item.find(':');
            if (colon == std::string::npos) {
                t.absolute = true;
                t.offset = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 0));
                t.resolved = t.offset;
            } else {
                t.module = static_cast<uint32_t>(std::strtoul(item.substr(0, colon).c_str(), nullptr, 0));
                t.offset = static_cast<uint32_t>(std::strtoul(item.substr(colon + 1).c_str(), nullptr, 16));
            }
            s.specs.push_back(t);
        }
        std::cout << "[PCTRACE] " << s.specs.size() << " trace points, max " << s.max_hits
                  << " hits each" << std::endl;
        return s;
    }();
    return state;
}

bool guest_ram(uint32_t p, uint32_t size) {
    return (p >= 0x80000000 && uint64_t(p) + size <= 0x81800000) ||
           (p >= 0x90000000 && uint64_t(p) + size <= 0x94000000);
}

// Walks the original OSLink module queue (__OSModuleList at 0x800030C8;
// OSModuleInfo: id +0, next +4, numSections +C, sectionInfo +10 after link).
void pc_trace_resolve(CPUContext& ctx, PcTraceState& s) {
    for (auto& t : s.specs) {
        if (t.absolute) continue;
        uint32_t found = 0;
        uint32_t module = ctx.mmu.read32(0x800030C8);
        for (unsigned n = 0; n < 1024 && guest_ram(module, 0x20); ++n) {
            if (ctx.mmu.read32(module) == t.module) {
                const uint32_t sections = ctx.mmu.read32(module + 0x10);
                const uint32_t count = ctx.mmu.read32(module + 0xC);
                for (uint32_t i = 0; i < count && i < 64 && guest_ram(sections + i * 8, 8); ++i) {
                    const uint32_t off = ctx.mmu.read32(sections + i * 8);
                    if ((off & 1) && guest_ram(off & ~1u, 4)) {
                        found = (off & ~1u) + t.offset;
                        break;
                    }
                }
                break;
            }
            module = ctx.mmu.read32(module + 4);
        }
        if (found != t.resolved) {
            std::cout << "[PCTRACE] " << t.label << " -> 0x" << std::hex << found << std::dec
                      << std::endl;
            t.resolved = found;
        }
    }
}

// Diagnostic hang watchdog, off unless NWII_SS_HANGDUMP is set. Value "1"
// watches nw4r::math::MTX34RotXYZFIdx (0x80437AB0..0x80437C18), otherwise
// "lo-hi" in hex. When the CPU thread's last published PC stays inside the
// range for ~2 s, a host thread prints the register file, GQRs, the stack and
// (for the RotXYZ range) the caller GetAnmRotation's CHR0 node/anim data.
// Read-only: it never writes guest registers or memory.
void hang_dump_words(CPUContext& ctx, const char* label, uint32_t address, uint32_t bytes) {
    if (!guest_ram(address, bytes)) return;
    std::ostringstream line;
    line << "[HANGDUMP] " << label << " @0x" << std::hex << address << ":";
    for (uint32_t i = 0; i < bytes; i += 4) {
        if (i && (i % 32) == 0) line << "\n[HANGDUMP]   +" << i << ":";
        line << " " << std::setw(8) << std::setfill('0') << ctx.mmu.read32(address + i);
    }
    std::cout << line.str() << std::endl;
}

void hang_dump(CPUContext& ctx, uint32_t pc) {
    std::ostringstream line;
    line << std::hex << "[HANGDUMP] pc=0x" << pc << " lr=0x" << ctx.lr << " ctr=0x" << ctx.ctr
         << " msr=0x" << ctx.msr << " xer=0x" << ctx.xer << " fpscr=0x" << ctx.fpscr << "\n";
    for (int r = 0; r < 32; ++r)
        line << "[HANGDUMP] r" << std::dec << r << "=0x" << std::hex << ctx.gpr[r]
             << ((r % 8) == 7 ? "\n" : "");
    for (int r = 0; r < 32; ++r) {
        uint64_t b0 = 0, b1 = 0;
        std::memcpy(&b0, &ctx.fpr[r], 8);
        std::memcpy(&b1, &ctx.ps1[r], 8);
        line << "[HANGDUMP] f" << std::dec << r << " ps0=" << std::setprecision(9) << ctx.fpr[r]
             << " (0x" << std::hex << b0 << ") ps1=" << std::dec << ctx.ps1[r] << " (0x"
             << std::hex << b1 << ")\n";
    }
    line << "[HANGDUMP] gqr:";
    for (int i = 0; i < 8; ++i) line << " " << std::hex << ctx.gqr[i];
    std::cout << line.str() << std::endl;
    // Back chain: each frame's saved LR lives at parent_sp + 4.
    uint32_t sp = ctx.gpr[1];
    for (int depth = 0; depth < 12 && guest_ram(sp, 8); ++depth) {
        const uint32_t parent = ctx.mmu.read32(sp);
        if (!guest_ram(parent, 8)) break;
        std::cout << "[HANGDUMP] frame " << std::dec << depth << std::hex << " sp=0x" << sp
                  << " ret=0x" << ctx.mmu.read32(parent + 4) << std::endl;
        sp = parent;
    }
    hang_dump_words(ctx, "stack", ctx.gpr[1], 0x140);
    // OS globals: current context 0xD4, FPU owner context 0xD8, current
    // thread 0xE4; active thread queue head 0xDC, linked via thread+0x2FC.
    // OSContext: FPR save area +0x90 (f64), srr0 +0x198, srr1 +0x19C, state +0x1A2.
    std::cout << "[HANGDUMP] os curctx=0x" << std::hex << ctx.mmu.read32(0x800000D4)
              << " fpuctx=0x" << ctx.mmu.read32(0x800000D8) << " curthread=0x"
              << ctx.mmu.read32(0x800000E4) << std::endl;
    uint32_t th = ctx.mmu.read32(0x800000DC);
    for (int n = 0; n < 24 && guest_ram(th, 0x300); ++n) {
        std::ostringstream t;
        t << "[HANGDUMP] thread 0x" << std::hex << th << " state=" << ctx.mmu.read16(th + 0x2C8)
          << " prio=" << std::dec << ctx.mmu.read32(th + 0x2D0) << std::hex
          << " srr0=0x" << ctx.mmu.read32(th + 0x198) << " srr1=0x" << ctx.mmu.read32(th + 0x19C)
          << " cstate=" << ctx.mmu.read16(th + 0x1A2) << " r1=0x" << ctx.mmu.read32(th + 4)
          << std::dec << std::setprecision(7);
        for (int f : {0, 1, 2, 3, 4, 11, 29, 30, 31})
            t << " f" << f << "=" << ctx.mmu.read_f64(th + 0x90 + f * 8);
        std::cout << t.str() << std::endl;
        th = ctx.mmu.read32(th + 0x2FC);
    }
    if (pc >= 0x80437AB0 && pc < 0x80437C18) {
        // GetAnmRotation (0x8043FED0) keeps pNodeData in r29, the advanced
        // pAnmData in r30 and the node flags in r31 across this leaf call.
        const uint32_t node = ctx.gpr[29], anm = ctx.gpr[30];
        std::cout << "[HANGDUMP] GetAnmRotation flags=0x" << std::hex << ctx.gpr[31]
                  << " node=0x" << node << " anm=0x" << anm << " frame(caller f31)="
                  << std::dec << ctx.mmu.read_f64(ctx.gpr[1] + 16 + 144) << std::endl;
        hang_dump_words(ctx, "node", node, 0x60);
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t word = guest_ram(anm - 12 + 4 * k, 4) ? ctx.mmu.read32(anm - 12 + 4 * k) : 0;
            if (word < 0x100000) hang_dump_words(ctx, "anmdata", node + word, 0x200);
        }
    }
}

void hang_watchdog_arm(CPUContext& ctx) {
    static const char* spec = std::getenv("NWII_SS_HANGDUMP");
    static std::atomic<bool> started{false};
    if (!spec || started.exchange(true)) return;
    uint32_t lo = 0x80437AB0, hi = 0x80437C18;
    if (std::strcmp(spec, "1") != 0) {
        char* end = nullptr;
        lo = static_cast<uint32_t>(std::strtoul(spec, &end, 16));
        hi = (end && *end == '-') ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 16)) : lo + 4;
    }
    std::cout << "[HANGDUMP] watching 0x" << std::hex << lo << "-0x" << hi << std::dec << std::endl;
    CPUContext* p = &ctx;
    std::thread([p, lo, hi] {
        int inside = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const uint32_t pc = *static_cast<volatile uint32_t*>(&p->pc);
            inside = (pc >= lo && pc < hi) ? inside + 1 : 0;
            if (inside == 8) {
                hang_dump(*p, pc);
                return;
            }
        }
    }).detach();
}

} // namespace

namespace nwii::runtime {

bool ss_pc_trace_enabled() {
    static const bool enabled = std::getenv("NWII_TRACEPC") != nullptr;
    return enabled;
}

void ss_pc_trace(CPUContext& ctx, uint32_t pc) {
    auto& s = pc_trace_state();
    // Re-resolve after OSLink/OSUnlink changes the queue ends, and periodically.
    const uint32_t head = ctx.mmu.read32(0x800030C8), tail = ctx.mmu.read32(0x800030CC);
    if (head != s.list_head || tail != s.list_tail || (++s.calls & 0xFFFFF) == 0) {
        s.list_head = head;
        s.list_tail = tail;
        pc_trace_resolve(ctx, s);
    }
    for (auto& t : s.specs) {
        if (t.resolved != pc || t.hits >= s.max_hits) continue;
        if (t.filter_register >= 0 && ctx.gpr[t.filter_register] == 0) continue;
        ++t.hits;
        std::ostringstream line;
        line << "[PCTRACE] " << t.label << " #" << t.hits << std::hex << " pc=0x" << pc
             << " lr=0x" << ctx.lr << " ctr=0x" << ctx.ctr << " r0=" << ctx.gpr[0];
        for (int r = 3; r <= 12; ++r) line << " r" << std::dec << r << "=" << std::hex << ctx.gpr[r];
        for (int r = 24; r <= 31; ++r) line << " r" << std::dec << r << "=" << std::hex << ctx.gpr[r];
        line << std::dec << " f1=" << ctx.fpr[1] << " f2=" << ctx.fpr[2];
        if (t.memory_register >= 0) {
            const uint32_t at = ctx.gpr[t.memory_register] + t.memory_offset;
            line << " mem@0x" << std::hex << at << ":";
            for (uint32_t i = 0; i < 16 && guest_ram(at, 16); i += 4)
                line << " " << std::setw(8) << std::setfill('0') << ctx.mmu.read32(at + i);
            line << std::dec;
        }
        line << '\n';
        std::cout << line.str();
    }
}

} // namespace nwii::runtime

extern "C" {

void SS_SCGetMplsMoviePlay(CPUContext& ctx) {
    // The introductory movie is a separate DOL launched through firmware.
    // This selects the normal game boot path; it does not claim gyro support.
    ctx.gpr[3] = 1;
}

void SS_WPADInit(CPUContext& ctx) {
    WPADInit(ctx);
    (void)InputManager::get();
    input_state.wpad_initialized = true;
}

void SS_KPADInitEx(CPUContext& ctx) {
    // Native sources own input publication; KMPLS reuses the original KPAD
    // objects and embedded raw rings initialized below.
    input_state = {};
    nwii::runtime::ss_motion_reset();
    // Original KPAD owns four 0x688-byte objects, each with its embedded
    // 16-entry raw ring at +0x180. No heap or guessed scratch RAM is used.
    require_ram(ctx, kKpadBase, 4 * kKpadStride);
    for (uint32_t offset = 0; offset < 4 * kKpadStride; offset += 4)
        ctx.mmu.write32(kKpadBase + offset, 0);
    for (uint32_t channel = 0; channel < 4; ++channel) {
        const uint32_t base = kKpadBase + channel * kKpadStride;
        ctx.mmu.write8(base + 0x67C, 1);
        ctx.mmu.write8(base + 0x649, 1); // original KPAD aiming convention
        for (uint32_t i = 0; i < 16; ++i)
            ctx.mmu.write8(base + 0x180 + i * 0x42 + 0x29, 0xFF);
        input_state.raw[channel][0x28] = kNoDevice;
        input_state.raw[channel][0x29] = 0xFF;
    }
    SS_WPADInit(ctx);
    KPADInit(ctx);
    input_state.kpad_initialized = true;
    std::cout << "[SS input] Native controller/Nunchuk bridge initialized; "
                 "virtual MotionPlus uses original KMPLS processing."
              << std::endl;
}

void SS_WPADRegisterAllocator(CPUContext&) {
    // SOUE01's WPADRegisterAllocator itself is a four-byte return stub. Native
    // InputManager storage is host-owned and never calls a guest allocator.
}

void SS_KPADSetConnectCallback(CPUContext& ctx) {
    const int32_t channel = static_cast<int32_t>(ctx.gpr[3]);
    if (!valid_channel(channel)) fail(ctx, "invalid KPAD callback channel");
    if (!input_state.kpad_initialized) fail(ctx, "callback registration before KPADInitEx");
    const uint32_t callback = ctx.gpr[4];
    if (callback != 0) {
        require_ram(ctx, callback, 4);
        if ((callback & 3) != 0) fail(ctx, "unaligned KPAD callback address");
    }
    input_state.callbacks[channel] = callback;
    // Notify connected channels through the runtime callback queue, never by
    // directly recursing into the guest before its initialization returns.
    input_state.reported_connection[channel] = kNoController;
    (void)read_state(ctx, channel);
}

void SS_WPADProbe(CPUContext& ctx) {
    const int32_t channel = static_cast<int32_t>(ctx.gpr[3]);
    const uint32_t device_pointer = ctx.gpr[4];
    if (!valid_channel(channel)) {
        ctx.gpr[3] = static_cast<uint32_t>(kInvalid);
        return;
    }
    const WiimoteState state = read_state(ctx, channel);
    const bool connected = state.err == 0;
    if (device_pointer != 0) {
        require_ram(ctx, device_pointer, 4);
        ctx.mmu.write32(device_pointer, device_type(channel, state));
    }
    ctx.gpr[3] = static_cast<uint32_t>(connected ? kOk : kNoController);
}

namespace {
// A real KPADRead returns every 200 Hz WPAD sample received since the previous read (newest first,
// up to the caller's buffer), so at 30 game frames/s the game sees ~6-7 statuses per frame.
// SS sums and maxes accelerometer samples per frame over that read length (EGG::CoreController
// getReadLength, dPad::ex_c::acc_c fn_80057AC0 / getMaxAccValue); with one status per frame its
// shake detectors see about 1/7 of the hardware magnitudes. NWII_SS_KPAD_MULTI ("1" = expand, "0" =
// one status per read as before; default kKpadMultiDefault) expands a read into n statuses from the producer history
// (virtual_motion.h resample_accel_bins): older statuses copy the newest one except for their
// accelerometer readings (remote +0x0C..0x18, Nunchuk +0x68..0x74) and carry no button edges, so
// trigger/release stay on the newest status. MotionPlus output stays the KMPLS result of the read.
// CONTROLS: default ON since live runs 20261001-233517 (multi) vs 20261002-000854 (NWII_SS_KPAD_MULTI=0): identical
// progression, walking and remote swing flags; only multi gives the Nunchuk directional shake flag (spin attack).
constexpr bool kKpadMultiDefault = true;
bool kpad_multi_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NWII_SS_KPAD_MULTI");
        const bool v = kKpadMultiDefault ? !(e && e[0] == '0') : (e && e[0] && e[0] != '0');
        std::printf("[SS input] KPAD reads: %s (NWII_SS_KPAD_MULTI=%s)\n",
                    v ? "200 Hz multi-sample" : "one status per read", e ? e : "unset");
        std::fflush(stdout);
        return v;
    }();
    return on;
}
uint32_t expand_kpad_samples(CPUContext& ctx, int32_t channel, uint32_t samples, int32_t count, uint64_t after,
                             uint64_t upto, bool nunchuk) {
    if (count <= 1 || after == 0) return 1;
    std::array<nwii::runtime::input::AccelBin, 16> bins{};
    const size_t max_bins = std::min<size_t>(static_cast<size_t>(count), 15);
    const size_t n = nwii::runtime::input::resample_accel_bins(
        nwii::runtime::input::g_virtual_motion_history[channel], after, upto, max_bins, bins.data());
    if (n <= 1) return 1;
    require_ram(ctx, samples, static_cast<uint32_t>(n * kStatusSize));
    std::array<uint8_t, kStatusSize> newest{};
    for (uint32_t i = 0; i < kStatusSize; ++i) newest[i] = ctx.mmu.read8(samples + i);
    for (size_t k = 1; k < n; ++k) {
        const uint32_t dst = samples + static_cast<uint32_t>(k * kStatusSize);
        const auto& b = bins[n - 1 - k];
        for (uint32_t i = 0; i < kStatusSize; ++i) ctx.mmu.write8(dst + i, newest[i]);
        ctx.mmu.write32(dst + 0x04, 0); // trigger
        ctx.mmu.write32(dst + 0x08, 0); // release
        for (int a = 0; a < 3; ++a) ctx.mmu.write_f32(dst + 0x0C + 4 * a, b.accel[a]);
        ctx.mmu.write_f32(dst + 0x18, std::sqrt(b.accel[0] * b.accel[0] + b.accel[1] * b.accel[1] +
                                                b.accel[2] * b.accel[2]));
        if (nunchuk) {
            for (int a = 0; a < 3; ++a) ctx.mmu.write_f32(dst + 0x68 + 4 * a, b.nunchuk_accel[a]);
            ctx.mmu.write_f32(dst + 0x74, std::sqrt(b.nunchuk_accel[0] * b.nunchuk_accel[0] +
                                                    b.nunchuk_accel[1] * b.nunchuk_accel[1] +
                                                    b.nunchuk_accel[2] * b.nunchuk_accel[2]));
        }
    }
    return static_cast<uint32_t>(n);
}
}

void SS_KPADReadEx(CPUContext& ctx) {
    sample_boot_state(ctx);
    hang_watchdog_arm(ctx); // NWII_SS_HANGDUMP diagnostic, off by default
    static const bool freeze_noted = (nwii::runtime::freeze::note_cpu_thread(ctx),
                                      nwii::runtime::freeze::set_game_info(&freeze_game_info), true); // FREEZE8
    (void)freeze_noted;
    publish_replay_title_phase(ctx); // NWII_SS_INPUT_REPLAY=new-game / new-game-long only
    publish_game_observation(ctx); // read-only; full observation/logs only for new-game or NWII_SS_GAME_TRACE
    publish_camera_trace(ctx); // read-only; NWII_SS_CAM_TRACE only
    {
        // Read-only: dPadManager instance (sInstance at 0x805751E8), mMode at +0x0C; 6 = PointCenter.
        const uint32_t manager = ctx.mmu.read32(0x805751E8);
        const bool point_center = manager >= 0x80000000u && manager < 0x81800000u &&
                                  ctx.mmu.read32(manager + 0x0C) == 6;
        nwii::runtime::input::g_guest_point_center.store(point_center);
        // INPUT13, read-only: the game's menu pointer is shown. dCsGame_c::sInstance (.sbss 0x805757B0):
        // mCursorActive u8 +0xCD0, mCursorType +0x1EC (CS_POINTER_DEF 1, CS_POINTER_CAT 2); both stored by
        // dCsGame_c::execute 0x801BF2A0 (stw r4,0x1EC / stb r0,0xCD0), set each frame via pointerDefSet by the
        // pause menu, map, shop, deposit, ...
        const uint32_t cs = ctx.mmu.read32(0x805757B0);
        const bool cs_valid = (cs >= 0x80000000u && cs < 0x81800000u - 0xCD4) ||
                              (cs >= 0x90000000u && cs < 0x94000000u - 0xCD4);
        const uint32_t cs_type = cs_valid ? ctx.mmu.read32(cs + 0x1EC) : 0;
        const bool cs_active = cs_valid && ctx.mmu.read8(cs + 0xCD0) != 0;
        nwii::runtime::input::g_guest_pointer_cursor.store(cs_active && (cs_type == 1 || cs_type == 2));
        // SOAK14, read-only: the game's own pointer, dPad::ex_c::m_current_ex (.sdata 0x80571C60) pos +0/+4 (computed
        // by d_pad.cpp ex_c::fn_80056790 from the KMPLS estimate), for the closed-loop aim (virtual_motion.h).
        if (ctx.gpr[3] == 0) {
            const uint32_t ex = ctx.mmu.read32(0x80571C60);
            if (ex >= 0x80000000u && ex < 0x81800000u - 8) {
                nwii::runtime::input::g_guest_pointer_x.store(ctx.mmu.read_f32(ex));
                nwii::runtime::input::g_guest_pointer_y.store(ctx.mmu.read_f32(ex + 4));
                nwii::runtime::input::g_guest_pointer_reads.fetch_add(1, std::memory_order_release);
            }
        }
        // NWII_INPUT_TRACE=1 (diagnostic): log the game cursor whenever it changes.
        static const bool cursor_trace = [] { const char* e = std::getenv("NWII_INPUT_TRACE"); return e && e[0] == '1'; }();
        static uint32_t cursor_last = 0xFFFFFFFFu;
        const uint32_t cursor_now = cs_valid ? (cs_active ? cs_type : 0x100u) : 0x200u;
        if (cursor_trace && cursor_now != cursor_last) {
            cursor_last = cursor_now;
            std::printf("[SS input] game cursor: %s type=%u (pointer screen=%d)\n",
                        !cs_valid ? "no dCsGame_c" : cs_active ? "active" : "inactive", unsigned(cs_type),
                        int(nwii::runtime::input::g_guest_pointer_cursor.load()));
            std::fflush(stdout);
        }
    }
    // CTRL18-CAM: once per guest frame (channel 0): latch the free-camera input, forward the game rumble (ss_camera.cpp).
    if (ctx.gpr[3] == 0 && nwii::runtime::input::g_ss_frame_tick) nwii::runtime::input::g_ss_frame_tick(ctx);
    const int32_t channel = static_cast<int32_t>(ctx.gpr[3]);
    const uint32_t samples = ctx.gpr[4];
    const int32_t count = static_cast<int32_t>(ctx.gpr[5]);
    const uint32_t error_pointer = ctx.gpr[6];
    if (!valid_channel(channel) || samples == 0 || count <= 0) {
        result_code(ctx, error_pointer, kInvalid);
        ctx.gpr[3] = 0;
        return;
    }
    if (!input_state.kpad_initialized) fail(ctx, "sample read before KPADInitEx");
    require_ram(ctx, samples, kStatusSize);
    const WiimoteState state = read_state(ctx, channel);
    update_motion_configuration(ctx, channel, state);
    const bool connected = state.err == 0;
    // SOUE01 ABI: include/rvl/KPAD/KPAD.h, KPADStatus (0xF0 bytes).
    // EGG::CoreController::beginFrame reads the first status even when the
    // result is NO_CONTROLLER, so initialize it on both return paths.
    for (uint32_t offset = 0; offset < kStatusSize; offset += 4) {
        ctx.mmu.write32(samples + offset, 0);
    }
    const uint8_t device = device_type(channel, state);
    ctx.mmu.write8(samples + 0x5C, device);
    ctx.mmu.write8(samples + 0x5D, connected ? 0 : 0xFF);
    // KPAD's no-sample/disconnected errors differ from WPADProbe's errors.
    result_code(ctx, error_pointer, connected ? kOk : -2);
    if (!connected) {
        input_state.previous_buttons[channel] = 0;
        input_state.published_sequence[channel] = 0;
        input_state.published_timestamp[channel] = 0;
        input_state.previous_status[channel] = {};
        input_state.raw[channel] = {};
        input_state.raw[channel][0x28] = kNoDevice;
        input_state.raw[channel][0x29] = 0xFF;
        ctx.gpr[3] = 0;
        return;
    }
    const uint32_t previous = input_state.previous_buttons[channel];
    const uint32_t buttons = (state.buttons & kCoreButtons) |
        (state.nunchuk.available ? state.nunchuk.buttons & kNunchukButtons : 0);
    ctx.mmu.write32(samples + 0x00, buttons);
    ctx.mmu.write32(samples + 0x04, buttons & ~previous);
    ctx.mmu.write32(samples + 0x08, previous & ~buttons);
    ctx.mmu.write_f32(samples + 0x0C, state.accel_x);
    ctx.mmu.write_f32(samples + 0x10, state.accel_y);
    ctx.mmu.write_f32(samples + 0x14, state.accel_z);
    ctx.mmu.write_f32(samples + 0x18, std::sqrt(state.accel_x * state.accel_x +
                                              state.accel_y * state.accel_y +
                                              state.accel_z * state.accel_z));
    const bool valid_pointer = std::isfinite(state.ir_x) && std::isfinite(state.ir_y) &&
                               state.ir_x >= 0 && state.ir_x <= 1 &&
                               state.ir_y >= 0 && state.ir_y <= 1;
    if (valid_pointer) {
        ctx.mmu.write_f32(samples + 0x20, state.ir_x * 2.0f - 1.0f);
        ctx.mmu.write_f32(samples + 0x24, 1.0f - state.ir_y * 2.0f);
        ctx.mmu.write_f32(samples + 0x34, 1.0f);
    }
    ctx.mmu.write8(samples + 0x5E, valid_pointer ? 2 : 0);
    // WPAD_FMT_FREESTYLE_BTN_ACC_DPD = 5; core BTN_ACC_DPD = 2.
    ctx.mmu.write8(samples + 0x5F, device == 6 ? 16 : state.nunchuk.available ? 5 : 2);
    if (state.nunchuk.available) {
        const auto& nunchuk = state.nunchuk;
        // KPADStatus::ex_status.fs begins at 0x60.
        ctx.mmu.write_f32(samples + 0x60, nunchuk.stick_x);
        ctx.mmu.write_f32(samples + 0x64, nunchuk.stick_y);
        ctx.mmu.write_f32(samples + 0x68, nunchuk.accel_x);
        ctx.mmu.write_f32(samples + 0x6C, nunchuk.accel_y);
        ctx.mmu.write_f32(samples + 0x70, nunchuk.accel_z);
        ctx.mmu.write_f32(samples + 0x74, std::sqrt(nunchuk.accel_x * nunchuk.accel_x +
            nunchuk.accel_y * nunchuk.accel_y + nunchuk.accel_z * nunchuk.accel_z));
        ctx.mmu.write_f32(samples + 0x78, nunchuk.accel_speed);
    }
    bool sample_available = true;
    bool motion_available = false;
    if (device == 6) {
        // InputManager stamps every controller snapshot, independently of
        // gyro availability. A slow guest can miss gyro integration history
        // while still receiving fresh buttons, sticks and pointer position.
        const uint64_t now = nwii::runtime::input::motion_now_ns();
        const auto& frame = state.motion;
        sample_available = frame.sequence > input_state.published_sequence[channel] &&
            frame.monotonic_ns > input_state.published_timestamp[channel] &&
            frame.monotonic_ns <= now &&
            now - frame.monotonic_ns <= nwii::runtime::input::kMotionMaxAgeNs;
        if (sample_available) {
            motion_available = nwii::runtime::ss_motion_read(ctx, channel, frame, now, samples);
            if (motion_available) {
                const uint32_t raw = nwii::runtime::ss_motion_latest_raw(channel);
                if (raw) {
                    for (uint32_t i = 0; i < 0x42; ++i)
                        input_state.raw[channel][i] = ctx.mmu.read8(raw + i);
                }
            } else {
                // Retain the last original KMPLS output exactly. A fresh core
                // packet does not fabricate a gyro step or advance calibration.
                for (uint32_t i = 0xB0; i < kStatusSize; ++i)
                    ctx.mmu.write8(samples + i, input_state.previous_status[channel][i]);
            }
        }
    } else {
        auto& raw = input_state.raw[channel];
        raw = {};
        raw[0] = uint8_t(buttons >> 8);
        raw[1] = uint8_t(buttons);
        raw[0x28] = device;
        raw[0x40] = state.nunchuk.available ? 5 : 2;
    }
    uint32_t produced = 1;
    if (sample_available && kpad_multi_enabled()) {
        produced = expand_kpad_samples(ctx, channel, samples, count, input_state.published_sequence[channel],
                                       state.motion.sequence, state.nunchuk.available);
    }
    if (!sample_available) {
        // No duplicate/stale controller frame may advance the original calibrator.
        // The original KPAD reader restores the previous complete status here;
        // EGG still uses its acceleration/orientation when the count is zero.
        for (uint32_t i = 0; i < kStatusSize; ++i)
            ctx.mmu.write8(samples + i, input_state.previous_status[channel][i]);
        result_code(ctx, error_pointer, -1);
    } else {
        input_state.previous_buttons[channel] = buttons;
        input_state.published_sequence[channel] = state.motion.sequence;
        input_state.published_timestamp[channel] = state.motion.monotonic_ns;
        for (uint32_t i = 0; i < kStatusSize; ++i)
            input_state.previous_status[channel][i] = ctx.mmu.read8(samples + i);
        if (channel == 0 && ((buttons ^ previous) != 0) && std::getenv("NWII_SS_BOOT_STATE")) {
            std::cout << "[SS input edge] hold=0x" << std::hex << buttons
                      << " trigger=0x" << ctx.mmu.read32(samples + 4)
                      << " release=0x" << ctx.mmu.read32(samples + 8) << std::dec
                      << " gyro=" << (motion_available ? "advanced" : "held")
                      << " sequence=" << state.motion.sequence << std::endl;
        }
    }
    ctx.gpr[3] = sample_available ? produced : 0;
}

void SS_KPADSetMplsWorkarea(CPUContext& ctx) {
    if (!ctx.gpr[3]) {
        nwii::runtime::ss_motion_reset();
        input_state.motion = {};
        // Original KMPLSShutdown (803F3A00) clears this library work pointer.
        ctx.mmu.write32(0x805764D0, 0);
        return;
    }
    nwii::runtime::SSMotionGuestMemory memory;
    memory.workarea = ctx.gpr[3];
    memory.workarea_size = nwii::runtime::kSSMotionWorkSize;
    for (uint32_t channel = 0; channel < 4; ++channel)
        memory.raw_rings[channel] = kKpadBase + channel * kKpadStride + 0x180;
    nwii::runtime::ss_motion_initialize(ctx, memory, dispatch_motion);
}

void SS_WPADiGetMplsStatus(CPUContext& ctx) {
    const int32_t channel = int32_t(ctx.gpr[3]);
    ctx.gpr[3] = valid_channel(channel) ? input_state.motion[channel].mode : 0;
}

void SS_WPADiControlMpls(CPUContext& ctx) {
    const int32_t channel = int32_t(ctx.gpr[3]);
    if (!valid_channel(channel) || !nwii::runtime::ss_motion_control_request(
            ctx, channel, ctx.gpr[4], ctx.gpr[5])) {
        const uint32_t callback = ctx.gpr[5];
        if (callback && valid_channel(channel)) ctx.queue_callback(callback, channel, uint32_t(kInvalid));
        ctx.gpr[3] = uint32_t(kInvalid);
    }
}

void SS_KPADGetUnifiedWpadStatus(CPUContext& ctx) {
    const int32_t channel = int32_t(ctx.gpr[3]);
    const uint32_t destination = ctx.gpr[4];
    const int32_t count = int32_t(ctx.gpr[5]);
    if (!valid_channel(channel) || count <= 0) return;
    if (count > 16) fail(ctx, "raw sample request exceeds native ring capacity");
    require_ram(ctx, destination, uint32_t(count) * 0x42);
    // Return the latest actual raw sensor sample. A core-only publication after
    // a gyro gap leaves this history unchanged; it cannot invent a gyro packet.
    // Additional requested history is marked unavailable.
    for (int32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < 0x42; ++j)
            ctx.mmu.write8(destination + i * 0x42 + j, i == 0 ? input_state.raw[channel][j] : 0);
        if (i) ctx.mmu.write8(destination + i * 0x42 + 0x29, 0xFE);
    }
}

void SS_WPADGetInfoAsync(CPUContext& ctx) {
    const int32_t channel = int32_t(ctx.gpr[3]);
    const uint32_t destination = ctx.gpr[4], callback = ctx.gpr[5];
    const bool connected = valid_channel(channel) && read_state(ctx, channel).err == 0;
    const int32_t result = connected ? kOk : kNoController;
    if (destination) {
        require_ram(ctx, destination, 0x18);
        for (uint32_t i = 0; i < 0x18; i += 4) ctx.mmu.write32(destination + i, 0);
        if (connected) {
            const auto state = InputManager::get().get_wiimote_state(channel);
            ctx.mmu.write32(destination, 1); // pointer reporting enabled
            ctx.mmu.write32(destination + 8, state.nunchuk.available ? 1 : 0);
            ctx.mmu.write8(destination + 0x14, 4); // virtual controller: full battery
            ctx.mmu.write8(destination + 0x15, uint8_t(1 << channel));
        }
    }
    if (callback && valid_channel(channel)) ctx.queue_callback(callback, channel, uint32_t(result));
    ctx.gpr[3] = uint32_t(result);
}

void SS_WPADCanSendStreamData(CPUContext& ctx) {
    const int32_t channel = int32_t(ctx.gpr[3]);
    ctx.gpr[3] = valid_channel(channel) && read_state(ctx, channel).err == 0 ? 1 : 0;
}

void SS_WPADGetRadioSensitivity(CPUContext& ctx) {
    const int32_t channel = int32_t(ctx.gpr[3]);
    ctx.gpr[3] = valid_channel(channel) && read_state(ctx, channel).err == 0 ? 100 : 0;
}

[[noreturn]] void SS_DvdLoadFailure(CPUContext& ctx) {
    // SOUE01 createOrDie deliberately loops forever after create returns null.
    require_ram(ctx, ctx.gpr[1], 8);
    const uint32_t caller = ctx.mmu.read32(ctx.gpr[1]);
    require_ram(ctx, caller, 0x40);
    std::cerr << "[SS load failure] caller path=" << guest_string(ctx, caller + 8)
              << " module_control=0x" << std::hex << ctx.gpr[30]
              << " archive=0x" << ctx.mmu.read32(ctx.gpr[13] - 14500)
              << std::dec << '\n';
    fail(ctx, "DVD createOrDie reached the original game's fatal loop");
}

[[noreturn]] void SS_ExceptionWaitFailure(CPUContext& ctx) {
    // This is the original EGG exception console's wait loop, not a normal
    // game delay. Report its state as failure instead of a successful timeout.
    constexpr uint32_t thp = 0x805CD440; // ELF ActivePlayer
    std::cerr << "[SS THP] open=" << ctx.mmu.read32(thp + 0xA0)
              << " state=" << unsigned(ctx.mmu.read8(thp + 0xA4))
              << " audio=" << unsigned(ctx.mmu.read8(thp + 0xA7))
              << " dvdError=" << int32_t(ctx.mmu.read32(thp + 0xA8))
              << " videoError=" << int32_t(ctx.mmu.read32(thp + 0xAC))
              << " onMemory=" << ctx.mmu.read32(thp + 0xB0)
              << " dimensions=" << ctx.mmu.read32(thp + 0x80) << 'x' << ctx.mmu.read32(thp + 0x84)
              << " tracks=" << ctx.mmu.read32(thp + 0x98)
              << " data=0x" << std::hex << ctx.mmu.read32(thp + 0xB4)
              << " work=0x" << ctx.mmu.read32(thp + 0x9C)
              << " fileStart=0x" << ctx.mmu.read32(thp + 0x30) << std::dec << std::endl;
    fail(ctx, "original game entered the EGG exception console");
}

[[noreturn]] void SS_sAssertTrace(CPUContext& ctx) {
    // At this verified SOUE01 call site r4 is EGG::HeapAllocArg, not text.
    if (ctx.lr == 0x80054DB4) {
        const uint32_t arg = ctx.gpr[4];
        require_ram(ctx, arg, 0x14);
        std::cerr << "[SS allocation] size=0x" << std::hex << ctx.mmu.read32(arg + 4)
                  << " alignment=0x" << ctx.mmu.read32(arg + 8)
                  << " result=0x" << ctx.mmu.read32(arg + 12)
                  << " heap=0x" << ctx.mmu.read32(arg + 16) << std::dec << '\n';
    }
    uint32_t frame = ctx.gpr[1];
    for (unsigned i = 0; i < 12; ++i) {
        if (frame < 0x80000000 || frame >= 0x81800000 - 8 || (frame & 3)) break;
        std::cerr << "[SS stack] frame=0x" << std::hex << frame
                  << " saved_lr=0x" << ctx.mmu.read32(frame + 4) << std::dec << '\n';
        uint32_t next = ctx.mmu.read32(frame);
        if (next <= frame) break;
        frame = next;
    }
    fail(ctx, "sAssert::assert");
}

[[noreturn]] void SS_EggAssertTrace(CPUContext& ctx) {
    const std::string file = guest_string(ctx, ctx.gpr[3]);
    const std::string format = guest_string(ctx, ctx.gpr[5]);
    fail(ctx, "EGG::Assert file=\"" + file + "\" line=" +
              std::to_string(ctx.gpr[4]) + " format=\"" + format + "\"");
}

} // extern "C"
