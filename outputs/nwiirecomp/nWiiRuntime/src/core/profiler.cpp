// PERF2 sampling profiler. See include/runtime/profiler.h.
//
// Options (read once by init()):
//   NWII_PROFILE=<hz>          sampling rate per thread (e.g. 1000); unset/0 = off
//   NWII_PROFILE_OUT=<file>    report path (default: next to the screenshots,
//                              <dir>/<stamp>-profile.txt, else nwii-profile.txt)
//   NWII_PROFILE_SPLIT=a,b,..  phase boundaries in seconds since init(), or
//                              "stage": a new phase whenever the guest's current
//                              stage (+room) changes (SOUE01 addresses below)
//   NWII_PROFILE_STAGE=<hex>   guest address of the stage name (default SOUE01
//                              dScGame_c::currentSpawnInfo 805B3860; room at +0x22)
//   NWII_PROFILE_LOGIC=<hex>   guest u32 logic-frame counter (default SOUE01
//                              dScGame_c::sUpdateFrameCount 80575798); 0 = none
//   NWII_PROFILE_SYMBOLS=<csv> DOL symbols (default ../pc-profile/symbols.csv)
//   NWII_PROFILE_RELSYMS=<txt> REL symbols "M id name" / "F id sec off size name"
//                              (default ../../work/perf2/rel_symbols.txt)
//   NWII_PROFILE_TOP=<n>       rows per table (default 25)
//   NWII_PROFILE_IDLE=lo-hi    guest idle-loop PC range (hex, default SOUE01
//                              SelectThread idle loop 803ACB58-803ACB80)
//   NWII_PROFILE_PERIOD=<s>    [PROF] line period (default 5)
#include "runtime/profiler.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace nwii::runtime::prof {

bool g_on = false;
std::atomic<uint8_t> g_host_phase{0};
std::atomic<uint64_t> g_counters[kCounterCount];
std::atomic<uint64_t> g_irq_bits[32];

