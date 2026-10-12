// FP track diagnostic: NWII_FPTRAP=<max events> (off by default).
//
// Finds host floating-point "invalid operation" events (the operations that
// create a NaN from non-NaN inputs: 0*inf, inf-inf, 0/0, sqrt(<0), and also
// out-of-range float->int conversions and ordered compares of NaN) on the CPU
// thread. CPUContext::fp_unavailable() calls fptrap_arm() on the CPU thread;
// it unmasks MXCSR.IM there. A vectored handler logs the event (host symbol +
// offset, instruction bytes, xmm0-xmm7, guest pc/lr/frame), masks IM for one
// instruction (EFLAGS.TF single step, so the instruction completes with the
// default NaN result exactly as it would without the trap) and unmasks again
// on the single-step event. After <max events> events (default 400) the trap
// stays masked. Only the first occurrence of each host RIP is printed in full;
// the summary at the end of the budget lists every RIP with its count. The
// saved MXCSR in ctx.exception_jmp_buf is kept in step (longjmp restores it).
// At arming it also prints MXCSR, so FTZ/DAZ (denormal flushing, which Gekko
// with FPSCR[NI]=0 does not do) would be visible.
//
// NWII_NANSTORE=1 reports float stores (stfs/stfd/psq_st float) of NaN values,
// first per guest function (ctx.pc = function entry or last return point in
// generated code), with the guest r1 back-chain.
// NWII_FPTRAP_FRAMES=lo-hi re-reports first occurrences inside those game frames
// (SS observation frame counter), so a NaN chain that starts later is not
// hidden by the global first-occurrence filter.
//
// This changes nothing unless NWII_FPTRAP / NWII_NANSTORE is set (the hooks in
// cpu_context.h stay null otherwise).

#include "runtime/cpu_context.h"
#include "../input/game_observation.h"

#if defined(_WIN32) && defined(_M_X64)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <xmmintrin.h>
#include <setjmp.h>
#include <cstdio>
#include <map>
#include <string>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace nwii::runtime {

extern CPUContext *g_ctx_ptr;

#if defined(_WIN32) && defined(_M_X64)
namespace {

constexpr DWORD kMxcsrIM = 0x80;     // invalid-operation mask
constexpr DWORD kMxcsrFlags = 0x3F;  // sticky exception flags
constexpr DWORD kEflagsTF = 0x100;

int g_budget = 0;            // remaining events (CPU thread only)
DWORD g_cpu_thread = 0;      // thread that armed the trap
bool g_stepping = false;     // single step in progress after an event
bool g_sym_ready = false;
bool g_summary_done = false;
uint64_t g_events = 0;
std::map<uint64_t, uint64_t> *g_rips = nullptr;

// NWII_FPTRAP_FRAMES=lo-hi: inside these game frames (KPAD frame counter of the
// SS observation), every first occurrence per RIP / per guest function is
// reported again, so a NaN chain that starts later is not hidden by the
// global first-occurrence filter.
struct Window { uint32_t lo = 1, hi = 0; };
const Window g_window = [] {
  Window w;
  if (const char *e = std::getenv("NWII_FPTRAP_FRAMES")) {
    unsigned a = 0, b = 0;
    if (std::sscanf(e, "%u-%u", &a, &b) == 2) { w.lo = a; w.hi = b; }
  }
  return w;
}();
uint32_t game_frame() { return nwii::runtime::input::g_game_observation.frames(); }
bool in_window(uint32_t f) { return f >= g_window.lo && f <= g_window.hi; }

std::string symbolize(uint64_t rip) {
  HANDLE proc = GetCurrentProcess();
  if (!g_sym_ready) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path);
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir.resize(slash);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS |
                  SYMOPT_LOAD_LINES);
    g_sym_ready = SymInitialize(proc, dir.c_str(), TRUE) != FALSE;
  }
  char buf[sizeof(SYMBOL_INFO) + 512] = {};
  auto *si = reinterpret_cast<SYMBOL_INFO *>(buf);
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = 500;
  DWORD64 disp = 0;
  char out[700];
  if (g_sym_ready && SymFromAddr(proc, rip, &disp, si)) {
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD ldisp = 0;
    if (SymGetLineFromAddr64(proc, rip, &ldisp, &line))
      std::snprintf(out, sizeof out, "%s+0x%llx (%s:%lu)", si->Name, (unsigned long long)disp,
                    line.FileName, line.LineNumber);
    else
      std::snprintf(out, sizeof out, "%s+0x%llx", si->Name, (unsigned long long)disp);
  } else {
    std::snprintf(out, sizeof out, "?0x%llx", (unsigned long long)rip);
  }
  return out;
}

