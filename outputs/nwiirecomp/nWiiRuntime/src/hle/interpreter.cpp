#include "runtime/cpu_context.h"
#include "runtime/ss_boot.h" // env-gated NWII_TRACEPC diagnostic
#include "runtime/profiler.h"
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <bit>
#include <cmath>





namespace nwii {
namespace runtime {



struct CodeRange { uint32_t start, end; };
static CodeRange g_code_ranges[16];
static int g_code_range_count = 0;

void add_recompiled_range(uint32_t start, uint32_t end) {
    if (g_code_range_count < 16)
        g_code_ranges[g_code_range_count++] = {start, end};
}

// PERF2: read-only view of the ranges (idle-loop scan in ios.cpp).
int recompiled_range_count() { return g_code_range_count; }
void recompiled_range_get(int i, uint32_t& start, uint32_t& end) {
    start = g_code_ranges[i].start;
    end = g_code_ranges[i].end;
}

bool in_recompiled_code(uint32_t pc) {
    for (int i = 0; i < g_code_range_count; ++i)
        if (pc >= g_code_ranges[i].start && pc < g_code_ranges[i].end)
            return true;
    return false;
}

static inline void set_cr0(CPUContext& ctx, int32_t v) {
    ctx.cr[0].lt = v < 0;
    ctx.cr[0].gt = v > 0;
    ctx.cr[0].eq = v == 0;
    ctx.cr[0].so = (ctx.xer >> 31) & 1;
}

static inline void set_crf(CPUContext& ctx, uint32_t crf, bool lt, bool gt, bool eq) {
    ctx.cr[crf].lt = lt;
    ctx.cr[crf].gt = gt;
    ctx.cr[crf].eq = eq;
    ctx.cr[crf].so = (ctx.xer >> 31) & 1;
}

static inline bool& cr_bit(CPUContext& ctx, uint32_t bit) {
    // PowerPC numbers CR bits from the most significant bit: LT, GT, EQ, SO.
    auto& field = ctx.cr[bit / 4];
    switch (bit % 4) {
    case 0: return field.lt;
    case 1: return field.gt;
    case 2: return field.eq;
    default: return field.so;
    }
}

// FREEZE8: report hook for an unsupported instruction. core/freeze_watchdog.cpp installs one that logs
// the REL module/offset/symbol loudly and shows a message box; the FREEZE8 opcode probe installs one
// that throws. The process still stops afterwards (fail fast, never continue with a skipped opcode).
void (*g_interp_unsupported_hook)(const CPUContext& ctx, uint32_t insn, const char* kind,
                                  uint32_t selector) = nullptr;
// FREEZE8 watchdog inputs (single writer = CPU thread; the watchdog reads them racily).
volatile int g_interp_depth = 0;              // > 0 while interpret_step runs
volatile uint64_t g_interp_budget_exceeded = 0; // "[Interp] Budget exceeded" count

[[noreturn]] static void unsupported_instruction(const CPUContext& ctx,
                                                uint32_t insn,
                                                const char* kind,
                                                uint32_t selector) {
    if (g_interp_unsupported_hook) g_interp_unsupported_hook(ctx, insn, kind, selector);
    std::cerr << "[Interp] Unsupported " << kind
              << " pc=0x" << std::hex << ctx.pc
              << " insn=0x" << std::setw(8) << std::setfill('0') << insn
              << " lr=0x" << ctx.lr << std::setfill(' ') << std::dec
              << " op=" << (insn >> 26) << " selector=" << selector << std::endl;
    std::cout << std::flush;
    std::fflush(stdout);
    std::exit(EXIT_FAILURE);
}

// FREEZE8: SPRs the CPUContext does not model (BATs, HID0/1/4, SDR1, DAR/DSISR, EAR, PMCs, L2CR, ...)
// live in a plain register file, like Dolphin's ppcState.spr[] (Dolphin mfspr/mtspr just read/write the
// array for these). Segment registers likewise (Dolphin ppcState.sr[]). Nothing here translates memory.
static uint32_t g_spr_file[1024] = {};
static uint32_t g_segment_regs[16] = {};
static constexpr uint32_t kSprPvrBroadway = 0x00087102; // Dolphin PowerPC.cpp, Wii (Broadway) PVR

static uint32_t read_other_spr(const CPUContext& ctx, uint32_t spr) {
    switch (spr) {
    case 268: case 269: { // TBL/TBU via mfspr (Dolphin mfspr SPR_TL/SPR_TU)
        const uint64_t tb = ctx.read_timebase();
        return spr == 269 ? (uint32_t)(tb >> 32) : (uint32_t)tb;
    }
    case 272: case 273: case 274: case 275: return ctx.sprg[spr - 272];
    case 287: return kSprPvrBroadway;
    case 921: return g_spr_file[921] & ~0x1Fu; // WPAR: BNE=0 (gather pipe drained synchronously)
    default: return g_spr_file[spr & 1023];
    }
}

static void write_other_spr(CPUContext& ctx, uint32_t spr, uint32_t value) {
    switch (spr) {
    case 272: case 273: case 274: case 275: ctx.sprg[spr - 272] = value; return;
    case 287: return; // PVR is read-only
    default: g_spr_file[spr & 1023] = value; return; // incl. TBL/TBU writes (284/285): the timebase is
                                                     // derived from inst_count and is not rewritten here
    }
}

// TO-field trap condition shared by tw/twi (Dolphin Interpreter_Integer.cpp Helper_Trap).
static bool trap_condition(uint32_t to, int32_t a, int32_t b) {
    return ((to & 0x10) && a < b) || ((to & 0x08) && a > b) || ((to & 0x04) && a == b) ||
           ((to & 0x02) && (uint32_t)a < (uint32_t)b) || ((to & 0x01) && (uint32_t)a > (uint32_t)b);
}

static inline uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

bool interpret_one(CPUContext& ctx) {
    uint32_t pc = ctx.pc;
    if (pc == 0x801c6124) {
        static int count = 0;
        if (count++ < 20)
            std::cout << "[DEBUG] OSDispatchInterrupt: r0 (pending)=0x" << std::hex << ctx.gpr[0] << " r3 (mask)=0x" << ctx.gpr[3] << " r4 (result)=0x" << ctx.gpr[4] << std::dec << "\n";
    }
    uint32_t insn = ctx.mmu.read32(pc);
    uint32_t op = insn >> 26;
    uint32_t rD = (insn >> 21) & 0x1F;
    uint32_t rA = (insn >> 16) & 0x1F;
    uint32_t rB = (insn >> 11) & 0x1F;
    int16_t simm = (int16_t)(insn & 0xFFFF);
    uint16_t uimm = (uint16_t)(insn & 0xFFFF);
    bool rc = insn & 1;

    auto ea_ra0 = [&](int32_t off) {
        return (rA ? ctx.gpr[rA] : 0) + off;
    };

    switch (op) {
    case 3: // twi TO, rA, SIMM (FREEZE8): a taken trap is a program exception, not delivered here
        if (trap_condition(rD, (int32_t)ctx.gpr[rA], simm))
            unsupported_instruction(ctx, insn, "trap taken (program exception) twi", rD);
        break;
    case 7:
        ctx.gpr[rD] = (int32_t)ctx.gpr[rA] * simm;
        break;
    case 8: { 
        uint64_t r = (uint64_t)(uint32_t)(~ctx.gpr[rA]) + (uint32_t)simm + 1;
        ctx.gpr[rD] = (uint32_t)r;
        if (r >> 32) ctx.xer |= 0x20000000; else ctx.xer &= ~0x20000000;
        break;
    }
    case 10: { 
        uint32_t a = ctx.gpr[rA];
        uint32_t b = uimm;
        set_crf(ctx, rD >> 2, a < b, a > b, a == b);
        break;
    }
    case 11: { 
        int32_t a = (int32_t)ctx.gpr[rA];
        set_crf(ctx, rD >> 2, a < simm, a > simm, a == simm);
        break;
    }
    case 12: { 
        uint64_t r = (uint64_t)ctx.gpr[rA] + (uint32_t)(int32_t)simm;
        ctx.gpr[rD] = (uint32_t)r;
        if (r >> 32) ctx.xer |= 0x20000000; else ctx.xer &= ~0x20000000;
        break;
    }
    case 13: { 
        uint64_t r = (uint64_t)ctx.gpr[rA] + (uint32_t)(int32_t)simm;
        ctx.gpr[rD] = (uint32_t)r;
        if (r >> 32) ctx.xer |= 0x20000000; else ctx.xer &= ~0x20000000;
        set_cr0(ctx, (int32_t)ctx.gpr[rD]);
        break;
    }
    case 14: 
        ctx.gpr[rD] = (rA ? ctx.gpr[rA] : 0) + simm;
        break;
    case 15: 
        ctx.gpr[rD] = (rA ? ctx.gpr[rA] : 0) + ((int32_t)simm << 16);
        break;
    case 16: { 
        uint32_t BO = rD, BI = rA;
        int32_t bd = (int16_t)(insn & 0xFFFC);
        bool ctr_ok = true;
        if (!(BO & 0x04)) {
            ctx.ctr--;
            ctr_ok = (BO & 0x02) ? (ctx.ctr == 0) : (ctx.ctr != 0);
        }
        bool cond_ok = true;
        if (!(BO & 0x10)) {
            uint32_t ci = BI / 4, cb = BI % 4;
            bool bit = cb == 0 ? ctx.cr[ci].lt : cb == 1 ? ctx.cr[ci].gt
                     : cb == 2 ? ctx.cr[ci].eq : ctx.cr[ci].so;
            cond_ok = (BO & 0x08) ? bit : !bit;
        }
        if (insn & 1) ctx.lr = pc + 4;
        ctx.pc = (ctr_ok && cond_ok) ? ((insn & 2) ? (uint32_t)bd : pc + bd)
                                     : pc + 4;
        return true;
    }
    case 17: 
        // sc has completed when its handler dispatches an interrupt/callback.
        // Save the continuation, and preserve any redirected PC on a yield.
        ctx.pc = pc + 4;
        if (handle_syscall(ctx)) return true;
        break;
    case 18: { 
        int32_t li = insn & 0x03FFFFFC;
        if (li & 0x02000000) li |= 0xFC000000;
        if (insn & 1) ctx.lr = pc + 4;
        ctx.pc = (insn & 2) ? (uint32_t)li : pc + li;
        return true;
    }
    case 19: { 
        uint32_t xo = (insn >> 1) & 0x3FF;
        if (xo == 257 || xo == 129 || xo == 289 || xo == 225 ||
            xo == 33 || xo == 449 || xo == 417 || xo == 193) {
            // These fields are BT, BA and BB, not GPR indices. Read both
            // sources before writing BT so overlapping operands work too.
            const bool a = cr_bit(ctx, rA);
            const bool b = cr_bit(ctx, rB);
            bool result;
            switch (xo) {
            case 257: result = a && b; break;       // crand
            case 129: result = a && !b; break;      // crandc
            case 289: result = a == b; break;       // creqv
            case 225: result = !(a && b); break;    // crnand
            case 33: result = !(a || b); break;     // crnor
            case 449: result = a || b; break;       // cror
            case 417: result = a || !b; break;      // crorc
            default: result = a != b; break;        // crxor
            }
            cr_bit(ctx, rD) = result;
            break;
        }
        if (xo == 16) { 
            uint32_t BO = rD, BI = rA;
            bool ctr_ok = true;
            if (!(BO & 0x04)) {
                ctx.ctr--;
                ctr_ok = (BO & 0x02) ? (ctx.ctr == 0) : (ctx.ctr != 0);
            }
            bool cond_ok = true;
            if (!(BO & 0x10)) {
                uint32_t ci = BI / 4, cb = BI % 4;
                bool bit = cb == 0 ? ctx.cr[ci].lt : cb == 1 ? ctx.cr[ci].gt
                         : cb == 2 ? ctx.cr[ci].eq : ctx.cr[ci].so;
                cond_ok = (BO & 0x08) ? bit : !bit;
            }
            uint32_t target = ctx.lr & ~3u;
            if (insn & 1) ctx.lr = pc + 4;
            ctx.pc = (ctr_ok && cond_ok) ? target : pc + 4;
            return true;
        }
        if (xo == 528) { 
            uint32_t BO = rD, BI = rA;
            bool cond_ok = true;
            if (!(BO & 0x10)) {
                uint32_t ci = BI / 4, cb = BI % 4;
                bool bit = cb == 0 ? ctx.cr[ci].lt : cb == 1 ? ctx.cr[ci].gt
                         : cb == 2 ? ctx.cr[ci].eq : ctx.cr[ci].so;
                cond_ok = (BO & 0x08) ? bit : !bit;
            }
            uint32_t target = ctx.ctr & ~3u;
            if (insn & 1) ctx.lr = pc + 4;
            ctx.pc = cond_ok ? target : pc + 4;
            return true;
        }
        if (xo == 0) { // mcrf
            uint32_t crfD = rD >> 2;
            uint32_t crfS = rA >> 2;
            ctx.cr[crfD] = ctx.cr[crfS];
            ctx.pc = pc + 4;
            return true;
        }
        if (xo == 50) { 
            uint32_t ee_was = ctx.msr & 0x8000;
            ctx.msr = ctx.srr1;
            ctx.pc = ctx.srr0;
            ctx.dispatch_saved_ctx = 0;
            // FP Unavailable, taken eagerly (CPUContext::fp_unavailable);
            // before any interrupt can save the new context's FPU state.
            if (g_fp_unavailable_emulation) ctx.fp_unavailable_check();
            if (!ee_was && (ctx.msr & 0x8000)) {
                if (process_pending_callbacks(ctx)) return true;
            }
            return true;
        }
        if (xo == 150) break; 
        unsupported_instruction(ctx, insn, "op19", xo);
    }
    case 20: { 
        uint32_t sh = rB, mb = (insn >> 6) & 0x1F, me = (insn >> 1) & 0x1F;
        uint32_t r = (ctx.gpr[rD] << sh) | (ctx.gpr[rD] >> ((32 - sh) & 31));
        uint32_t mask = (mb <= me)
            ? (((uint32_t)-1 >> mb) & ((uint32_t)-1 << (31 - me)))
            : (((uint32_t)-1 >> mb) | ((uint32_t)-1 << (31 - me)));
        ctx.gpr[rA] = (r & mask) | (ctx.gpr[rA] & ~mask);
        if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]);
        break;
    }
    case 21: { 
        uint32_t sh = rB, mb = (insn >> 6) & 0x1F, me = (insn >> 1) & 0x1F;
        uint32_t r = (ctx.gpr[rD] << sh) | (ctx.gpr[rD] >> ((32 - sh) & 31));
        uint32_t mask = (mb <= me)
            ? (((uint32_t)-1 >> mb) & ((uint32_t)-1 << (31 - me)))
            : (((uint32_t)-1 >> mb) | ((uint32_t)-1 << (31 - me)));
        ctx.gpr[rA] = r & mask;
        if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]);
        break;
    }
    case 23: { 
        uint32_t sh = ctx.gpr[rB] & 0x1F, mb = (insn >> 6) & 0x1F, me = (insn >> 1) & 0x1F;
        uint32_t r = (ctx.gpr[rD] << sh) | (ctx.gpr[rD] >> ((32 - sh) & 31));
        uint32_t mask = (mb <= me)
            ? (((uint32_t)-1 >> mb) & ((uint32_t)-1 << (31 - me)))
            : (((uint32_t)-1 >> mb) | ((uint32_t)-1 << (31 - me)));
        ctx.gpr[rA] = r & mask;
        if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]);
        break;
    }
    case 24: ctx.gpr[rA] = ctx.gpr[rD] | uimm; break;            
    case 25: ctx.gpr[rA] = ctx.gpr[rD] | ((uint32_t)uimm << 16); break; 
    case 26: ctx.gpr[rA] = ctx.gpr[rD] ^ uimm; break;            
    case 27: ctx.gpr[rA] = ctx.gpr[rD] ^ ((uint32_t)uimm << 16); break; 
    case 28: ctx.gpr[rA] = ctx.gpr[rD] & uimm; set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
    case 29: ctx.gpr[rA] = ctx.gpr[rD] & ((uint32_t)uimm << 16); set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
    case 31: {
        uint32_t xo = (insn >> 1) & 0x3FF;
        switch (xo) {
        case 0: { 
            int32_t a = (int32_t)ctx.gpr[rA], b = (int32_t)ctx.gpr[rB];
            set_crf(ctx, rD >> 2, a < b, a > b, a == b);
            break;
        }
        case 32: { 
            uint32_t a = ctx.gpr[rA], b = ctx.gpr[rB];
            set_crf(ctx, rD >> 2, a < b, a > b, a == b);
            break;
        }
        case 10: case 522:      // addc[ o ]
        case 8: case 520:       // subfc[ o ]
        case 138: case 650:     // adde[ o ]
        case 202: case 714:     // addze[ o ]
        case 200: case 712:     // subfze[ o ]
        case 234: case 746:     // addme[ o ]
        case 232: case 744:     // subfme[ o ]
        case 136: case 648: {   // subfe[ o ]
            // Every carrying XO-form: CA = carry out of the 32-bit sum
            // (Dolphin Interpreter_Integer.cpp addcx/subfcx/addex/...).
            const uint32_t a = ctx.gpr[rA], b = ctx.gpr[rB];
            const uint32_t carry = (ctx.xer >> 29) & 1;
            uint64_t sum;
            int64_t signed_sum;
            switch (xo & 0x1FF) {
            case 10:
                sum = (uint64_t)a + b;
                signed_sum = (int64_t)(int32_t)a + (int32_t)b;
                break;
            case 8:
                sum = (uint64_t)(uint32_t)~a + b + 1;
                signed_sum = (int64_t)(int32_t)b - (int32_t)a;
                break;
            case 138:
                sum = (uint64_t)a + b + carry;
                signed_sum = (int64_t)(int32_t)a + (int32_t)b + carry;
                break;
            case 202:
                sum = (uint64_t)a + carry;
                signed_sum = (int64_t)(int32_t)a + carry;
                break;
            case 200:
                sum = (uint64_t)(uint32_t)~a + carry;
                signed_sum = (int64_t)(int32_t)~a + carry;
                break;
            case 234:
                sum = (uint64_t)a + 0xFFFFFFFFull + carry;
                signed_sum = (int64_t)(int32_t)a - 1 + carry;
                break;
            case 232:
                sum = (uint64_t)(uint32_t)~a + 0xFFFFFFFFull + carry;
                signed_sum = (int64_t)(int32_t)~a - 1 + carry;
                break;
            default:
                sum = (uint64_t)(uint32_t)~a + b + carry;
                signed_sum = (int64_t)(int32_t)b - (int32_t)a + carry - 1;
                break;
            }
            ctx.gpr[rD] = (uint32_t)sum;
            ctx.xer = (ctx.xer & ~0x20000000u) | ((uint32_t)(sum >> 32) << 29);
            if (xo & 0x200) { // OE updates OV and preserves sticky SO.
                const bool overflow = signed_sum < INT32_MIN || signed_sum > INT32_MAX;
                ctx.xer = (ctx.xer & ~0x40000000u) | (overflow ? 0xC0000000u : 0);
            }
            if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rD]);
            break;
        }
        case 266: ctx.gpr[rD] = ctx.gpr[rA] + ctx.gpr[rB]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rD]); break;
        case 40:  ctx.gpr[rD] = ctx.gpr[rB] - ctx.gpr[rA]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rD]); break;
        case 104: ctx.gpr[rD] = -(int32_t)ctx.gpr[rA]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rD]); break;
        case 235: ctx.gpr[rD] = (int32_t)ctx.gpr[rA] * (int32_t)ctx.gpr[rB]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rD]); break;
        case 778: case 552: case 616: case 747: { // FREEZE8: addo/subfo/nego/mullwo (Dolphin addx/subfx/negx/mullwx, OE=1)
            const int64_t a = (int32_t)ctx.gpr[rA], b = (int32_t)ctx.gpr[rB];
            const int64_t wide = xo == 778 ? a + b : xo == 552 ? b - a : xo == 616 ? -a : a * b;
            ctx.gpr[rD] = (uint32_t)wide;
            const bool overflow = wide < INT32_MIN || wide > INT32_MAX; // OV; SO is sticky
            ctx.xer = (ctx.xer & ~0x40000000u) | (overflow ? 0xC0000000u : 0);
            if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rD]);
            break;
        }
        case 75:  ctx.gpr[rD] = (uint32_t)(((int64_t)(int32_t)ctx.gpr[rA] * (int32_t)ctx.gpr[rB]) >> 32); if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rD]); break;
        case 11:  ctx.gpr[rD] = (uint32_t)(((uint64_t)ctx.gpr[rA] * ctx.gpr[rB]) >> 32); if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rD]); break;
        case 491: case 1003: { // divw[o]: Dolphin divwx; never run the host's trapping division
            const int32_t a = (int32_t)ctx.gpr[rA], b = (int32_t)ctx.gpr[rB];
            const bool ovf = b == 0 || ((uint32_t)a == 0x80000000u && b == -1);
            ctx.gpr[rD] = ovf ? (a < 0 ? 0xFFFFFFFFu : 0u) : (uint32_t)(a / b);
            if (xo & 0x200) ctx.xer = ovf ? (ctx.xer | 0xC0000000u) : (ctx.xer & ~0x40000000u);
            if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rD]);
            break;
        }
        case 459: case 971: { // divwu[o]: Dolphin divwux
            const uint32_t a = ctx.gpr[rA], b = ctx.gpr[rB];
            ctx.gpr[rD] = b == 0 ? 0u : a / b;
            if (xo & 0x200) ctx.xer = (b == 0) ? (ctx.xer | 0xC0000000u) : (ctx.xer & ~0x40000000u);
            if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rD]);
            break;
        }
        case 28:  ctx.gpr[rA] = ctx.gpr[rD] & ctx.gpr[rB]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
        case 60:  ctx.gpr[rA] = ctx.gpr[rD] & ~ctx.gpr[rB]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
        case 444: ctx.gpr[rA] = ctx.gpr[rD] | ctx.gpr[rB]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
        case 412: ctx.gpr[rA] = ctx.gpr[rD] | ~ctx.gpr[rB]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
        case 316: ctx.gpr[rA] = ctx.gpr[rD] ^ ctx.gpr[rB]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
        case 476: ctx.gpr[rA] = ~(ctx.gpr[rD] & ctx.gpr[rB]); if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
        case 124: ctx.gpr[rA] = ~(ctx.gpr[rD] | ctx.gpr[rB]); if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
        case 284: ctx.gpr[rA] = ~(ctx.gpr[rD] ^ ctx.gpr[rB]); if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
        case 954: ctx.gpr[rA] = (int32_t)(int8_t)ctx.gpr[rD]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
        case 922: ctx.gpr[rA] = (int32_t)(int16_t)ctx.gpr[rD]; if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]); break; 
        case 26: { 
            uint32_t v = ctx.gpr[rD];
            ctx.gpr[rA] = std::countl_zero(v);
            if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]);
            break;
        }
        case 24: { 
            uint32_t sh = ctx.gpr[rB] & 0x3F;
            ctx.gpr[rA] = sh > 31 ? 0 : ctx.gpr[rD] << sh;
            if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]);
            break;
        }
        case 536: { 
            uint32_t sh = ctx.gpr[rB] & 0x3F;
            ctx.gpr[rA] = sh > 31 ? 0 : ctx.gpr[rD] >> sh;
            if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]);
            break;
        }
        case 792: { 
            uint32_t sh = ctx.gpr[rB] & 0x3F;
            int32_t v = (int32_t)ctx.gpr[rD];
            uint32_t ca;
            if (sh > 31) { ctx.gpr[rA] = (v < 0) ? 0xFFFFFFFF : 0; ca = (v < 0) ? 1 : 0; }
            else { ctx.gpr[rA] = (uint32_t)(v >> sh);
                   ca = (v < 0 && (uint32_t)(v & ((sh==0)?0:((1u<<sh)-1))) != 0) ? 1 : 0; }
            ctx.xer = (ctx.xer & ~(1u << 29)) | (ca << 29);
            if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]);
            break;
        }
        case 824: { 
            uint32_t sh = rB;
            int32_t v = (int32_t)ctx.gpr[rD];
            ctx.gpr[rA] = (uint32_t)(v >> sh);
            uint32_t ca = (v < 0 && (uint32_t)(v & ((sh==0)?0:((1u<<sh)-1))) != 0) ? 1 : 0;
            ctx.xer = (ctx.xer & ~(1u << 29)) | (ca << 29);
            if (rc) set_cr0(ctx, (int32_t)ctx.gpr[rA]);
            break;
        }
        case 339: { 
            uint32_t spr = ((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5);
            if (spr == 8) ctx.gpr[rD] = ctx.lr;
            else if (spr == 9) ctx.gpr[rD] = ctx.ctr;
            else if (spr == 1) ctx.gpr[rD] = ctx.xer;
            else if (spr == 22) ctx.gpr[rD] = ctx.read_dec();
            else if (spr == 26) ctx.gpr[rD] = ctx.srr0;
            else if (spr == 27) ctx.gpr[rD] = ctx.srr1;
            else if (spr >= 912 && spr <= 919) ctx.gpr[rD] = ctx.gqr[spr - 912];
            else if (spr == 920 || spr == 922 || spr == 923)
                ctx.gpr[rD] = ctx.read_lc_spr(spr);
            else ctx.gpr[rD] = read_other_spr(ctx, spr); // FREEZE8: Dolphin spr[] file semantics
            break;
        }
        case 467: { 
            uint32_t spr = ((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5);
            if (spr == 8) ctx.lr = ctx.gpr[rD];
            else if (spr == 9) ctx.ctr = ctx.gpr[rD];
            else if (spr == 1) ctx.xer = ctx.gpr[rD];
            else if (spr == 22) ctx.write_dec(ctx.gpr[rD]);
            else if (spr == 26) ctx.srr0 = ctx.gpr[rD];
            else if (spr == 27) ctx.srr1 = ctx.gpr[rD];
            else if (spr >= 912 && spr <= 919) ctx.gqr[spr - 912] = ctx.gpr[rD];
            else if (spr == 920 || spr == 922 || spr == 923)
                ctx.write_lc_spr(spr, ctx.gpr[rD]);
            else write_other_spr(ctx, spr, ctx.gpr[rD]); // FREEZE8: Dolphin spr[] file semantics
            break;
        }
        case 371: { 
            uint32_t spr = ((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5);
            if (spr != 268 && spr != 269)
                unsupported_instruction(ctx, insn, "mftb selector", spr);
            uint64_t tb = ctx.read_timebase();
            ctx.gpr[rD] = (spr == 269) ? (uint32_t)(tb >> 32) : (uint32_t)tb;
            break;
        }
        case 83:  ctx.gpr[rD] = ctx.msr; break;  
        case 146: // mtmsr; FP Unavailable taken eagerly (CPUContext::fp_unavailable)
            ctx.msr = ctx.gpr[rD];
            if (g_fp_unavailable_emulation) ctx.fp_unavailable_check();
            break;
        case 19: { 
            uint32_t v = 0;
            for (int i = 0; i < 8; i++) {
                v |= (ctx.cr[i].lt ? 8u : 0) << (28 - i * 4);
                v |= (ctx.cr[i].gt ? 4u : 0) << (28 - i * 4);
                v |= (ctx.cr[i].eq ? 2u : 0) << (28 - i * 4);
                v |= (ctx.cr[i].so ? 1u : 0) << (28 - i * 4);
            }
            ctx.gpr[rD] = v;
            break;
        }
        case 144: { 
            uint32_t crm = (insn >> 12) & 0xFF;
            uint32_t v = ctx.gpr[rD];
            for (int i = 0; i < 8; i++) {
                if (crm & (0x80 >> i)) {
                    uint32_t nib = (v >> (28 - i * 4)) & 0xF;
                    ctx.cr[i].lt = nib & 8;
                    ctx.cr[i].gt = nib & 4;
                    ctx.cr[i].eq = nib & 2;
                    ctx.cr[i].so = nib & 1;
                }
            }
            break;
        }
        case 23:  ctx.gpr[rD] = ctx.mmu.read32(ea_ra0(0) + ctx.gpr[rB]); break;  
        case 87:  ctx.gpr[rD] = ctx.mmu.read8(ea_ra0(0) + ctx.gpr[rB]); break;   
        case 279: ctx.gpr[rD] = ctx.mmu.read16(ea_ra0(0) + ctx.gpr[rB]); break;  
        case 311: { // lhzux; capture EA before a possible rD/rB overlap.
            const uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB];
            ctx.gpr[rD] = ctx.mmu.read16(ea);
            ctx.gpr[rA] = ea;
            break;
        }
        case 343: ctx.gpr[rD] = (int32_t)(int16_t)ctx.mmu.read16(ea_ra0(0) + ctx.gpr[rB]); break; 
        case 151: ctx.mmu.write32(ea_ra0(0) + ctx.gpr[rB], ctx.gpr[rD]); break;  
        case 215: ctx.mmu.write8(ea_ra0(0) + ctx.gpr[rB], (uint8_t)ctx.gpr[rD]); break; 
        case 407: ctx.mmu.write16(ea_ra0(0) + ctx.gpr[rB], (uint16_t)ctx.gpr[rD]); break; 
        case 183: { // stwux; store the old source value before updating rA.
            const uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB];
            ctx.mmu.write32(ea, ctx.gpr[rD]);
            ctx.gpr[rA] = ea;
            break;
        }
        case 439: { // sthux
            const uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB];
            ctx.mmu.write16(ea, (uint16_t)ctx.gpr[rD]);
            ctx.gpr[rA] = ea;
            break;
        }
        case 20: { 
            ctx.gpr[rD] = ctx.mmu.read32(ea_ra0(0) + ctx.gpr[rB]);
            ctx.reservation_addr = ea_ra0(0) + ctx.gpr[rB];
            break;
        }
        case 150: { 
            ctx.mmu.write32(ea_ra0(0) + ctx.gpr[rB], ctx.gpr[rD]);
            ctx.cr[0].lt = ctx.cr[0].gt = false;
            ctx.cr[0].eq = true;
            ctx.cr[0].so = (ctx.xer >> 31) & 1;
            break;
        }
        case 1014: { 
            ctx.zero_cache_line(ea_ra0(0) + ctx.gpr[rB]);
            break;
        }
        
        case 535: ctx.fpr[rD] = ctx.ps1[rD] = ctx.mmu.read_f32(ea_ra0(0) + ctx.gpr[rB]); break; // lfsx: Gekko copies to ps1 (RELAOT diff)
        case 567: { uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB]; ctx.fpr[rD] = ctx.ps1[rD] = ctx.mmu.read_f32(ea); ctx.gpr[rA] = ea; break; } // lfsux: ps1 too
        case 599: ctx.fpr[rD] = ctx.mmu.read_f64(ea_ra0(0) + ctx.gpr[rB]); break; 
        case 631: { uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB]; ctx.fpr[rD] = ctx.mmu.read_f64(ea); ctx.gpr[rA] = ea; break; } 
        case 663: ctx.mmu.write_f32(ea_ra0(0) + ctx.gpr[rB], (float)ctx.fpr[rD]); break; 
        case 695: { uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB]; ctx.mmu.write_f32(ea, (float)ctx.fpr[rD]); ctx.gpr[rA] = ea; break; } 
        case 727: ctx.mmu.write_f64(ea_ra0(0) + ctx.gpr[rB], ctx.fpr[rD]); break; 
        case 759: { uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB]; ctx.mmu.write_f64(ea, ctx.fpr[rD]); ctx.gpr[rA] = ea; break; } 
        case 983: { 
            uint64_t bits; std::memcpy(&bits, &ctx.fpr[rD], 8);
            ctx.mmu.write32(ea_ra0(0) + ctx.gpr[rB], (uint32_t)bits);
            break;
        }
        // FREEZE8: the remaining Gekko op31 forms (Dolphin Interpreter_LoadStore/_Integer/_SystemRegisters).
        // Update forms capture EA before writing rD/rA.
        case 55: { const uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB]; ctx.gpr[rD] = ctx.mmu.read32(ea); ctx.gpr[rA] = ea; break; } // lwzux
        case 119: { const uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB]; ctx.gpr[rD] = ctx.mmu.read8(ea); ctx.gpr[rA] = ea; break; } // lbzux
        case 375: { const uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB]; ctx.gpr[rD] = (int32_t)(int16_t)ctx.mmu.read16(ea); ctx.gpr[rA] = ea; break; } // lhaux
        case 247: { const uint32_t ea = ctx.gpr[rA] + ctx.gpr[rB]; ctx.mmu.write8(ea, (uint8_t)ctx.gpr[rD]); ctx.gpr[rA] = ea; break; } // stbux
        case 534: ctx.gpr[rD] = bswap32(ctx.mmu.read32(ea_ra0(0) + ctx.gpr[rB])); break;                       // lwbrx
        case 790: { const uint16_t h = ctx.mmu.read16(ea_ra0(0) + ctx.gpr[rB]); ctx.gpr[rD] = (uint16_t)((h >> 8) | (h << 8)); break; } // lhbrx
        case 662: ctx.mmu.write32(ea_ra0(0) + ctx.gpr[rB], bswap32(ctx.gpr[rD])); break;                       // stwbrx
        case 918: { const uint16_t h = (uint16_t)ctx.gpr[rD]; ctx.mmu.write16(ea_ra0(0) + ctx.gpr[rB], (uint16_t)((h >> 8) | (h << 8))); break; } // sthbrx
        case 597: { // lswi rD, rA|0, NB
            uint32_t ea = ea_ra0(0), n = rB ? rB : 32, r = rD - 1, i = 0;
            while (n > 0) {
                if (i == 0) { r = (r + 1) & 31; ctx.gpr[r] = 0; }
                ctx.gpr[r] |= (uint32_t)ctx.mmu.read8(ea) << (24 - i);
                i = (i + 8) & 31; ++ea; --n;
            }
            break;
        }
        case 533: { // lswx rD, rA|0, rB; byte count = XER[25:31]
            const uint32_t ea = ea_ra0(0) + ctx.gpr[rB], count = ctx.xer & 0x7F;
            for (uint32_t n = 0; n < count; ++n) {
                const uint32_t reg = (rD + (n >> 2)) & 31, shift = 24 - ((n & 3) << 3);
                if ((n & 3) == 0) ctx.gpr[reg] = 0;
                ctx.gpr[reg] |= (uint32_t)ctx.mmu.read8(ea + n) << shift;
            }
            break;
        }
        case 725: { // stswi rS, rA|0, NB
            uint32_t ea = ea_ra0(0), n = rB ? rB : 32, r = rD - 1, i = 0;
            while (n > 0) {
                if (i == 0) r = (r + 1) & 31;
                ctx.mmu.write8(ea, (uint8_t)(ctx.gpr[r] >> (24 - i)));
                i = (i + 8) & 31; ++ea; --n;
            }
            break;
        }
        case 661: { // stswx rS, rA|0, rB
            uint32_t ea = ea_ra0(0) + ctx.gpr[rB], n = ctx.xer & 0x7F, r = rD, i = 0;
            while (n > 0) {
                ctx.mmu.write8(ea, (uint8_t)(ctx.gpr[r] >> (24 - i)));
                ++ea; --n; i += 8;
                if (i == 32) { i = 0; r = (r + 1) & 31; }
            }
            break;
        }
        case 512: { // mcrxr crfD: CR field = XER[SO,OV,CA,0]; clear SO/OV/CA
            const uint32_t f = ctx.xer >> 28;
            set_crf(ctx, rD >> 2, (f & 8) != 0, (f & 4) != 0, (f & 2) != 0);
            ctx.cr[rD >> 2].so = (f & 1) != 0;
            ctx.xer &= ~0xE0000000u;
            break;
        }
        case 4: // tw TO, rA, rB: a taken trap is a program exception, which this runtime does not deliver
            if (trap_condition(rD, (int32_t)ctx.gpr[rA], (int32_t)ctx.gpr[rB]))
                unsupported_instruction(ctx, insn, "trap taken (program exception) tw", rD);
            break;
        case 310: case 438: { // eciwx / ecowx: need EAR.E and word alignment, else DSI/alignment exception
            const uint32_t ea = ea_ra0(0) + ctx.gpr[rB];
            if (!(g_spr_file[282] & 0x80000000u) || (ea & 3))
                unsupported_instruction(ctx, insn, "external-control access exception (EAR.E=0 or unaligned)", xo);
            if (xo == 310) ctx.gpr[rD] = ctx.mmu.read32(ea);
            else ctx.mmu.write32(ea, ctx.gpr[rD]);
            break;
        }
        case 595: ctx.gpr[rD] = g_segment_regs[rA & 15]; break;                    // mfsr
        case 659: ctx.gpr[rD] = g_segment_regs[ctx.gpr[rB] >> 28]; break;         // mfsrin
        case 210: g_segment_regs[rA & 15] = ctx.gpr[rD]; break;                    // mtsr
        case 242: g_segment_regs[ctx.gpr[rB] >> 28] = ctx.gpr[rD]; break;         // mtsrin
        case 278: case 306: case 566: break; // dcbt (hint), tlbie, tlbsync: no cache/TLB model (Dolphin: no-op)
        case 54: case 86: case 246: case 470: case 598: case 982: case 854:
            break;
        default:
            unsupported_instruction(ctx, insn, "op31", xo);
        }
        break;
    }
    case 32: ctx.gpr[rD] = ctx.mmu.read32(ea_ra0(simm)); break;           
    case 33: ctx.gpr[rD] = ctx.mmu.read32(ctx.gpr[rA] + simm); ctx.gpr[rA] += simm; break; 
    case 34: ctx.gpr[rD] = ctx.mmu.read8(ea_ra0(simm)); break;            
    case 35: ctx.gpr[rD] = ctx.mmu.read8(ctx.gpr[rA] + simm); ctx.gpr[rA] += simm; break;  
    case 36: ctx.mmu.write32(ea_ra0(simm), ctx.gpr[rD]); break;           
    case 37: ctx.mmu.write32(ctx.gpr[rA] + simm, ctx.gpr[rD]); ctx.gpr[rA] += simm; break; 
    case 38: ctx.mmu.write8(ea_ra0(simm), (uint8_t)ctx.gpr[rD]); break;   
    case 39: ctx.mmu.write8(ctx.gpr[rA] + simm, (uint8_t)ctx.gpr[rD]); ctx.gpr[rA] += simm; break; 
    case 40: ctx.gpr[rD] = ctx.mmu.read16(ea_ra0(simm)); break;           
    case 41: ctx.gpr[rD] = ctx.mmu.read16(ctx.gpr[rA] + simm); ctx.gpr[rA] += simm; break; 
    case 42: ctx.gpr[rD] = (int32_t)(int16_t)ctx.mmu.read16(ea_ra0(simm)); break; 
    case 43: { // lhau
        const uint32_t ea = ctx.gpr[rA] + simm;
        ctx.gpr[rD] = (int32_t)(int16_t)ctx.mmu.read16(ea);
        ctx.gpr[rA] = ea;
        break;
    }
    case 44: ctx.mmu.write16(ea_ra0(simm), (uint16_t)ctx.gpr[rD]); break; 
    case 45: ctx.mmu.write16(ctx.gpr[rA] + simm, (uint16_t)ctx.gpr[rD]); ctx.gpr[rA] += simm; break; 
    case 46: { 
        uint32_t ea = ea_ra0(simm);
        for (uint32_t r = rD; r < 32; r++, ea += 4) ctx.gpr[r] = ctx.mmu.read32(ea);
        break;
    }
    case 47: { 
        uint32_t ea = ea_ra0(simm);
        for (uint32_t r = rD; r < 32; r++, ea += 4) ctx.mmu.write32(ea, ctx.gpr[r]);
        break;
    }
    case 48: ctx.fpr[rD] = ctx.ps1[rD] = ctx.mmu.read_f32(ea_ra0(simm)); break; 
    case 49: { 
        uint32_t ea = ctx.gpr[rA] + simm;
        ctx.fpr[rD] = ctx.ps1[rD] = ctx.mmu.read_f32(ea); // lfsu: ps1 too
        ctx.gpr[rA] = ea;
        break;
    }
    case 50: ctx.fpr[rD] = ctx.mmu.read_f64(ea_ra0(simm)); break;         
    case 51: { 
        uint32_t ea = ctx.gpr[rA] + simm;
        ctx.fpr[rD] = ctx.mmu.read_f64(ea);
        ctx.gpr[rA] = ea;
        break;
    }
    case 52: ctx.mmu.write_f32(ea_ra0(simm), (float)ctx.fpr[rD]); break;  
    case 53: { 
        uint32_t ea = ctx.gpr[rA] + simm;
        ctx.mmu.write_f32(ea, (float)ctx.fpr[rD]);
        ctx.gpr[rA] = ea;
        break;
    }
    case 54: ctx.mmu.write_f64(ea_ra0(simm), ctx.fpr[rD]); break;         
    case 55: { 
        uint32_t ea = ctx.gpr[rA] + simm;
        ctx.mmu.write_f64(ea, ctx.fpr[rD]);
        ctx.gpr[rA] = ea;
        break;
    }

    case 56: case 57: case 60: case 61: {
        int32_t d = (int32_t)(insn << 20) >> 20; 
        uint32_t W = (insn >> 15) & 1;
        uint32_t I = (insn >> 12) & 7;
        uint32_t ea = (op == 57 || op == 61) ? ctx.gpr[rA] + d : ea_ra0(d);
        if (op == 56 || op == 57) ctx.psq_load(rD, ea, W, I);
        else                      ctx.psq_store(rD, ea, W, I);
        if (op == 57 || op == 61) ctx.gpr[rA] = ea;
        break;
    }
    case 59: { // single-precision A-form: Gekko writes the result to BOTH lanes (Dolphin Fill)
        uint32_t xo5 = (insn >> 1) & 0x1F;
        uint32_t fC = (insn >> 6) & 0x1F;
        double a = ctx.fpr[rA], b = ctx.fpr[rB], c = ctx.fpr[fC];
        double r;
        switch (xo5) {
        case 18: r = fp_round_single(a / b); break;                                  // fdivs
        case 20: r = fp_round_single(a - b); break;                                  // fsubs
        case 21: r = fp_round_single(a + b); break;                                  // fadds
        case 22: r = fp_round_single(std::sqrt(b)); break;                           // fsqrts
        case 24: r = fp_fres(b); break;                                              // fres (table estimate)
        case 25: r = fp_round_single(a * fp_force25(c)); break;                      // fmuls
        case 28: r = fp_round_single(fp_madd_single(a, c, -b)); break;               // fmsubs
        case 29: r = fp_round_single(fp_madd_single(a, c, b)); break;                // fmadds
        case 30: r = fp_neg_unless_nan(fp_round_single(fp_madd_single(a, c, -b))); break; // fnmsubs
        case 31: r = fp_neg_unless_nan(fp_round_single(fp_madd_single(a, c, b))); break;  // fnmadds
        default:
            unsupported_instruction(ctx, insn, "op59", xo5);
            r = 0.0;
        }
        ctx.fpr[rD] = r;
        ctx.ps1[rD] = r;
        break;
    }
    case 63: { 
        uint32_t xo5 = (insn >> 1) & 0x1F;
        uint32_t xo10 = (insn >> 1) & 0x3FF;
        uint32_t fC = (insn >> 6) & 0x1F;
        double a = ctx.fpr[rA], b = ctx.fpr[rB], c = ctx.fpr[fC];
        // Double-precision ops write ps0 only (Dolphin SetPS0); frsp fills both lanes.
        bool aform = true;
        switch (xo5) {
        case 18: ctx.fpr[rD] = a / b; break;                           
        case 20: ctx.fpr[rD] = a - b; break;                           
        case 21: ctx.fpr[rD] = a + b; break;                           
        case 22: ctx.fpr[rD] = std::sqrt(b); break;                    
        case 25: ctx.fpr[rD] = a * c; break;                           
        case 26: ctx.fpr[rD] = fp_frsqrte(b); break;                   // frsqrte (table estimate)
        case 28: ctx.fpr[rD] = fp_fma(a, c, -b); break;                // fmsub
        case 29: ctx.fpr[rD] = fp_fma(a, c, b); break;                 // fmadd
        case 30: ctx.fpr[rD] = fp_neg_unless_nan(fp_fma(a, c, -b)); break; // fnmsub
        case 31: ctx.fpr[rD] = fp_neg_unless_nan(fp_fma(a, c, b)); break;  // fnmadd
        case 23: ctx.fpr[rD] = (a >= -0.0) ? c : b; break;             
        default: aform = false; break;
        }
        if (aform) break;
        switch (xo10) {
        case 0: case 32: { 
            uint32_t crf = rD >> 2;
            const bool lt = a < b, gt = a > b, eq = a == b; // all false when unordered
            ctx.cr[crf].lt = lt;
            ctx.cr[crf].gt = gt;
            ctx.cr[crf].eq = eq;
            ctx.cr[crf].so = !(lt || gt || eq);
            break;
        }
        case 12: ctx.fpr[rD] = ctx.ps1[rD] = fp_round_single(b); break; // frsp: Fill
        case 14: case 15: { // fctiw / fctiwz (Dolphin ConvertToInteger)
            const uint64_t bits = fp_fctiw_bits(b, xo10 == 15);
            std::memcpy(&ctx.fpr[rD], &bits, 8);
            break;
        }
        case 40:  ctx.fpr[rD] = -b; break;                             
        case 72:  ctx.fpr[rD] = b; break;                              
        case 136: ctx.fpr[rD] = -std::fabs(b); break;                  
        case 264: ctx.fpr[rD] = std::fabs(b); break;                   
        case 38:  ctx.fpscr |= 0x80000000u >> rD; break;               // mtfsb1 crbD
        case 70:  ctx.fpscr &= ~(0x80000000u >> rD); break;            // mtfsb0 crbD
        case 134: { // mtfsfi crfD, IMM
            const uint32_t shift = 4 * (7 - (rD >> 2));
            const uint32_t imm = (insn >> 12) & 0xF;
            ctx.fpscr = (ctx.fpscr & ~(0xFu << shift)) | (imm << shift);
            break;
        }
        case 711: { // mtfsf FM, frB
            const uint32_t fm = (insn >> 17) & 0xFF;
            uint32_t m = 0;
            for (uint32_t i = 0; i < 8; ++i)
                if (fm & (1u << i)) m |= 0xFu << (i * 4);
            uint64_t bits;
            std::memcpy(&bits, &ctx.fpr[rB], 8);
            ctx.fpscr = (ctx.fpscr & ~m) | (static_cast<uint32_t>(bits) & m);
            break;
        }
        case 64: { // mcrfs crfD, crfS: copy the field, clear the exception bits read (FX | any X)
            const uint32_t shift = 4 * (7 - (rA >> 2));
            const uint32_t f = (ctx.fpscr >> shift) & 0xF;
            ctx.fpscr &= ~((0xFu << shift) & 0x9FF80700u);
            const uint32_t crf = rD >> 2;
            ctx.cr[crf].lt = (f & 8) != 0;
            ctx.cr[crf].gt = (f & 4) != 0;
            ctx.cr[crf].eq = (f & 2) != 0;
            ctx.cr[crf].so = (f & 1) != 0;
            break;
        }
        case 583: { uint64_t z = 0xFFF8000000000000ull | ctx.fpscr; std::memcpy(&ctx.fpr[rD], &z, 8); break; } // mffs
        default:
            unsupported_instruction(ctx, insn, "op63", xo10);
        }
        break;
    }
    case 4: { 
        uint32_t xo5 = (insn >> 1) & 0x1F;
        uint32_t xo6 = (insn >> 1) & 0x3F;
        uint32_t xo10 = (insn >> 1) & 0x3FF;
        uint32_t fC = (insn >> 6) & 0x1F;
        
        if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) {
            uint32_t W = (insn >> 10) & 1;
            uint32_t I = (insn >> 7) & 7;
            uint32_t ea = (xo6 >= 38 ? ctx.gpr[rA] : (rA ? ctx.gpr[rA] : 0)) + ctx.gpr[rB];
            if (xo6 == 6 || xo6 == 38) ctx.psq_load(rD, ea, W, I);
            else                       ctx.psq_store(rD, ea, W, I);
            if (xo6 >= 38) ctx.gpr[rA] = ea;
            break;
        }
        bool aform = true;
        switch (xo5) {
        case 10: ctx.ps_sum0(rD, rA, fC, rB); break;
        case 11: ctx.ps_sum1(rD, rA, fC, rB); break;
        case 12: ctx.ps_muls0(rD, rA, fC); break;
        case 13: ctx.ps_muls1(rD, rA, fC); break;
        case 14: ctx.ps_madds0(rD, rA, fC, rB); break;
        case 15: ctx.ps_madds1(rD, rA, fC, rB); break;
        case 18: ctx.ps_div(rD, rA, rB); break;
        case 20: ctx.ps_sub(rD, rA, rB); break;
        case 21: ctx.ps_add(rD, rA, rB); break;
        case 23: ctx.ps_sel(rD, rA, fC, rB); break;
        case 24: ctx.ps_res(rD, rB); break;     // ps_res (table estimate)
        case 25: ctx.ps_mul(rD, rA, fC); break;
        case 26: ctx.ps_rsqrte(rD, rB); break;  // ps_rsqrte (table estimate)
        case 28: ctx.ps_msub(rD, rA, fC, rB); break;
        case 29: ctx.ps_madd(rD, rA, fC, rB); break;
        case 30: ctx.ps_nmsub(rD, rA, fC, rB); break;
        case 31: ctx.ps_nmadd(rD, rA, fC, rB); break;
        default: aform = false; break;
        }
        if (aform) break;
        switch (xo10) {
        case 0:   ctx.ps_cmpu0(rD >> 2, rA, rB); break;
        case 32:  ctx.ps_cmpo0(rD >> 2, rA, rB); break;
        case 64:  ctx.ps_cmpu1(rD >> 2, rA, rB); break;
        case 96:  ctx.ps_cmpo1(rD >> 2, rA, rB); break;
        case 40:  ctx.ps_neg(rD, rB); break;
        case 72:  ctx.ps_mr(rD, rB); break;
        case 136: ctx.ps_nabs(rD, rB); break;
        case 264: ctx.ps_abs(rD, rB); break;
        case 528: ctx.ps_merge00(rD, rA, rB); break;
        case 560: ctx.ps_merge01(rD, rA, rB); break;
        case 592: ctx.ps_merge10(rD, rA, rB); break;
        case 624: ctx.ps_merge11(rD, rA, rB); break;
        case 1014: { // dcbz_l zeros the containing 32-byte cache line.
            ctx.zero_cache_line(ea_ra0(0) + ctx.gpr[rB]);
            break;
        }
        default:
            unsupported_instruction(ctx, insn, "op4", xo10);
        }
        break;
    }
    default:
        unsupported_instruction(ctx, insn, "primary opcode", op);
    }

    ctx.pc = pc + 4;
    return true;
}