void count_irq_causes(uint32_t bits) {
  if (!g_on) return;
  for (int i = 0; i < 32; ++i)
    if (bits & (1u << i)) {
      std::atomic<uint64_t> &a = g_irq_bits[i];
      a.store(a.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
}

namespace {

using clk = std::chrono::steady_clock;

const char *const kPhaseNames[kHostPhaseCount] = {"other", "input",  "gx-process", "present",
                                                  "swap/vsync", "audio", "sleep"};
const char *const kCounterNames[kCounterCount] = {
    "host_iters", "gx_us", "swap_us", "present_us", "vi_host", "vi_virtual",
    "ext_irq", "dec_irq", "interp_entries", "interp_insns", "idle_skips", "idle_skip_insns",
    "dispatch_shortcut", "gx_idle_parse"};

enum GuestCat : uint8_t { kGcNative = 0, kGcIdle = 1, kGcOther = 2, kGcNone = 3, kGcCount = 4 };
const char *const kGcNames[kGcCount] = {"native-dol", "idle-loop", "rel/interp", "none"};

struct Options {
  unsigned hz = 0;
  std::string out;
  std::vector<double> split;
  std::string symbols = "../pc-profile/symbols.csv";
  std::string relsyms = "../../work/perf2/rel_symbols.txt";
  unsigned top = 25;
  uint32_t idle_lo = 0x803ACB58, idle_hi = 0x803ACB80;
  // The idle loop calls these two; a sample there with LR back in the loop is idle.
  uint32_t idle_call_a = 0x803A8D20, idle_call_b = 0x803A8D00;
  double period = 5.0;
  bool split_stage = false;
  uint32_t stage_addr = 0x805B3860, logic_addr = 0x80575798;
};

struct Snapshot {
  double t = 0;
  uint64_t inst = 0, polls = 0, frames = 0, xfb = 0, logic = 0;
  uint64_t c[kCounterCount] = {};
  uint64_t irq[32] = {};
};

struct PhaseData {
  std::string name;
  Snapshot s0, s1;
  bool started = false;
  uint64_t cpu_samples = 0, host_samples = 0;
  uint64_t gcat[kGcCount] = {};
  uint64_t host_phase[kHostPhaseCount] = {};
  std::unordered_map<uint64_t, uint32_t> cpu_rip;  // rip | gcat << 56
  std::unordered_map<uint64_t, uint32_t> host_rip; // rip | phase << 56
  std::unordered_map<uint32_t, uint32_t> dol_pc;   // non-idle native guest pc
  std::unordered_map<uint64_t, uint32_t> rel;      // id<<40 | sec<<32 | off, or raw pc (id 0xFFFFF)
};

struct ModRange {
  uint32_t lo, hi, id, sec;
};

struct State {
  Options opt;
  GuestProbe probe;
  clk::time_point t0;
#ifdef _WIN32
  HANDLE threads[2] = {nullptr, nullptr};
#endif
  std::atomic<bool> stop{false};
  std::thread sampler;
  std::vector<PhaseData> phases;
  std::vector<ModRange> mods;
  std::vector<std::string> series; // periodic lines
  bool reported = false;
  uint64_t suspend_failures = 0;
  double sampler_busy_s = 0;
};

State &S() {
  static State s;
  return s;
}

uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

double now_s() { return std::chrono::duration<double>(clk::now() - S().t0).count(); }

Snapshot take_snapshot() {
  State &s = S();
  Snapshot sn;
  sn.t = now_s();
  if (s.probe.inst) sn.inst = *s.probe.inst;
  if (s.probe.polls) sn.polls = s.probe.polls();
  if (s.probe.frames) sn.frames = *s.probe.frames;
  if (s.probe.xfb_copies) sn.xfb = *s.probe.xfb_copies;
  if (s.opt.logic_addr && s.probe.guest_ptr)
    if (const uint8_t *p = s.probe.guest_ptr(s.opt.logic_addr, 4)) sn.logic = be32(p);
  for (int i = 0; i < kCounterCount; ++i) sn.c[i] = g_counters[i].load(std::memory_order_relaxed);
  for (int i = 0; i < 32; ++i) sn.irq[i] = g_irq_bits[i].load(std::memory_order_relaxed);
  return sn;
}

// Current stage name + room ("F405/0"), or "" when unreadable.
std::string read_stage() {
  State &st = S();
  if (!st.opt.stage_addr || !st.probe.guest_ptr) return "";
  const uint8_t *p = st.probe.guest_ptr(st.opt.stage_addr, 0x24);
  if (!p) return "";
  std::string n;
  for (int i = 0; i < 32 && p[i]; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) return "?";
    n += (char)p[i];
  }
  return (n.empty() ? std::string("-") : n) + "/" + std::to_string((unsigned)p[0x22]);
}

size_t phase_index(double t) {
  const auto &sp = S().opt.split;
  size_t i = 0;
  while (i < sp.size() && t >= sp[i]) ++i;
  return i;
}

void refresh_modules() {
  State &s = S();
  s.mods.clear();
  if (!s.probe.guest_ptr) return;
  auto rd = [&](uint32_t a, uint32_t &v) -> bool {
    const uint8_t *p = s.probe.guest_ptr(a, 4);
    if (!p) return false;
    v = be32(p);
    return true;
  };
  uint32_t m = 0;
  if (!rd(0x800030C8, m)) return; // __OSModuleInfoList.head
  for (int n = 0; m && n < 4096; ++n) {
    uint32_t id, next, nsec, secinfo;
    if (!rd(m, id) || !rd(m + 4, next) || !rd(m + 0xC, nsec) || !rd(m + 0x10, secinfo)) break;
    if (nsec > 64 || secinfo < 0x80000000u) break;
    for (uint32_t i = 0; i < nsec; ++i) {
      uint32_t off, size;
      if (!rd(secinfo + 8 * i, off) || !rd(secinfo + 8 * i + 4, size)) break;
      const uint32_t base = off & ~1u;
      if ((off & 1) && size && size < 0x01000000u && base >= 0x80000000u)
        s.mods.push_back({base, base + size, id, i});
    }
    if (next == m) break;
    m = next;
  }
  std::sort(s.mods.begin(), s.mods.end(),
            [](const ModRange &a, const ModRange &b) { return a.lo < b.lo; });
}

uint64_t rel_key(uint32_t pc) {
  const auto &mods = S().mods;
  auto it = std::upper_bound(mods.begin(), mods.end(), pc,
                             [](uint32_t v, const ModRange &r) { return v < r.lo; });
  if (it != mods.begin()) {
    --it;
    if (pc >= it->lo && pc < it->hi)
      return ((uint64_t)it->id << 40) | ((uint64_t)it->sec << 32) | (pc - it->lo);
  }
  return (0xFFFFFull << 40) | pc;
}

std::string fmt(const char *f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

std::string window_line(const Snapshot &a, const Snapshot &b, const uint64_t gc[kGcCount],
                        const uint64_t hp[kHostPhaseCount]) {
  const double dt = b.t - a.t > 1e-6 ? b.t - a.t : 1e-6;
  auto rate = [&](uint64_t x, uint64_t y) { return (double)(y - x) / dt; };
  uint64_t gtot = 0, htot = 0;
  for (int i = 0; i < kGcCount; ++i) gtot += gc[i];
  for (int i = 0; i < kHostPhaseCount; ++i) htot += hp[i];
  auto pc = [](uint64_t x, uint64_t t) { return t ? 100.0 * (double)x / (double)t : 0.0; };
  std::string l = fmt(
      "t=%.1f stage=%s lfps=%.2f fps=%.2f gfps=%.2f inst/s=%.1fM exec/s=%.1fM polls/s=%.1fM interp_entries/s=%.0f "
      "interp_insn/s=%.1fM "
      "shortcut/s=%.0f vi_host/s=%.1f vi_virt/s=%.1f ext_irq/s=%.1f dec/s=%.1f idle_skips/s=%.0f "
      "host_iters/s=%.1f gx_ms/s=%.0f swap_ms/s=%.0f | cpu: native=%.0f%% idle=%.0f%% rel=%.0f%% "
      "none=%.0f%% | host:",
      b.t, read_stage().c_str(), b.logic >= a.logic ? rate(a.logic, b.logic) : 0.0,
      rate(a.frames, b.frames), rate(a.xfb, b.xfb), rate(a.inst, b.inst) / 1e6,
      (rate(a.inst, b.inst) - rate(a.c[kIdleSkipTicks], b.c[kIdleSkipTicks])) / 1e6,
      rate(a.polls, b.polls) / 1e6,
      rate(a.c[kInterpEntries], b.c[kInterpEntries]),
      rate(a.c[kInterpInsns], b.c[kInterpInsns]) / 1e6,
      rate(a.c[kDispatchShortcut], b.c[kDispatchShortcut]), rate(a.c[kViHost], b.c[kViHost]),
      rate(a.c[kViVirtual], b.c[kViVirtual]), rate(a.c[kExtIrq], b.c[kExtIrq]),
      rate(a.c[kDecIrq], b.c[kDecIrq]), rate(a.c[kIdleSkips], b.c[kIdleSkips]),
      rate(a.c[kHostIters], b.c[kHostIters]), rate(a.c[kGxUs], b.c[kGxUs]) / 1000.0,
      rate(a.c[kSwapUs], b.c[kSwapUs]) / 1000.0, pc(gc[kGcNative], gtot), pc(gc[kGcIdle], gtot),
      pc(gc[kGcOther], gtot), pc(gc[kGcNone], gtot));
  for (int i = 0; i < kHostPhaseCount; ++i)
    if (hp[i]) l += fmt(" %s=%.0f%%", kPhaseNames[i], pc(hp[i], htot));
  return l;
}

#ifdef _WIN32
void sampler_main() {
  State &s = S();
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  const LONGLONG period_100ns = (LONGLONG)(10000000.0 / (double)s.opt.hz);
  double next_mod_refresh = 0, next_line = s.opt.period, next_stage_check = 0;
  Snapshot win0 = take_snapshot();
  uint64_t win_gc[kGcCount] = {}, win_hp[kHostPhaseCount] = {};
  size_t cur_phase = 0;
  s.phases[0].s0 = win0;
  s.phases[0].started = true;
  const auto busy0 = clk::now();
  double busy = 0;
  uint64_t last_inst = 0;
  int jumps_reported = 0;

  while (!s.stop.load(std::memory_order_relaxed)) {
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -period_100ns;
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 100);
    } else {
      Sleep(1);
    }
    const auto b0 = clk::now();
    const double t = now_s();

    // Phase boundary: close the previous phase with a snapshot.
    size_t pi = s.opt.split_stage ? cur_phase : std::min(phase_index(t), s.phases.size() - 1);
    if (s.opt.split_stage && t >= next_stage_check) {
      next_stage_check = t + 0.1;
      const std::string stg = read_stage();
      if (stg != s.phases[cur_phase].name) {
        if (s.phases[cur_phase].cpu_samples == 0 && cur_phase + 1 == s.phases.size()) {
          s.phases[cur_phase].name = stg; // nothing sampled yet: just rename
        } else {
          s.phases.emplace_back();
          s.phases.back().name = stg;
          pi = s.phases.size() - 1;
        }
      }
    }
    if (pi != cur_phase) {
      const Snapshot sn = take_snapshot();
      s.phases[cur_phase].s1 = sn;
      cur_phase = pi;
      s.phases[cur_phase].s0 = sn;
      s.phases[cur_phase].started = true;
      std::cout << "[PROF] phase " << cur_phase << " (" << s.phases[cur_phase].name
                << ") begins at t=" << sn.t << "\n";
    }
    PhaseData &ph = s.phases[cur_phase];

    if (t >= next_mod_refresh) {
      refresh_modules();
      next_mod_refresh = t + 0.2;
    }

    for (int slot = 0; slot < 2; ++slot) {
      HANDLE h = s.threads[slot];
      if (!h) continue;
      if (SuspendThread(h) == (DWORD)-1) {
        ++s.suspend_failures;
        continue;
      }
      CONTEXT c;
      std::memset(&c, 0, sizeof c);
      c.ContextFlags = CONTEXT_CONTROL;
      const BOOL ok = GetThreadContext(h, &c); // also completes the suspension
      uint32_t gpc = 0, glr = 0;
      uint64_t ginst = 0;
      if (slot == 0 && s.probe.pc) {
        gpc = *s.probe.pc;
        glr = s.probe.lr ? *s.probe.lr : 0;
        ginst = s.probe.inst ? *s.probe.inst : 0;
      }
      const uint8_t hph = g_host_phase.load(std::memory_order_relaxed);
      ResumeThread(h);
      if (!ok) {
        ++s.suspend_failures;
        continue;
      }
      // No allocation happens while a thread is suspended (it may hold the
      // heap lock); everything below runs after ResumeThread.
      const uint64_t rip = (uint64_t)c.Rip & 0x00FFFFFFFFFFFFFFull;
      if (slot == 0) {
        // Clock discontinuities: > 5e9 instructions between two samples (~1 ms)
        // cannot be execution; report where the guest was (first 16).
        if (last_inst && ginst > last_inst + 5000000000ull && jumps_reported < 16) {
          ++jumps_reported;
          std::cout << "[PROF] clock jump +" << (ginst - last_inst) << " insns at t=" << t
                    << " pc=0x" << std::hex << gpc << " lr=0x" << glr << std::dec << '\n';
        }
        last_inst = ginst;
        GuestCat gc;
        if (gpc == 0) gc = kGcNone;
        else if ((gpc >= s.opt.idle_lo && gpc < s.opt.idle_hi) ||
                 ((gpc == s.opt.idle_call_a || gpc == s.opt.idle_call_b) &&
                  glr >= s.opt.idle_lo && glr < s.opt.idle_hi))
          gc = kGcIdle;
        else if (s.probe.is_native && s.probe.is_native(gpc)) gc = kGcNative;
        else gc = kGcOther;
        ++ph.cpu_samples;
        ++ph.gcat[gc];
        ++win_gc[gc];
        ++ph.cpu_rip[rip | ((uint64_t)gc << 56)];
        if (gc == kGcNative) ++ph.dol_pc[gpc];
        else if (gc == kGcOther) ++ph.rel[rel_key(gpc)];
      } else {
        const uint8_t p = hph < kHostPhaseCount ? hph : 0;
        ++ph.host_samples;
        ++ph.host_phase[p];
        ++win_hp[p];
        ++ph.host_rip[rip | ((uint64_t)p << 56)];
      }
    }

    if (t >= next_line) {
      const Snapshot sn = take_snapshot();
      std::string l = "[PROF] " + window_line(win0, sn, win_gc, win_hp) + "\n";
      std::cout << l;
      s.series.push_back(l);
      win0 = sn;
      std::memset(win_gc, 0, sizeof win_gc);
      std::memset(win_hp, 0, sizeof win_hp);
      next_line = t + s.opt.period;
    }
    busy += std::chrono::duration<double>(clk::now() - b0).count();
  }
  s.phases[cur_phase].s1 = take_snapshot();
  s.sampler_busy_s = busy;
  (void)busy0;
  if (timer) CloseHandle(timer);
}
#endif

// ---------------------------------------------------------------- report ---

struct DolSym {
  uint32_t lo, hi;
  std::string name;
};

std::vector<DolSym> load_dol_symbols(const std::string &path) {
  std::vector<DolSym> v;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    const size_t a = line.find(','), b = line.find(',', a + 1);
    if (a == std::string::npos || b == std::string::npos) continue;
    const uint32_t lo = (uint32_t)std::strtoul(line.c_str() + a + 1, nullptr, 0);
    const uint32_t sz = (uint32_t)std::strtoul(line.c_str() + b + 1, nullptr, 0);
    if (!lo) continue;
    v.push_back({lo, lo + (sz ? sz : 4), line.substr(0, a)});
  }
  std::sort(v.begin(), v.end(), [](const DolSym &x, const DolSym &y) { return x.lo < y.lo; });
  return v;
}