// longjmp (ios.cpp exception paths) restores MXCSR from the jump buffer on
// x64 Windows; keep the saved MXCSR in step with the trap state.
void set_jmpbuf_mask(bool masked) {
  CPUContext *ctx = g_ctx_ptr;
  if (!ctx) return;
  auto *jb = reinterpret_cast<_JUMP_BUFFER *>(&ctx->exception_jmp_buf);
  jb->MxCsr = masked ? (jb->MxCsr | kMxcsrIM) : (jb->MxCsr & ~(kMxcsrIM | kMxcsrFlags));
}

void print_summary() {
  if (g_summary_done || !g_rips) return;
  g_summary_done = true;
  std::printf("[FPTRAP] budget exhausted after %llu events; per-RIP counts:\n",
              (unsigned long long)g_events);
  for (const auto &kv : *g_rips)
    std::printf("[FPTRAP]   %s x%llu\n", symbolize(kv.first).c_str(), (unsigned long long)kv.second);
  std::fflush(stdout);
}

void report(EXCEPTION_POINTERS *ep) {
  CONTEXT *c = ep->ContextRecord;
  const uint64_t rip = c->Rip;
  ++g_events;
  if (!g_rips) g_rips = new std::map<uint64_t, uint64_t>();
  bool first = ((*g_rips)[rip]++ == 0);
  const uint32_t gframe = game_frame();
  if (in_window(gframe)) {
    static std::map<uint64_t, uint64_t> *win = new std::map<uint64_t, uint64_t>();
    if ((*win)[rip]++ == 0) first = true;
  }
  CPUContext *ctx = g_ctx_ptr;
  static const uint64_t t0 = GetTickCount64();
  const double secs = (GetTickCount64() - t0) / 1000.0;
  if (first) {
    const auto *b = reinterpret_cast<const uint8_t *>(rip);
    char bytes[64];
    std::snprintf(bytes, sizeof bytes, "%02X %02X %02X %02X %02X %02X %02X %02X", b[0], b[1], b[2], b[3],
                  b[4], b[5], b[6], b[7]);
    std::printf("[FPTRAP] #%llu frame=%u code=0x%08lX t=%.1f inst=%llu guest pc=0x%08X lr=0x%08X at %s bytes=[%s]\n",
                (unsigned long long)g_events, gframe, ep->ExceptionRecord->ExceptionCode, secs,
                ctx ? (unsigned long long)ctx->inst_count : 0ull, ctx ? ctx->pc : 0u, ctx ? ctx->lr : 0u,
                symbolize(rip).c_str(), bytes);
    const M128A *x = &c->Xmm0;
    std::printf("[FPTRAP]   xmm0-7 lo(double|float):");
    for (int i = 0; i < 8; ++i) {
      double d;
      float f;
      std::memcpy(&d, &x[i], 8);
      std::memcpy(&f, &x[i], 4);
      std::printf(" %d:%g|%g", i, d, f);
    }
    std::printf("\n");
    if (ctx) {
      std::printf("[FPTRAP]   guest fpr ps0/ps1:");
      for (int i = 0; i < 32; ++i) std::printf(" f%d=%g/%g", i, ctx->fpr[i], ctx->ps1[i]);
      std::printf("\n[FPTRAP]   guest r3=%08X r4=%08X r5=%08X r31=%08X backtrace:", ctx->gpr[3], ctx->gpr[4],
                  ctx->gpr[5], ctx->gpr[31]);
      // Back chain: [sp] = caller's sp, [caller sp + 4] = saved LR.
      uint32_t sp = ctx->gpr[1];
      for (int i = 0; i < 12; ++i) {
        if (sp < 0x80000000u || sp >= 0x94000000u || (sp & 3)) break;
        const uint32_t next = ctx->mmu.read32(sp);
        if (next < 0x80000000u || next >= 0x94000000u || next <= sp) break;
        std::printf(" %08X", ctx->mmu.read32(next + 4));
        sp = next;
      }
      std::printf("\n");
    }
    std::fflush(stdout);
  }
}

