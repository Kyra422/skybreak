// CTRL18-CAM (T1, design work/claude-notes/CTRL18-design.md 2.6/7): the game's rumble on the controller.
//
// SOUE01 rumbles through EGG: dRumble_c::execute / start (0x800665B0 / 0x80066B00) -> EGG::CoreController
// startPatternRumble -> ControllerRumbleMgr::calc (0x8049A690, in CoreController::beginFrame, once per frame while a
// rumble unit is active: on/off per frame, PWM-like patterns) -> CoreController::startMotor / stopMotor (tail calls)
// -> WPADControlMotor(chan, cmd) 0x803DB1E0. Before CTRL18 nothing reached the pad: our WPADInit (SS_WPADInit) never
// creates the SDK control blocks and never sets the SDK "motor enabled" word (r13-0x3030 = 0x80576410, set by
// __wpadInitSub from SCGetWpadMotorMode), so the original WPADControlMotor returned without effect.
//
// [hle_hooks] "803DB1E0" = "SS_WPADControlMotor":
//  1. records the requested motor state per channel (cmd != 0), for the per-frame forwarding below;
//  2. then does EXACTLY what the original body does to the guest (a transliteration of the generated
//     ss_WPADControlMotor incl. the inlined OSDisableInterrupts / OSRestoreInterrupts): same stack writes, same
//     reads/writes of __rvl_p_wpadcb[chan] +0x900/+0x914/+0x918 and of the motor-enabled word, same registers at the
//     return (r0 = LR, r3 = 0, r4/r5 = MSR values, CR0 of OSRestoreInterrupts' cmpwi, r30/r31 restored), same MSR
//     (incl. the FP-unavailable guard after each mtmsr) and the same instruction count per path. One difference:
//     an interrupt that becomes deliverable when OSRestoreInterrupts re-enables EE is taken at the next poll point
//     instead of inside this call (an HLE body must not switch threads; the hooked body has no resume points).
//     outputs/pc-tests/ctrl18cam compares this against the extracted original bodies on many states.
// Forwarding (frame tick, once per guest frame; ss_camera.cpp): a channel whose game motor is on drives the pad in
// the same slot (Wii Remote k = controller slot k) through the INPUT13 pad_hub request path (the controller thread
// calls SDL_GameControllerRumble; DualSense/DS4 via SDL's HIDAPI rumble, enabled in main.cpp), strength
// [input] rumble 0..100 (g_rumble_strength, default 100; 0 = no request at all), never while the remote is driven by
// keyboard/mouse (g_active_device). A request lasts 150 ms and is renewed every 3 frames while on, so a stalled game
// never leaves the pad buzzing.
// Env: NWII_RUMBLE=0..100 (overrides the setting), NWII_RUMBLE_LOG=1 ("[Input] rumble" lines: game motor changes,
// pad requests; a count line every 300 frames comes with NWII_CAM_LOG).
#include "ss_camera.h"
#include "runtime/camera_stick.h"
#include "../input/pad_hub.h"
#include "runtime/cpu_context.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

using nwii::runtime::CPUContext;
namespace in = nwii::runtime::input;