const DolSym *find_dol(const std::vector<DolSym> &v, uint32_t pc) {
  auto it = std::upper_bound(v.begin(), v.end(), pc,
                             [](uint32_t x, const DolSym &d) { return x < d.lo; });
  if (it == v.begin()) return nullptr;
  --it;
  return pc < it->hi ? &*it : nullptr;
}

struct RelSyms {
  std::map<uint32_t, std::string> modname;
  // (id<<8|sec) -> sorted functions
  std::map<uint64_t, std::vector<DolSym>> funcs;
};

RelSyms load_rel_symbols(const std::string &path) {
  RelSyms r;
  std::ifstream f(path);
  std::string kind;
  while (f >> kind) {
    if (kind == "M") {
      uint32_t id;
      std::string name;
      f >> id >> name;
      r.modname[id] = name;
    } else if (kind == "F") {
      uint32_t id, sec;
      std::string off, size, name;
      f >> id >> sec >> off >> size >> name;
      const uint32_t lo = (uint32_t)std::strtoul(off.c_str(), nullptr, 16);
      const uint32_t sz = (uint32_t)std::strtoul(size.c_str(), nullptr, 16);
      r.funcs[((uint64_t)id << 8) | sec].push_back({lo, lo + (sz ? sz : 4), name});
    } else {
      std::string rest;
      std::getline(f, rest);
    }
  }
  for (auto &[k, v] : r.funcs)
    std::sort(v.begin(), v.end(), [](const DolSym &x, const DolSym &y) { return x.lo < y.lo; });
  return r;
}

