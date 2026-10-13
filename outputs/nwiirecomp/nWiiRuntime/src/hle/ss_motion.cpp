#include "runtime/ss_motion.h"
#include "../input/game_context.h"
#include "../input/gyro_calibration.h"
#include "runtime/cpu_context.h"
#include "input/input_source.h"
#include "../input/virtual_motion.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <stdexcept>

namespace nwii::runtime {
namespace {
constexpr uint32_t kReturn = 0xFFFFFFF8;
constexpr uint32_t kInit = 0x803F37B0, kSetRing = 0x803F5AA0;
constexpr uint32_t kRead = 0x803F5AD0, kReset = 0x803F3A10, kStop = 0x803F6000;
constexpr double kPi = 3.14159265358979323846;
// Explicit virtual ADC contract. These are not measured hardware calibration.
constexpr float kAdcZero = 8192.f, kAdcScale = 4096.f;
constexpr int kSlowDegrees = 270, kFastDegrees = 1200;
// The producer cap (input_source.h) must encode without clamping in the 14-bit fast-mode range.
static_assert(kAdcZero + input::kVirtualMotionMaxRateRadS * 180.0 / kPi * kAdcScale / kFastDegrees <= 16383.0 &&
                  kAdcZero - input::kVirtualMotionMaxRateRadS * 180.0 / kPi * kAdcScale / kFastDegrees >= 0.0,
              "kVirtualMotionMaxRateRadS exceeds the raw MotionPlus fast-mode range");
struct Channel {
    uint64_t sequence = 0, timestamp = 0;
    uint32_t latest_raw = 0, pending_callback = 0;
    unsigned slot = 0;
    uint64_t history_samples = 0; // raw samples fed from the producer history
    bool available = false;
    bool history_overflow_logged = false;
};
struct State {
    CPUContext* owner = nullptr;
    SSMotionDispatch dispatch = nullptr;
    SSMotionGuestMemory memory{};
    std::array<Channel, 4> channels{};
    bool initialized = false;
};
thread_local State state;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool valid_channel(int32_t channel) { return channel >= 0 && channel < 4; }
void require_owner(CPUContext& ctx) {
    require(state.initialized && state.owner == &ctx, "SS motion: uninitialized or wrong guest context");
}
void require_ram(CPUContext& ctx, uint32_t address, uint32_t size) {
    const uint64_t p = address & 0x3FFFFFFF;
    const uint64_t end = p + size;
    require(address && size && uint64_t(address) + size <= 0x100000000ULL &&
            ((p < ctx.mmu.mem1.size() - 8 && end <= ctx.mmu.mem1.size() - 8) ||
             (p >= 0x10000000 && end <= 0x10000000ULL + ctx.mmu.mem2.size() - 8)),
            "SS motion: guest buffer outside RAM");
}
bool overlap(uint32_t a, uint32_t as, uint32_t b, uint32_t bs) {
    const uint64_t ap = a & 0x3FFFFFFF, bp = b & 0x3FFFFFFF;
    return ap < bp + bs && bp < ap + as;
}

// Only registers are restored: original KMPLS work/output memory persists.
struct Registers {
    CPUContext& ctx;
    CPUContext::BackupState saved;
    bool callback;
    explicit Registers(CPUContext& c) : ctx(c), saved{c.gpr,c.fpr,c.ps1,c.cr,c.lr,c.ctr,
        c.xer,c.pc,c.srr0,c.srr1,c.msr,c.fpscr,c.gqr,c.sprg}, callback(c.in_callback) {}
    ~Registers() {
        ctx.gpr=saved.gpr; ctx.fpr=saved.fpr; ctx.ps1=saved.ps1; ctx.cr=saved.cr;
        ctx.lr=saved.lr; ctx.ctr=saved.ctr; ctx.xer=saved.xer; ctx.pc=saved.pc;
        ctx.srr0=saved.srr0; ctx.srr1=saved.srr1; ctx.msr=saved.msr; ctx.fpscr=saved.fpscr;
        ctx.gqr=saved.gqr; ctx.sprg=saved.sprg; ctx.in_callback=callback;
    }
};
// STAB20: the host stopping the CPU is not an error. At shutdown main.cpp clears is_running (and pc) while the CPU
// thread may be inside a KMPLS dispatch below (KPADReadEx -> ss_motion_read -> invoke). This used to be a require()
// that threw std::runtime_error; nothing on the CPU thread catches it, so std::thread's noexcept entry called
// std::terminate -> abort -> __fastfail: exit 0xC0000409 at shutdown, without a log line (work/claude-notes/STAB20.md,
// reproduced with work/stab20/dbgrun.py --kmpls-stop). GuestStopped unwinds to the public entry point (Registers
// restores the caller's registers on the way), which returns as "no motion / nothing done"; the dispatcher then sees
// is_running == false and ends the CPU thread as usual. Real contract violations still throw.
struct GuestStopped {};
void invoke(CPUContext& ctx, uint32_t pc, uint32_t r3=0, uint32_t r4=0,
            uint32_t r5=0, uint32_t r6=0, uint32_t r7=0, double f1=0) {
    Registers registers(ctx);
    const auto backup_depth=ctx.backup_stack.size();
    const auto callback_depth=ctx.callback_depth;
    const uint64_t start=ctx.inst_count;
    ctx.in_callback=true;
    ctx.gpr[3]=r3; ctx.gpr[4]=r4; ctx.gpr[5]=r5; ctx.gpr[6]=r6; ctx.gpr[7]=r7;
    ctx.fpr[1]=f1; ctx.lr=kReturn; ctx.pc=pc;
    unsigned resumes=0;
    while (ctx.pc != kReturn) {
        if (!ctx.is_running.load()) throw GuestStopped{}; // STAB20 (was: require -> std::runtime_error)
        require(++resumes <= 10000 && ctx.inst_count-start < 10000000,
                "SS motion: KMPLS dispatch budget exceeded");
        state.dispatch(ctx);
    }
    require(ctx.gpr[1] == registers.saved.gpr[1] && ctx.backup_stack.size() == backup_depth &&
            ctx.callback_depth == callback_depth, "SS motion: guest call did not restore its stack");
}
void clear_ring(CPUContext& ctx, int32_t channel) {
    const auto ring=state.memory.raw_rings[channel];
    std::memset(ctx.mmu.get_ptr(ring),0,kSSMotionRawRingSize);
    for (unsigned i=0;i<16;++i) {
        ctx.mmu.write8(ring+i*0x42+0x28,253);
        ctx.mmu.write8(ring+i*0x42+0x29,255);
    }
}
int16_t accel(float value) {
    require(std::isfinite(value), "SS motion: nonfinite KPAD acceleration");
    return static_cast<int16_t>(std::lround(std::clamp(value,-128.f,127.f)*256.f));
}
int8_t stick(float value) {
    require(std::isfinite(value), "SS motion: nonfinite Nunchuk stick");
    return static_cast<int8_t>(std::lround(std::clamp(value,-1.f,1.f)*127.f));
}
void write_raw(CPUContext& ctx, uint32_t raw, uint32_t status, const input::MotionSample& motion) {
    std::memset(ctx.mmu.get_ptr(raw),0,0x42);
    const uint8_t device=ctx.mmu.read8(status+0x5C);
    const bool nunchuk=device==6;
    ctx.mmu.write16(raw,static_cast<uint16_t>(ctx.mmu.read32(status)));
    for (unsigned a=0;a<3;++a)
        ctx.mmu.write16(raw+2+a*2,static_cast<uint16_t>(accel(ctx.mmu.read_f32(status+0xC+a*4))));
    // A virtual two-point sensor bar represents the same processed pointer and
    // horizon. This is a defined virtual model, not physical IR observations.
    const float x=ctx.mmu.read_f32(status+0x20), y=ctx.mmu.read_f32(status+0x24);
    const float hx=ctx.mmu.read_f32(status+0x34), hy=ctx.mmu.read_f32(status+0x38);
    const bool ir=static_cast<int8_t>(ctx.mmu.read8(status+0x5E))>0 &&
        std::isfinite(x) && std::isfinite(y) && std::isfinite(hx) && std::isfinite(hy);
    for (unsigned i=0;i<4;++i) {
        const auto p=raw+8+i*8;
        ctx.mmu.write16(p,0xFFFF); ctx.mmu.write16(p+2,0xFFFF); ctx.mmu.write8(p+6,0xFF);
        if (ir && i<2) {
            const float sign=i?1.f:-1.f;
            ctx.mmu.write16(p,static_cast<uint16_t>(std::lround(std::clamp((x+1)*512+sign*40*hx,0.f,1023.f))));
            ctx.mmu.write16(p+2,static_cast<uint16_t>(std::lround(std::clamp((1-y)*384-sign*40*hy,0.f,767.f))));
            ctx.mmu.write16(p+4,1); ctx.mmu.write8(p+6,static_cast<uint8_t>(i));
        }
    }
    ctx.mmu.write8(raw+0x28,device); ctx.mmu.write8(raw+0x29,ctx.mmu.read8(status+0x5D));
    if (nunchuk) {
        for (unsigned a=0;a<3;++a)
            ctx.mmu.write16(raw+0x2A+a*2,static_cast<uint16_t>(accel(ctx.mmu.read_f32(status+0x68+a*4))));
        ctx.mmu.write8(raw+0x30,static_cast<uint8_t>(stick(ctx.mmu.read_f32(status+0x60))));
        ctx.mmu.write8(raw+0x31,static_cast<uint8_t>(stick(ctx.mmu.read_f32(status+0x64))));
    }
    uint8_t flags=nunchuk?0xC1:0x80;
    const float values[]={motion.gyro_x_rad_s,motion.gyro_y_rad_s,motion.gyro_z_rad_s};
    constexpr uint8_t slow_bits[]={4,8,2};
    for (unsigned a=0;a<3;++a) {
        require(std::abs(values[a])<=input::kVirtualMotionMaxRateRadS,
                "SS motion: virtual angular rate outside producer contract");
        const double degrees=values[a]*180.0/kPi;
        const bool slow=std::abs(degrees)<=kSlowDegrees;
        if (slow) flags|=slow_bits[a];
        const double counts=kAdcZero+degrees*kAdcScale/(slow?kSlowDegrees:kFastDegrees);
        ctx.mmu.write16(raw+0x38+a*2,static_cast<uint16_t>(std::lround(std::clamp(counts,0.,16383.))));
    }
    ctx.mmu.write8(raw+0x36,flags);
    ctx.mmu.write8(raw+0x40,16);
}
void deliver_pending_callback(CPUContext& ctx, int32_t channel, Channel& c) {
    if (!c.pending_callback) return;
    const uint32_t callback=c.pending_callback;
    c.pending_callback=0;
    invoke(ctx,callback,channel,0);
}
void require_finite_output(CPUContext& ctx, uint32_t status) {
    for (unsigned i=0;i<15;++i)
        require(std::isfinite(ctx.mmu.read_f32(status+0xB0+i*4)), "SS motion: nonfinite original KMPLS output");
}
// Producer history path (src/input/virtual_motion.h). The InputManager records
// every fresh producer step; a guest that reads KPAD less often (a slow scene)
// still receives the whole interval since its previous read, like the 200 Hz
// WPAD sample ring of real hardware, instead of only the newest snapshot.
// The steps are resampled into at most 15 equal raw samples (KMPLSRead's
// read_mpls walks cursor-count-1 .. cursor-2 and wraps the 16-entry ring once)
// whose average rates preserve each axis' integrated angle exactly. Returns
// false when the history does not cover this sample (no producer history, e.g.
// unit fixtures), so the caller keeps its single-sample behaviour.
bool read_history(CPUContext& ctx, int32_t channel, Channel& c,
                  const input::MotionSample& sample, uint32_t status) {
    constexpr size_t kMaxSteps=input::MotionStepHistory::kCapacity;
    constexpr unsigned kMaxSamples=15;
    constexpr double kSamplePeriod=0.005; // 200 Hz, the hardware WPAD report rate
    static thread_local std::vector<input::MotionStep> steps(kMaxSteps);
    bool latest=false, lost=false;
    const size_t count=input::g_virtual_motion_history[channel].since(
        c.sequence,sample.sequence,steps.data(),kMaxSteps,&latest,&lost);
    if (!latest || count==0) return false;
    double total=0;
    for (size_t i=0;i<count;++i) {
        const auto& s=steps[i];
        require(std::isfinite(s.dt_seconds) && s.dt_seconds>0 && s.dt_seconds<=0.1f &&
                std::isfinite(s.rate[0]) && std::isfinite(s.rate[1]) && std::isfinite(s.rate[2]),
                "SS motion: invalid producer history step");
        total+=s.dt_seconds;
    }
    if (lost && !c.history_overflow_logged) {
        c.history_overflow_logged=true;
        std::printf("[SS motion] channel=%d producer history overflow: integrating the %zu retained steps\n",
                    int(channel),count);
        std::fflush(stdout);
    }
    const unsigned samples=static_cast<unsigned>(std::clamp<long long>(
        std::llround(total/kSamplePeriod),1,kMaxSamples));
    const double width=total/samples;
    size_t step=0;
    double step_left=steps[0].dt_seconds;
    input::MotionSample bin=sample;
    uint32_t raw=0;
    for (unsigned k=0;k<samples;++k) {
        double angle[3]={0,0,0}, need=width;
        while (need>0 && step<count) {
            const double take=std::min(need,step_left);
            for (unsigned a=0;a<3;++a) angle[a]+=steps[step].rate[a]*take;
            need-=take; step_left-=take;
            if (step_left<=1e-12 && ++step<count) step_left=steps[step].dt_seconds;
        }
        const auto rate=[&](unsigned a) {
            return static_cast<float>(std::clamp(angle[a]/width,-double(input::kVirtualMotionMaxRateRadS),
                                                 double(input::kVirtualMotionMaxRateRadS)));
        };
        bin.gyro_x_rad_s=rate(0); bin.gyro_y_rad_s=rate(1); bin.gyro_z_rad_s=rate(2);
        raw=state.memory.raw_rings[channel]+((c.slot+k)%16)*0x42;
        write_raw(ctx,raw,status,bin);
    }
    c.sequence=sample.sequence; c.timestamp=sample.monotonic_ns;
    c.latest_raw=raw;
    deliver_pending_callback(ctx,channel,c);
    invoke(ctx,kRead,channel,status+0xB0,(c.slot+samples+1)%16,samples,status,width);
    c.slot=(c.slot+samples)%16;
    c.history_samples+=samples;
    require_finite_output(ctx,status);
    return true;
}
}

void ss_motion_reset() { state=State{}; }
bool ss_motion_initialized() { return state.initialized; }
void ss_motion_initialize(CPUContext& ctx, const SSMotionGuestMemory& memory, SSMotionDispatch dispatch) try { // STAB20
    require(dispatch && memory.workarea_size>=kSSMotionWorkSize && !(memory.workarea&31),
            "SS motion: invalid workarea/dispatcher");
    require_ram(ctx,memory.workarea,kSSMotionWorkSize);
    for (unsigned i=0;i<4;++i) {
        require_ram(ctx,memory.raw_rings[i],kSSMotionRawRingSize);
        require(!(memory.raw_rings[i]&1) && !overlap(memory.workarea,kSSMotionWorkSize,memory.raw_rings[i],kSSMotionRawRingSize),
                "SS motion: overlapping or unaligned raw ring");
        for (unsigned j=0;j<i;++j)
            require(!overlap(memory.raw_rings[i],kSSMotionRawRingSize,memory.raw_rings[j],kSSMotionRawRingSize),
                    "SS motion: overlapping channel rings");
    }
    state=State{}; state.owner=&ctx; state.memory=memory; state.dispatch=dispatch;
    invoke(ctx,kInit,memory.workarea);
    for (int32_t channel=0;channel<4;++channel) {
        clear_ring(ctx,channel);
        invoke(ctx,kSetRing,channel,memory.raw_rings[channel],0,0);
    }
    state.initialized=true;
} catch (const GuestStopped&) { // STAB20: stopped during KMPLSInit/SetKpadRingBuffer: stays uninitialized
}
uint32_t ss_motion_latest_raw(int32_t channel) {
    return state.initialized && valid_channel(channel)?state.channels[channel].latest_raw:0;
}
bool ss_motion_read(CPUContext& ctx, int32_t channel, const input::MotionSample& sample,
                    uint64_t now_ns, uint32_t status) try { // STAB20
    require_owner(ctx);
    require(valid_channel(channel), "SS motion: invalid channel");
    require_ram(ctx,status,0xF0);
    auto& c=state.channels[channel];
    const uint8_t device=ctx.mmu.read8(status+0x5C);
    c.available=input::motion_sample_fresh(sample,now_ns) && (device==5 || device==6) &&
        ctx.mmu.read8(status+0x5D)==0;
    if (!c.available || sample.sequence==c.sequence) return false;
    if (c.sequence && (sample.sequence<c.sequence || sample.monotonic_ns<=c.timestamp)) return false;
    // A steady intentional turn can look still to both bias learners. Synthetic
    // inputs have no hardware bias; measured inputs already use the producer's
    // guided bias or must preserve the game's active steering reference. Freeze
    // KMPLS learning in those cases too, restoring the game's setting on exit.
    // Offset 198 and channel stride 3504 are the original KMPLSReset/Read layout.
    struct DriftGuard {
        CPUContext& ctx; uint32_t address; uint8_t saved;
        DriftGuard(CPUContext& c, uint32_t a, bool freeze) : ctx(c), address(a), saved(c.mmu.read8(a)) {
            if (freeze) ctx.mmu.write8(address, 0);
        }
        ~DriftGuard() { ctx.mmu.write8(address, saved); }
    } drift(ctx, state.memory.workarea + channel * 3504 + 198,
            (sample.kind == input::MotionSourceKind::Virtual && !sample.sensor_measured &&
             !(std::getenv("NWII_VIRTUAL_ZERO_DRIFT") && std::getenv("NWII_VIRTUAL_ZERO_DRIFT")[0] == '0')) ||
            (sample.sensor_measured && (!input::gyro_bias_learning_allowed(input::g_guest_context.get()) ||
                                       input::calibration_status(channel).applied)));
    if (c.sequence && read_history(ctx,channel,c,sample,status)) return true;
    double dt=sample.dt_seconds;
    if (c.sequence) {
        const uint64_t elapsed=sample.monotonic_ns-c.timestamp;
        if (elapsed>input::kMotionMaxAgeNs) {
            c.sequence=sample.sequence; c.timestamp=sample.monotonic_ns;
            return false; // Re-anchor; a latest-only source cannot reconstruct a gap.
        }
        dt=elapsed*1.e-9;
    }
    c.sequence=sample.sequence; c.timestamp=sample.monotonic_ns;
    const uint32_t raw=state.memory.raw_rings[channel]+c.slot*0x42;
    write_raw(ctx,raw,status,sample);
    c.latest_raw=raw;
    if (c.pending_callback) {
        const uint32_t callback=c.pending_callback;
        c.pending_callback=0;
        invoke(ctx,callback,channel,0);
    }
    invoke(ctx,kRead,channel,status+0xB0,(c.slot+2)%16,1,status,dt);
    c.slot=(c.slot+1)%16;
    for (unsigned i=0;i<15;++i)
        require(std::isfinite(ctx.mmu.read_f32(status+0xB0+i*4)), "SS motion: nonfinite original KMPLS output");
    return true;
} catch (const GuestStopped&) { // STAB20: stopped mid-read: no motion this read (the KPAD caller keeps its status)
    return false;
}
void ss_motion_reset_channel(CPUContext& ctx, int32_t channel) try { // STAB20
    require_owner(ctx); require(valid_channel(channel), "SS motion: invalid reset channel");
    auto& c=state.channels[channel];
    if (c.pending_callback) invoke(ctx,c.pending_callback,channel,uint32_t(-1));
    invoke(ctx,kStop,channel); invoke(ctx,kReset,channel);
    c=Channel{}; clear_ring(ctx,channel);
} catch (const GuestStopped&) { // STAB20: stopped mid-reset
}
bool ss_motion_control_request(CPUContext& ctx, int32_t channel, uint32_t command, uint32_t callback) {
    if (command!=0x80) return false;
    require_owner(ctx); require(valid_channel(channel), "SS motion: invalid control channel");
    auto& c=state.channels[channel];
    if (!c.available) { ctx.gpr[3]=uint32_t(-1); return true; }
    if (c.pending_callback) { ctx.gpr[3]=uint32_t(-2); return true; }
    if (callback) {
        require(!(callback&3), "SS motion: unaligned control callback");
        require_ram(ctx,callback,4);
        c.pending_callback=callback;
    }
    ctx.gpr[3]=0;
    return true;
}
void ss_motion_write_calibration(CPUContext& ctx, int32_t channel, uint32_t fast, uint32_t slow) {
    require_owner(ctx); require(valid_channel(channel), "SS motion: invalid calibration channel");
    require_ram(ctx,fast,0x1C); require_ram(ctx,slow,0x1C);
    for (unsigned record=0;record<2;++record) {
        const auto p=record?slow:fast;
        for (unsigned axis=0;axis<3;++axis) {
            ctx.mmu.write_f32(p+axis*8,kAdcZero);
            ctx.mmu.write_f32(p+axis*8+4,kAdcScale);
        }
        ctx.mmu.write32(p+0x18,record?kSlowDegrees:kFastDegrees);
    }
}
}

extern "C" void SS_WPADiGetMplsCalibration(nwii::runtime::CPUContext& ctx) {
    nwii::runtime::ss_motion_write_calibration(ctx,static_cast<int32_t>(ctx.gpr[3]),ctx.gpr[4],ctx.gpr[5]);
}