static bool try_native_helper(CPUContext& ctx) {
    static bool enabled = std::getenv("NWII_NO_FASTMEM") == nullptr;
    if (!enabled)
        return false;
    uint32_t pc = ctx.pc;

    
    
    if (ctx.mmu.read32(pc)      == 0x7C041840 &&
        ctx.mmu.read32(pc + 4)  == 0x41800028 &&
        ctx.mmu.read32(pc + 8)  == 0x3884FFFF &&
        ctx.mmu.read32(pc + 12) == 0x38C3FFFF &&
        ctx.mmu.read32(pc + 16) == 0x38A50001) {
        uint32_t dst = ctx.gpr[3], src = ctx.gpr[4], n = ctx.gpr[5];
        if (n > 0x2000000u) {
            static int warned = 0;
            if (warned++ < 8)
                std::cout << "[Interp] memcpy huge n=0x" << std::hex << n
                          << " dst=0x" << dst << " src=0x" << src << " lr=0x"
                          << ctx.lr << std::dec << " -- suspicious, skipping\n";
            ctx.pc = ctx.lr;
            return true;
        }
        if (src >= dst) {
            for (uint32_t i = 0; i < n; ++i)
                ctx.mmu.write8(dst + i, ctx.mmu.read8(src + i));
        } else {
            for (uint32_t i = n; i-- > 0;)
                ctx.mmu.write8(dst + i, ctx.mmu.read8(src + i));
        }
        ctx.pc = ctx.lr; 
        return true;
    }

    

    if (ctx.mmu.read32(pc)      == 0x28050020 &&
        ctx.mmu.read32(pc + 4)  == 0x5484063E &&
        ctx.mmu.read32(pc + 8)  == 0x38C3FFFF &&
        ctx.mmu.read32(pc + 12) == 0x7C872378) {
        uint32_t dst = ctx.gpr[3], val = ctx.gpr[4] & 0xFF, n = ctx.gpr[5];
        if (n <= 0x2000000u)
            for (uint32_t i = 0; i < n; ++i)
                ctx.mmu.write8(dst + i, (uint8_t)val);
        ctx.pc = ctx.lr;
        return true;
    }
    return false;
}