struct HostSym {
  uint64_t start = 0;
  std::string name, module;
};

#ifdef _WIN32
std::unordered_map<uint64_t, HostSym> symbolize(const std::vector<uint64_t> &rips,
                                                std::string &note) {
  std::unordered_map<uint64_t, HostSym> out;
  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS |
                SYMOPT_NO_PROMPTS);
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string dir = exe;
  const size_t slash = dir.find_last_of("\\/");
  if (slash != std::string::npos) dir.resize(slash);
  if (!SymInitialize(proc, dir.c_str(), TRUE)) {
    note = fmt("SymInitialize failed (%lu)", GetLastError());
    return out;
  }
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 1024];
  for (uint64_t rip : rips) {
    HostSym hs;
    IMAGEHLP_MODULE64 mi;
    std::memset(&mi, 0, sizeof mi);
    mi.SizeOfStruct = sizeof mi;
    hs.module = SymGetModuleInfo64(proc, rip, &mi) ? mi.ModuleName : "?";
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    std::memset(buf, 0, sizeof buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 1000;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, rip, &disp, si)) {
      hs.start = si->Address;
      hs.name = si->Name;
    } else {
      hs.start = rip & ~0xFFFull;
      hs.name = fmt("?%" PRIx64, hs.start);
    }
    out[rip] = std::move(hs);
  }
  SymCleanup(proc);
  return out;
}
#endif