LONG CALLBACK handler(EXCEPTION_POINTERS *ep) {
  if (GetCurrentThreadId() != g_cpu_thread) return EXCEPTION_CONTINUE_SEARCH;
  // The handler runs with the faulting thread's MXCSR: mask everything here so
  // the report itself (printf of NaN, dbghelp) cannot trap. The resumed code
  // gets ContextRecord->MxCsr.
  _mm_setcsr(_mm_getcsr() | 0x1F80u);
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  CONTEXT *c = ep->ContextRecord;
  if (code == EXCEPTION_SINGLE_STEP && g_stepping) {
    g_stepping = false;
    c->EFlags &= ~kEflagsTF;
    if (g_budget > 0) {
      c->MxCsr = (c->MxCsr & ~(kMxcsrIM | kMxcsrFlags));
      c->FltSave.MxCsr = c->MxCsr;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == STATUS_FLOAT_INVALID_OPERATION || code == STATUS_FLOAT_MULTIPLE_TRAPS ||
      code == STATUS_FLOAT_MULTIPLE_FAULTS) {
    report(ep);
    --g_budget;
    c->MxCsr = (c->MxCsr | kMxcsrIM) & ~kMxcsrFlags;
    c->FltSave.MxCsr = c->MxCsr;
    if (g_budget > 0) {
      g_stepping = true;
      c->EFlags |= kEflagsTF;
    } else {
      set_jmpbuf_mask(true);
      print_summary();
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void fptrap_arm() {
  static bool installed = false;
  if (!installed) {
    installed = true;
    g_budget = g_env_fptrap > 1 ? g_env_fptrap : 400;
    g_cpu_thread = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, handler);
    const unsigned csr = _mm_getcsr();
    std::printf("[FPTRAP] armed on thread %lu, budget %d, MXCSR=0x%04X (FTZ=%u DAZ=%u RC=%u), FMA3=%d\n",
                g_cpu_thread, g_budget, csr, (csr >> 15) & 1, (csr >> 6) & 1, (csr >> 13) & 3,
                g_cpu_has_fma3 ? 1 : 0);
    std::fflush(stdout);
  }
  // Re-arm (longjmp restores MXCSR from its jump buffer on x64 Windows).
  if (g_budget > 0 && GetCurrentThreadId() == g_cpu_thread && !g_stepping) {
    _mm_setcsr(_mm_getcsr() & ~(kMxcsrIM | kMxcsrFlags));
    set_jmpbuf_mask(false);
  }
}
// NWII_NANSTORE: first NaN float store per guest function (ctx.pc = function
// entry in generated code), with guest backtrace. Up to 400 functions.
void nanstore_hit(uint32_t addr, uint32_t bits) {
  static std::map<uint32_t, uint64_t> *seen = new std::map<uint32_t, uint64_t>();
  static uint64_t total = 0;
  CPUContext *ctx = g_ctx_ptr;
  if (!ctx) return;
  ++total;
  const uint32_t key = ctx->pc;
  bool fresh = (*seen)[key]++ == 0 && seen->size() <= 400;
  const uint32_t gframe = game_frame();
  if (in_window(gframe)) {
    static std::map<uint32_t, uint64_t> *win = new std::map<uint32_t, uint64_t>();
    if ((*win)[key]++ == 0 && win->size() <= 400) fresh = true;
  }
  if (!fresh) return;
  static int fpr_dumps = 0;
  if (in_window(gframe) && fpr_dumps < 3) {
    ++fpr_dumps;
    std::printf("[NANSTORE]   fpr ps0/ps1:");
    for (int i = 0; i < 32; ++i) std::printf(" f%d=%g/%g", i, ctx->fpr[i], ctx->ps1[i]);
    std::printf(" r1=%08X\n", ctx->gpr[1]);
  }
  static const uint64_t t0 = GetTickCount64();
  std::printf("[NANSTORE] frame=%u t=%.1f n=%llu inst=%llu addr=%08X bits=%08X pc=%08X lr=%08X bt:",
              gframe, (GetTickCount64() - t0) / 1000.0, (unsigned long long)total,
              (unsigned long long)ctx->inst_count, addr, bits, ctx->pc, ctx->lr);
  uint32_t sp = ctx->gpr[1];
  for (int i = 0; i < 8; ++i) {
    if (sp < 0x80000000u || sp >= 0x94000000u || (sp & 3)) break;
    const uint32_t next = ctx->mmu.read32(sp);
    if (next < 0x80000000u || next >= 0x94000000u || next <= sp) break;
    std::printf(" %08X", ctx->mmu.read32(next + 4));
    sp = next;
  }
  std::printf("\n");
  std::fflush(stdout);
}
// Install the hooks cpu_context.h calls (only when the diagnostics are enabled).
static const bool g_hooks_installed = [] {
  if (g_env_fptrap) g_fptrap_arm_hook = &fptrap_arm;
  if (g_env_nanstore) g_nanstore_hook = &nanstore_hit;
  return true;
}();
#endif

} // namespace nwii::runtime

// Nothing references this object file (the hooks are installed by the static
// initializer above), so the static library member would be dropped by the
// linker; CMakeLists.txt forces it in with /INCLUDE of this symbol.
extern "C" int nwii_fp_trap_anchor = 1;
