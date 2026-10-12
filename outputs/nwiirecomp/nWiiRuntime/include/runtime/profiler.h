#pragma once
// PERF2 sampling profiler (NWII_PROFILE=<hz>; off by default).
//
// A host thread suspends the registered threads at <hz> (SuspendThread +
// GetThreadContext RIP + ResumeThread) and also records the guest PC/LR the
// CPU thread last published (ctx.pc is stored at every recompiled call and
// back-edge, and per instruction by the REL interpreter). At exit the host RIPs
// are symbolised with DbgHelp (needs the PDB the /DEBUG link writes), DOL guest
// PCs are mapped through symbols.csv, interpreted PCs through the live OS module
// list (0x800030C8) to (REL module id, section, offset) and optionally to names,
// and a report is written next to the log.
//
// Nothing here touches guest state. The instrumentation hooks below are a
// single predictable branch on a startup-cached flag when profiling is off.
#include <atomic>
#include <cstdint>

namespace nwii::runtime::prof {

// Set once by init() before any other runtime thread starts; plain reads after.
extern bool g_on;

// What the host (render) thread is doing; written once or twice per host-loop
// iteration, read by the sampler.
enum HostPhase : uint8_t {
  kHostOther = 0,
  kHostInput,
  kHostGx,      // ProcessGXFifo: FIFO parse + decode + GL render
  kHostPresent, // blit, stats, screenshots
  kHostSwap,    // SDL_GL_SwapWindow (vsync wait)
  kHostAudio,
  kHostSleep,   // headless sleep
  kHostPhaseCount
};
extern std::atomic<uint8_t> g_host_phase;
inline void host_phase(HostPhase p) {
  if (g_on) g_host_phase.store(p, std::memory_order_relaxed);
}

// Event counters. Each has exactly one writer thread, so the increment is a
// plain load+store (no lock prefix); the sampler reads them racily (diagnostic).
enum Counter : int {
  kHostIters = 0, // host-loop iterations
  kGxUs,          // host us in ProcessGXFifo
  kSwapUs,        // host us in SDL_GL_SwapWindow
  kPresentUs,     // host us in blit/stat/screenshot
  kViHost,        // VI interrupts raised from the host-loop vblank flag
  kViVirtual,     // VI interrupts raised by the virtual-time scheduler event
  kExtIrq,        // external interrupts dispatched to __OSDispatchInterrupt
  kDecIrq,        // decrementer exceptions delivered
  kInterpEntries, // interpret_step() entries (dispatcher -> REL interpreter)
  kInterpInsns,   // instructions executed by the REL interpreter
  kIdleSkips,     // idle-loop fast-forwards (PERF2 idle skip)
  kIdleSkipTicks, // guest instructions (inst_count) skipped by them
  kDispatchShortcut, // dispatcher hops to interpret_step without the bounds scan
  kGxIdleParse,   // GX FIFO parses done on the CPU thread while the guest idled
  kCounterCount
};
extern std::atomic<uint64_t> g_counters[kCounterCount];
inline void bump(Counter c, uint64_t n = 1) {
  if (!g_on) return;
  std::atomic<uint64_t> &a = g_counters[c];
  a.store(a.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}
// External interrupt dispatches by PI cause bit (PI_INTSR bit index).
void count_irq_causes(uint32_t bits);

// How the sampler reads guest-side state. Pointers alias live CPU-thread
// fields; they are read only while that thread is suspended (pc/lr) or racily
// (counters), never written.
struct GuestProbe {
  const volatile uint32_t *pc = nullptr;
  const volatile uint32_t *lr = nullptr;
  const volatile uint64_t *inst = nullptr;   // ctx.inst_count
  const volatile uint64_t *frames = nullptr; // g_stat_frames (frames presented by the host loop)
  const volatile uint64_t *xfb_copies = nullptr; // guest EFB->XFB copies (game frames)
  uint64_t (*polls)() = nullptr;             // process_pending_callbacks calls
  bool (*is_native)(uint32_t pc) = nullptr;  // PC inside recompiled DOL text
  // Host pointer to `size` bytes of guest RAM at `addr`, or null.
  const uint8_t *(*guest_ptr)(uint32_t addr, uint32_t size) = nullptr;
};

// Reads NWII_PROFILE (sampling rate in Hz; unset or 0 = off) and the
// NWII_PROFILE_* options, and starts the sampler. Returns g_on.
// Call once at start-up, before the CPU thread starts.
bool init(const GuestProbe &probe);
// Host code extent [lo, lo+len) of the function containing `fn` (x64 unwind
// table). Used to recognise calls made from the generated dispatcher.
bool host_function_extent(const void *fn, uintptr_t &lo, uintptr_t &len);
// slot 0 = CPU (guest) thread, 1 = host/render thread.
void register_current_thread(int slot);
// Stops the sampler and writes the report (no-op when off). Idempotent.
void shutdown_and_report();

} // namespace nwii::runtime::prof