bool has(const std::string &s, const char *p) { return s.find(p) != std::string::npos; }
bool starts(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }

std::string classify_cpu(const HostSym &h, uint8_t gc, const std::string &exe_mod) {
  if (gc == kGcIdle) return "idle-loop";
  const std::string &n = h.name;
  if (!exe_mod.empty() && h.module != exe_mod) return "os/" + h.module;
  if (n == "run_game") return gc == kGcOther ? "dispatcher(rel target)" : "dispatcher";
  if (starts(n, "ss_") || starts(n, "func_")) return "native-dol";
  if (has(n, "interpret") || has(n, "try_native_helper") || has(n, "in_recompiled_code"))
    return "interp";
  if (has(n, "process_pending_callbacks") || has(n, "EventScheduler") || has(n, "ai_update") ||
      has(n, "di_tick") || has(n, "advance_counter") || has(n, "poll_stat") ||
      has(n, "dec_expired") || has(n, "read_timebase"))
    return "poll";
  if (has(n, "WGPIPE") || has(n, "wgp_")) return "gx-wgp";
  if (has(n, "MMU::")) return gc == kGcOther ? "mem(interp)" : "mem(native)";
  if (has(n, "HW_Reg") || has(n, "MMIODispatcher") || has(n, "::hw::")) return "mmio";
  if (has(n, "memcpy") || has(n, "memset") || has(n, "memmove")) return "crt-mem";
  return gc == kGcOther ? "runtime(interp ctx)" : "runtime/hle";
}

std::string pct(uint64_t x, uint64_t t) { return fmt("%5.1f%%", t ? 100.0 * x / t : 0.0); }