namespace nwii::runtime::ss_rumble {
namespace {

constexpr uint32_t kWpadCbTable = 0x805F66D0; // __rvl_p_wpadcb[4] (lis 0x805F + 26320)
constexpr int32_t kMotorEnabledSda = -12336;  // r13 - 0x3030
constexpr uint32_t kMsrEE = 0x8000;
constexpr uint16_t kRequestMs = 150;
constexpr uint32_t kRenewFrames = 3;

struct State {
    bool env_done = false, log = false;
    std::array<bool, 4> motor{};   // game's requested motor state (last WPADControlMotor cmd per channel)
    std::array<bool, 4> logged{};  // last logged game state
    std::array<bool, 4> sent_on{}; // what the pad was last told
    std::array<uint32_t, 4> since_post{};
    uint64_t frame = 0;
    Stats st;
};
State& S() {
    static State s;
    return s;
}

void apply_env() {
    State& s = S();
    if (s.env_done) return;
    s.env_done = true;
    if (const char* e = std::getenv("NWII_RUMBLE"); e && e[0])
        in::g_rumble_strength.store(std::clamp(int(std::strtol(e, nullptr, 10)), 0, 100));
    const char* l = std::getenv("NWII_RUMBLE_LOG");
    s.log = l && l[0] && l[0] != '0';
}

// Same encoding as gamepad_source.cpp request_rumble (serial << 48 | ms << 32 | low << 16 | high); the controller
// thread (or the inline capture) applies a new serial with SDL_GameControllerRumble.
void post(int slot, uint16_t low, uint16_t high, uint16_t ms) {
    auto& cell = in::pad_hub().rumble[size_t(slot)];
    uint64_t old = cell.load();
    for (;;) {
        uint64_t serial = ((old >> 48) + 1) & 0xFFFF;
        if (serial == 0) serial = 1;
        const uint64_t v = serial << 48 | uint64_t(ms) << 32 | uint64_t(low) << 16 | high;
        if (cell.compare_exchange_weak(old, v)) break;
    }
    ++S().st.pad_requests;
}

} // namespace

Stats stats() { return S().st; }
void reset_for_test() { S() = State{}; }

void frame_tick() {
    apply_env();
    State& s = S();
    ++s.frame;
    const int strength = std::clamp(in::g_rumble_strength.load(std::memory_order_relaxed), 0, 100);
    bool any_on = false;
    for (int c = 0; c < 4; ++c) {
        const bool on = s.motor[size_t(c)];
        any_on = any_on || on;
        const bool keyboard = in::g_active_device[size_t(c)].load(std::memory_order_relaxed) ==
                              int(in::ActiveDevice::KeyboardMouse);
        const bool want = on && strength > 0 && !keyboard;
        if (s.log && on != s.logged[size_t(c)]) {
            s.logged[size_t(c)] = on;
            std::printf("[Input] rumble: game motor %s on Wii Remote %d (frame %llu; strength %d%s)\n", on ? "ON" : "off",
                        c + 1, (unsigned long long)s.frame, strength, keyboard ? ", keyboard: not forwarded" : "");
            std::fflush(stdout);
        }
        auto& since = s.since_post[size_t(c)];
        ++since;
        if (want && (!s.sent_on[size_t(c)] || since >= kRenewFrames)) {
            const uint32_t high = uint32_t(0xFFFF) * uint32_t(strength) / 100u;
            const uint32_t low = uint32_t(0x9000) * uint32_t(strength) / 100u;
            post(c, uint16_t(low), uint16_t(high), kRequestMs);
            if (s.log && !s.sent_on[size_t(c)])
                std::printf("[Input] rumble: controller slot %d on (low 0x%04X high 0x%04X, %u ms renewed)\n", c, low,
                            high, unsigned(kRequestMs));
            s.sent_on[size_t(c)] = true;
            since = 0;
        } else if (!want && s.sent_on[size_t(c)]) {
            post(c, 0, 0, 0);
            if (s.log) std::printf("[Input] rumble: controller slot %d off\n", c);
            s.sent_on[size_t(c)] = false;
            since = 0;
        }
    }
    if (any_on) ++s.st.on_frames;
}

} // namespace nwii::runtime::ss_rumble