namespace relaot { bool enter(CPUContext& ctx); } // RELAOT track: see core/rel_aot.cpp
void interpret_step(CPUContext& ctx) {
    prof::bump(prof::kInterpEntries); // PERF2 (NWII_PROFILE only)
    if (nwii::runtime::relaot::enter(ctx)) return; // RELAOT (M2.8, RELAOT track): statically recompiled REL module (core/rel_aot.cpp); NWII_REL_AOT=0 disables
    struct DepthGuard { DepthGuard() { g_interp_depth = g_interp_depth + 1; } ~DepthGuard() { g_interp_depth = g_interp_depth - 1; } } depth_guard; // FREEZE8

    

    
    static const bool relcap = std::getenv("NWII_RELCAP") != nullptr;
    if (relcap) {
        static bool done = false;
        if (!done && ctx.pc >= 0x8051d000 && ctx.pc < 0x80570000) {
            uint16_t hi = ctx.mmu.read16(0x8051dff2);
            uint16_t lo = ctx.mmu.read16(0x8051dff6);
            uint32_t val = ((uint32_t)hi << 16) | lo;
            uint32_t bss = val - 0x3c;
            std::cout << "[RELCAP] module_base=0x8051de60 first-exec-pc=0x"
                      << std::hex << ctx.pc << " hi=0x" << hi << " lo=0x" << lo
                      << " => bss_base=0x" << bss << std::dec << "\n";
            // Ground-truth the ctor-table pointer the game's OSLink produced:
            // prolog lis @0x8051df54, addi @0x8051df58. Compute both the
            // sign-extended (addi) and zero-extended (ori) results so we can
            // see which layout sec2 actually got.
            uint16_t plis = ctx.mmu.read16(0x8051df56);
            uint16_t padd = ctx.mmu.read16(0x8051df5a);
            uint32_t r31_addi = ((uint32_t)plis << 16) + (int32_t)(int16_t)padd;
            uint32_t r31_ori = ((uint32_t)plis << 16) | padd;
            std::cout << "[RELCAP] prolog lis=0x" << std::hex << plis << " low=0x"
                      << padd << " => ctor_table addi=0x" << r31_addi
                      << " ori=0x" << r31_ori << std::dec << "\n";
            done = true;
        }
    }
    static int announce_budget = 200;
    if (announce_budget > 0) {
        announce_budget--;
        std::cout << "[Interp] Interpreting at 0x" << std::hex << ctx.pc
                  << " lr=0x" << ctx.lr << " r1=0x" << ctx.gpr[1]
                  << " r3=0x" << ctx.gpr[3] << " r4=0x" << ctx.gpr[4]
                  << " r5=0x" << ctx.gpr[5] << std::dec << "\n";
    }

    

    if (ctx.pc >= 0x80800000) {
        static int wild_budget = 20;
        if (wild_budget > 0) {
            wild_budget--;
            std::cout << "[Interp] WILD JUMP to 0x" << std::hex << ctx.pc
                      << " lr=0x" << ctx.lr << " ctr=0x" << ctx.ctr
                      << " r1=0x" << ctx.gpr[1] << " r3=0x" << ctx.gpr[3]
                      << std::dec << "\n";
        }
    }

    

    if (try_native_helper(ctx) && in_recompiled_code(ctx.pc))
        return;

    static const bool pc_trace = ss_pc_trace_enabled(); // NWII_TRACEPC, off by default
    int consecutive_nops = 0;
    uint32_t taken_branches = 0;
    for (uint64_t i = 0; i < 100000000ULL; ++i) {
        if (ctx.is_running == 0) return;
        uint32_t before = ctx.pc;
        uint32_t insn = ctx.mmu.read32(before);
        
        if (insn == 0) {
            if (++consecutive_nops > 64) {
                std::cout << "[Interp] Aborting: " << std::dec << consecutive_nops
                          << " zero-opcodes from 0x" << std::hex << before
                          << " (walked into data). lr=0x" << ctx.lr << std::dec
                          << "\n";
                ctx.is_running = false;
                return;
            }
        } else {
            consecutive_nops = 0;
        }
        ++ctx.inst_count;
        prof::bump(prof::kInterpInsns);
        uint32_t ee_was = ctx.msr & 0x8000;
        if (pc_trace) ss_pc_trace(ctx, before);
        interpret_one(ctx);

        
        if (!ee_was && (ctx.msr & 0x8000)) {
            if (process_pending_callbacks(ctx) && in_recompiled_code(ctx.pc))
                return;
        }
        if (ctx.pc != before + 4) {

            if (try_native_helper(ctx)) {
                if (in_recompiled_code(ctx.pc))
                    return;
                continue;
            }
            if ((ctx.pc & 0x3FFFFFFF) < 0x100) {
                std::cout << "[Interp] Jump to low address 0x" << std::hex
                          << ctx.pc << " from 0x" << before << " insn=0x"
                          << ctx.mmu.read32(before) << " lr=0x" << ctx.lr
                          << " ctr=0x" << ctx.ctr << std::dec << "\n";
            }

            
            if (in_recompiled_code(ctx.pc))
                return;

            // FREEZE8: poll every 4096 *taken branches*. The old test `(i & 0xFFF) == 0` only polled when the
            // instruction with index i = k*4096 happened to be a taken branch, so a REL loop whose length is a
            // power of two (spin/wait loops) could alias and never take an interrupt (only the 100M budget below
            // ended it, ~1 s per poll = an apparent freeze).
            if ((++taken_branches & 0xFFF) == 0 && process_pending_callbacks(ctx))
                return;
        }
    }
    std::cout << "[Interp] Budget exceeded at 0x" << std::hex << ctx.pc
              << std::dec << "\n";
    ++g_interp_budget_exceeded; // FREEZE8 watchdog reports it
}

void micro_interpret(CPUContext& ctx, uint32_t opcode, uint32_t pc) {
    ctx.pc = pc;
    interpret_one(ctx);
}

}
}