void write_phase(std::ostream &o, const char *title, const PhaseData &ph,
                 const std::unordered_map<uint64_t, HostSym> &syms, const std::string &exe_mod,
                 const std::vector<DolSym> &dol, const RelSyms &rs, unsigned top) {
  o << "\n=== " << title << " ===\n";
  const Snapshot &a = ph.s0, &b = ph.s1;
  const double dt = b.t - a.t > 1e-6 ? b.t - a.t : 1e-6;
  o << fmt("wall %.1f-%.1f s (%.1f s); cpu samples %" PRIu64 ", host samples %" PRIu64 "\n", a.t,
           b.t, dt, ph.cpu_samples, ph.host_samples);
  o << fmt("presented fps %.2f (frames %" PRIu64 "); game fps (XFB copies) %.2f; guest inst/s "
           "%.1fM; polls/s %.1fM\n",
           (b.frames - a.frames) / dt, b.frames - a.frames, (b.xfb - a.xfb) / dt,
           (b.inst - a.inst) / dt / 1e6, (b.polls - a.polls) / dt / 1e6);
  o << "counters per second:";
  for (int i = 0; i < kCounterCount; ++i) o << fmt(" %s=%.1f", kCounterNames[i], (b.c[i] - a.c[i]) / dt);
  o << "\next irq causes per second (PI bit):";
  for (int i = 0; i < 32; ++i)
    if (b.irq[i] != a.irq[i]) o << fmt(" b%d=%.1f", i, (b.irq[i] - a.irq[i]) / dt);
  o << "\n";
  if (b.c[kHostIters] > a.c[kHostIters]) {
    const double it = (double)(b.c[kHostIters] - a.c[kHostIters]);
    o << fmt("host loop: %.1f it/s; per iteration gx %.2f ms, swap %.2f ms, present %.2f ms\n",
             it / dt, (b.c[kGxUs] - a.c[kGxUs]) / it / 1000.0,
             (b.c[kSwapUs] - a.c[kSwapUs]) / it / 1000.0,
             (b.c[kPresentUs] - a.c[kPresentUs]) / it / 1000.0);
  }

  // CPU thread: guest category (by published guest PC) and mode (by host symbol).
  o << "\n-- CPU thread: guest category (published guest PC) --\n";
  for (int i = 0; i < kGcCount; ++i)
    o << fmt("  %-12s %s\n", kGcNames[i], pct(ph.gcat[i], ph.cpu_samples).c_str());

  std::map<std::string, uint64_t> modes;
  std::map<std::pair<uint64_t, std::string>, uint64_t> funcs; // (start, category) -> n
  for (const auto &[key, n] : ph.cpu_rip) {
    const uint64_t rip = key & 0x00FFFFFFFFFFFFFFull;
    const uint8_t gc = (uint8_t)(key >> 56);
    auto it = syms.find(rip);
    HostSym h = it != syms.end() ? it->second : HostSym{rip, fmt("?%" PRIx64, rip), "?"};
    const std::string cat = classify_cpu(h, gc, exe_mod);
    modes[cat] += n;
    funcs[{h.start, cat}] += n;
  }
  std::vector<std::pair<uint64_t, std::string>> mv;
  for (auto &[k, n] : modes) mv.push_back({n, k});
  std::sort(mv.rbegin(), mv.rend());
  o << "\n-- CPU thread: mode (host symbol + guest category) --\n";
  for (auto &[n, k] : mv) o << fmt("  %-24s %s\n", k.c_str(), pct(n, ph.cpu_samples).c_str());

  std::unordered_map<uint64_t, std::string> start_name;
  for (const auto &[rip, h] : syms) start_name.emplace(h.start, h.module + "!" + h.name);
  auto name_of = [&](uint64_t start) -> std::string {
    auto it = start_name.find(start);
    return it != start_name.end() ? it->second : fmt("?%" PRIx64, start);
  };
  std::vector<std::pair<uint64_t, std::pair<uint64_t, std::string>>> fv;
  for (auto &[k, n] : funcs) fv.push_back({n, k});
  std::sort(fv.begin(), fv.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
  o << "\n-- CPU thread: top host functions --\n";
  for (size_t i = 0; i < fv.size() && i < top; ++i)
    o << fmt("  %s  %-20s %s\n", pct(fv[i].first, ph.cpu_samples).c_str(),
             fv[i].second.second.c_str(), name_of(fv[i].second.first).c_str());

  // Guest functions: native DOL by symbols.csv, interpreted by module.
  std::map<std::string, uint64_t> gfun;
  uint64_t gtot = 0;
  for (const auto &[pc, n] : ph.dol_pc) {
    const DolSym *d = find_dol(dol, pc);
    gfun[d ? d->name : fmt("dol?%08x", pc)] += n;
    gtot += n;
  }
  std::map<std::string, uint64_t> relmod;
  for (const auto &[key, n] : ph.rel) {
    const uint32_t id = (uint32_t)(key >> 40), sec = (uint32_t)(key >> 32) & 0xFF;
    const uint32_t off = (uint32_t)key;
    gtot += n;
    if (id == 0xFFFFF) {
      gfun[fmt("unmapped@%08x", off & ~0xFFFu)] += n;
      relmod["(no module)"] += n;
      continue;
    }
    auto mn = rs.modname.find(id);
    const std::string mname = mn != rs.modname.end() ? mn->second : fmt("rel#%u", id);
    relmod[mname] += n;
    std::string fname;
    auto fl = rs.funcs.find(((uint64_t)id << 8) | sec);
    if (fl != rs.funcs.end())
      if (const DolSym *d = find_dol(fl->second, off)) fname = d->name;
    if (fname.empty()) fname = fmt("s%u+0x%x", sec, off & ~0xFFu);
    gfun[mname + ":" + fname] += n;
  }
  std::vector<std::pair<uint64_t, std::string>> gv;
  for (auto &[k, n] : gfun) gv.push_back({n, k});
  std::sort(gv.rbegin(), gv.rend());
  o << "\n-- Guest functions (non-idle CPU samples; DOL via symbols.csv, REL via module list) --\n";
  for (size_t i = 0; i < gv.size() && i < top; ++i)
    o << fmt("  %s  %s\n", pct(gv[i].first, ph.cpu_samples).c_str(), gv[i].second.c_str());
  std::vector<std::pair<uint64_t, std::string>> rv;
  for (auto &[k, n] : relmod) rv.push_back({n, k});
  std::sort(rv.rbegin(), rv.rend());
  o << "\n-- Interpreted REL modules --\n";
  for (size_t i = 0; i < rv.size() && i < top; ++i)
    o << fmt("  %s  %s\n", pct(rv[i].first, ph.cpu_samples).c_str(), rv[i].second.c_str());
  (void)gtot;

  // Host thread.
  o << "\n-- Host thread: phase --\n";
  for (int i = 0; i < kHostPhaseCount; ++i)
    if (ph.host_phase[i])
      o << fmt("  %-12s %s\n", kPhaseNames[i], pct(ph.host_phase[i], ph.host_samples).c_str());
  std::map<std::pair<uint64_t, int>, uint64_t> hf;
  for (const auto &[key, n] : ph.host_rip) {
    const uint64_t rip = key & 0x00FFFFFFFFFFFFFFull;
    auto it = syms.find(rip);
    hf[{it != syms.end() ? it->second.start : rip, (int)(key >> 56)}] += n;
  }
  std::vector<std::pair<uint64_t, std::pair<uint64_t, int>>> hv;
  for (auto &[k, n] : hf) hv.push_back({n, k});
  std::sort(hv.begin(), hv.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
  o << "\n-- Host thread: top functions --\n";
  for (size_t i = 0; i < hv.size() && i < top; ++i)
    o << fmt("  %s  %-11s %s\n", pct(hv[i].first, ph.host_samples).c_str(),
             kPhaseNames[hv[i].second.second], name_of(hv[i].second.first).c_str());
}

std::string default_out_path() {
  const char *shot = std::getenv("NWII_SCREENSHOT");
  if (!shot || !*shot) return "nwii-profile.txt";
  std::string p = shot;
  const size_t slash = p.find_last_of("\\/");
  std::string dir = slash == std::string::npos ? "." : p.substr(0, slash);
  std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
  if (starts(base, "beeld-")) base = base.substr(6);
  return dir + "/" + base + "-profile.txt";
}

} // namespace

bool init(const GuestProbe &probe) {
  State &s = S();
  const char *e = std::getenv("NWII_PROFILE");
  const long hz = e ? std::strtol(e, nullptr, 10) : 0;
  if (hz <= 0) return g_on = false;
  s.opt.hz = (unsigned)std::min<long>(hz, 10000);
  s.probe = probe;
  s.t0 = clk::now();
  if (const char *v = std::getenv("NWII_PROFILE_OUT")) s.opt.out = v;
  else s.opt.out = default_out_path();
  if (const char *v = std::getenv("NWII_PROFILE_STAGE")) s.opt.stage_addr = (uint32_t)std::strtoul(v, nullptr, 16);
  if (const char *v = std::getenv("NWII_PROFILE_LOGIC")) s.opt.logic_addr = (uint32_t)std::strtoul(v, nullptr, 16);
  if (const char *v = std::getenv("NWII_PROFILE_SPLIT"); v && std::strcmp(v, "stage") == 0) {
    s.opt.split_stage = true;
  } else if (v) {
    std::stringstream ss(v);
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) s.opt.split.push_back(std::atof(tok.c_str()));
    std::sort(s.opt.split.begin(), s.opt.split.end());
  }
  if (const char *v = std::getenv("NWII_PROFILE_SYMBOLS")) s.opt.symbols = v;
  if (const char *v = std::getenv("NWII_PROFILE_RELSYMS")) s.opt.relsyms = v;
  if (const char *v = std::getenv("NWII_PROFILE_TOP")) s.opt.top = (unsigned)std::max(1, std::atoi(v));
  if (const char *v = std::getenv("NWII_PROFILE_PERIOD")) s.opt.period = std::max(0.5, std::atof(v));
  if (const char *v = std::getenv("NWII_PROFILE_IDLE")) {
    unsigned lo = 0, hi = 0;
    if (std::sscanf(v, "%x-%x", &lo, &hi) == 2) { s.opt.idle_lo = lo; s.opt.idle_hi = hi; }
  }
  s.phases.resize(s.opt.split.size() + 1);
  for (auto &c : g_counters) c.store(0);
  for (auto &c : g_irq_bits) c.store(0);
  g_on = true;
#ifdef _WIN32
  s.sampler = std::thread(sampler_main);
#endif
  std::cout << "[PROF] sampling at " << s.opt.hz << " Hz, report -> " << s.opt.out << std::endl;
  return true;
}