extern "C" void SS_WPADControlMotor(CPUContext& ctx) {
    using namespace nwii::runtime::ss_rumble;
    const uint32_t chan = ctx.gpr[3], cmd = ctx.gpr[4];
    {
        State& s = S();
        ++s.st.motor_calls;
        if (cmd != 0) ++s.st.motor_on_calls;
        if (chan < 4) s.motor[chan] = cmd != 0;
    }
    // ---- The original body (0x803DB1E0..0x803DB2A8), transliterated from the generated ss_WPADControlMotor ----
    auto& m = ctx.mmu;
    // Instruction count exactly as the generated body counts it: "++ctx.inst_count; loc_X:" precedes each
    // instruction, so an instruction reached by falling through counts 1 and a branch target reached by a taken
    // branch does not.
    uint64_t n = 0;
    const auto os_restore = [&] { // OSRestoreInterrupts 0x803A8D40 (r3 = level from OSDisableInterrupts)
        const int32_t level = int32_t(ctx.gpr[3]);
        ctx.cr[0].lt = level < 0;
        ctx.cr[0].gt = level > 0;
        ctx.cr[0].eq = level == 0;
        ctx.cr[0].so = (ctx.xer >> 31) & 1;
        ctx.gpr[4] = ctx.msr;
        if (level == 0) {
            ctx.gpr[5] = ctx.gpr[4] & ~kMsrEE; // cmpwi, mfmsr, beq -> (rlwinm: branch target) mtmsr, rlwinm, blr
            n += 6;
        } else {
            ctx.gpr[5] = ctx.gpr[4] | kMsrEE; // cmpwi, mfmsr, beq, ori, b -> (mtmsr: branch target) rlwinm, blr
            n += 7;
        }
        ctx.msr = ctx.gpr[5]; // mtmsr + the generated FP-unavailable guard (the interrupt poll: see the header)
        ctx.fp_unavailable_check();
        ctx.gpr[3] = (ctx.gpr[4] >> 15) & 1;
    };
    // prologue 0x803DB1E0..0x803DB208 (11): stwu r1,-16(r1); mflr r0; lis r5,0x805F; stw r0,20(r1);
    // rlwinm r0,r3,2,0,29; addi r5,r5,26320; stw r31,12(r1); stw r30,8(r1); mr r30,r4; lwzx r31,r5,r0; bl
    const uint32_t sp = ctx.gpr[1] - 16;
    m.write32(sp, ctx.gpr[1]);
    ctx.gpr[1] = sp;
    ctx.gpr[0] = ctx.lr;
    m.write32(sp + 20, ctx.gpr[0]);
    ctx.gpr[0] = (ctx.gpr[3] << 2) & 0xFFFFFFFCu;
    ctx.gpr[5] = kWpadCbTable;
    m.write32(sp + 12, ctx.gpr[31]);
    m.write32(sp + 8, ctx.gpr[30]);
    ctx.gpr[30] = cmd;
    const uint32_t wpd = m.read32(ctx.gpr[5] + ctx.gpr[0]);
    ctx.gpr[31] = wpd;
    // OSDisableInterrupts 0x803A8D00 (5): mfmsr, rlwinm, mtmsr (EE 1 -> 0: no poll), rlwinm, blr
    ctx.gpr[3] = ctx.msr;
    ctx.gpr[4] = ctx.gpr[3] & ~kMsrEE;
    ctx.msr = ctx.gpr[4];
    ctx.fp_unavailable_check();
    ctx.gpr[3] = (ctx.gpr[3] >> 15) & 1;
    n += 11 + 5;
    n += 3; // 20C lwz 2304(r31), 210 cmpwi -1, 214 bne
    if (int32_t(m.read32(wpd + 0x900)) == -1) {
        n += 1; // 218 bl
        os_restore();
        n += 1 + 5; // 21C b -> (294) 298..2A8
    } else {
        n += 2; // (220 lwz) 224 cmpwi, 228 bne
        const uint32_t enabled = m.read32(ctx.gpr[13] + uint32_t(kMotorEnabledSda));
        bool at248 = enabled != 0, store = false;
        if (!at248) {
            n += 2; // 22C cmpwi r30,0; 230 bne
            if (cmd != 0) {
                os_restore(); // (240 bl)
                n += 1 + 5; // 244 b -> epilogue
            } else {
                n += 3; // 234 lwz 2328(r31), 238 cmpwi 1, 23C beq
                if (m.read32(wpd + 0x918) == 1) {
                    at248 = true;
                } else {
                    n += 1; // 240 bl
                    os_restore();
                    n += 1 + 5; // 244 b -> epilogue
                }
            }
        }
        if (at248) {
            n += 1; // (248 cmpwi r30,0) 24C bne
            if (cmd == 0) {
                n += 3; // 250 lwz, 254 cmpwi 0, 258 beq
                if (m.read32(wpd + 0x918) == 0) {
                    os_restore(); // (270 bl)
                    n += 1 + 5; // 274 b -> epilogue
                } else {
                    n += 2; // 25C cmplwi r30,1; 260 bne -> 278
                    store = true;
                }
            } else {
                n += 1; // (25C cmplwi) 260 bne
                if (cmd != 1) {
                    store = true;
                } else {
                    n += 3; // 264 lwz, 268 cmpwi 1, 26C bne
                    if (m.read32(wpd + 0x918) != 1) {
                        store = true;
                    } else {
                        n += 1; // 270 bl
                        os_restore();
                        n += 1 + 5; // 274 b -> epilogue
                    }
                }
            }
        }
        if (store) {
            // (278 neg r4,r30) 27C li r0,1; 280 or r4,r4,r30; 284 rlwinm r4,r4,1,31,31; 288 stw r4,2328(r31);
            // 28C stw r0,2324(r31); 290 bl; then the epilogue is reached by falling through (294..2A8 = 6)
            ctx.gpr[4] = cmd != 0 ? 1u : 0u;
            ctx.gpr[0] = 1;
            m.write32(wpd + 0x918, ctx.gpr[4]);
            m.write32(wpd + 0x914, ctx.gpr[0]);
            n += 6;
            os_restore();
            n += 6;
        }
    }
    // epilogue: lwz r0,20(r1); lwz r31,12(r1); lwz r30,8(r1); mtlr r0; addi r1,r1,16; blr
    ctx.gpr[0] = m.read32(sp + 20);
    ctx.gpr[31] = m.read32(sp + 12);
    ctx.gpr[30] = m.read32(sp + 8);
    ctx.lr = ctx.gpr[0];
    ctx.gpr[1] = sp + 16;
    ctx.inst_count += n;
}