bool host_function_extent(const void *fn, uintptr_t &lo, uintptr_t &len) {
#ifdef _WIN32
  DWORD64 image = 0;
  PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)(uintptr_t)fn, &image, nullptr);
  if (!rf) return false;
  lo = (uintptr_t)(image + rf->BeginAddress);
  len = (uintptr_t)(rf->EndAddress - rf->BeginAddress);
  return (uintptr_t)fn >= lo && (uintptr_t)fn - lo < len;
#else
  (void)fn;
  lo = len = 0;
  return false;
#endif
}

void register_current_thread(int slot) {
  if (!g_on || slot < 0 || slot > 1) return;
#ifdef _WIN32
  HANDLE h = nullptr;
  if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                      THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                      FALSE, 0))
    S().threads[slot] = h;
#endif
}

void shutdown_and_report() {
  State &s = S();
  if (!g_on || s.reported) return;
  s.reported = true;
  s.stop = true;
  if (s.sampler.joinable()) s.sampler.join();

  std::vector<uint64_t> rips;
  {
    std::unordered_map<uint64_t, bool> seen;
    for (const auto &ph : s.phases) {
      for (const auto &[k, n] : ph.cpu_rip) seen[k & 0x00FFFFFFFFFFFFFFull] = true;
      for (const auto &[k, n] : ph.host_rip) seen[k & 0x00FFFFFFFFFFFFFFull] = true;
    }
    for (const auto &[r, b] : seen) rips.push_back(r);
  }
  std::string note;
  std::unordered_map<uint64_t, HostSym> syms;
  std::string exe_mod;
#ifdef _WIN32
  syms = symbolize(rips, note);
  {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string p = exe;
    const size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos) p = p.substr(slash + 1);
    const size_t dot = p.find_last_of('.');
    if (dot != std::string::npos) p.resize(dot);
    exe_mod = p;
  }
#endif
  const std::vector<DolSym> dol = load_dol_symbols(s.opt.symbols);
  const RelSyms rs = load_rel_symbols(s.opt.relsyms);

  // Whole-run aggregate.
  PhaseData all;
  bool first = true;
  for (const auto &ph : s.phases) {
    if (!ph.started) continue;
    if (first) { all.s0 = ph.s0; first = false; }
    all.s1 = ph.s1;
    all.cpu_samples += ph.cpu_samples;
    all.host_samples += ph.host_samples;
    for (int i = 0; i < kGcCount; ++i) all.gcat[i] += ph.gcat[i];
    for (int i = 0; i < kHostPhaseCount; ++i) all.host_phase[i] += ph.host_phase[i];
    for (const auto &[k, n] : ph.cpu_rip) all.cpu_rip[k] += n;
    for (const auto &[k, n] : ph.host_rip) all.host_rip[k] += n;
    for (const auto &[k, n] : ph.dol_pc) all.dol_pc[k] += n;
    for (const auto &[k, n] : ph.rel) all.rel[k] += n;
  }

  std::ostringstream o;
  o << "NWII PERF2 profile report\n";
  o << fmt("rate %u Hz/thread; symbols: %zu host RIPs (%s); DOL syms %zu (%s); REL modules %zu, "
           "REL func tables %zu (%s)\n",
           s.opt.hz, rips.size(), note.empty() ? "dbghelp ok" : note.c_str(), dol.size(),
           s.opt.symbols.c_str(), rs.modname.size(), rs.funcs.size(), s.opt.relsyms.c_str());
  o << fmt("sampler: busy %.2f s, suspend/context failures %" PRIu64 "\n", s.sampler_busy_s,
           s.suspend_failures);
  o << "\n-- periodic windows --\n";
  for (const auto &l : s.series) o << l;
  write_phase(o, "WHOLE RUN", all, syms, exe_mod, dol, rs, s.opt.top);
  if (s.phases.size() > 1)
    for (size_t i = 0; i < s.phases.size(); ++i)
      if (s.phases[i].started) {
        const std::string title = fmt("PHASE %zu %s", i, s.phases[i].name.c_str());
        write_phase(o, title.c_str(), s.phases[i], syms, exe_mod, dol, rs, s.opt.top);
      }
  std::ofstream f(s.opt.out, std::ios::binary);
  f << o.str();
  f.close();
  std::cout << "[PROF] report written: " << s.opt.out << " (" << all.cpu_samples
            << " cpu samples, " << all.host_samples << " host samples)" << std::endl;
}

} // namespace nwii::runtime::prof
