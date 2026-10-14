// RELAOT (roadmap M2.7/M2.8): registry + dispatch for statically recompiled
// REL modules. See include/runtime/rel_aot.h for the code model.
//
// Module discovery: the game links RELs with its own (recompiled) OSLink,
// which enqueues the module on __OSModuleInfoList (head 0x800030C8, tail
// 0x800030CC) before relocating it. Every lookup compares the queue tail with
// the value seen at the last scan; a change (a module was linked) or a header
// id mismatch at a registered base (memory reused) triggers a rescan of the
// queue. A module is registered only when its id has a generated descriptor,
// its executable section is already converted to an absolute address, the
// size matches and image_hash() of the linked bytes equals the hash of the
// original .rel. Unlinked modules drop out at the next rescan. Guest memory is
// only read.
//
// Environment (read once):
//   NWII_REL_AOT=0           disable (pure interpreter, as before)
//   NWII_REL_AOT_ONLY=a,b    only these module ids;  NWII_REL_AOT_SKIP=a,b
//   NWII_REL_STAT=1          [RELAOT] counters every 5 s (host time)
//   NWII_REL_PROF=1          guest-PC sampler thread: DOL / REL-AOT / REL-interp
//   NWII_REL_LOCKSTEP=a,b|all  differential check of top-level native entries
//                            against the interpreter (diagnostic, slow)
#include "runtime/rel_aot.h"
#include "runtime/cpu_context.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace nwii {
namespace runtime {
namespace relaot {

#ifdef NWII_HAVE_REL_AOT_TABLE
extern const ModuleDesc *const g_modules[];
extern const uint32_t g_module_count;
extern const DolFn g_dol_funcs[];
extern const uint32_t g_dol_func_count;
#else
static const ModuleDesc *const *const g_modules = nullptr;
static const uint32_t g_module_count = 0;
static const DolFn *const g_dol_funcs = nullptr;
static const uint32_t g_dol_func_count = 0;
#endif

namespace {

std::set<uint32_t> parse_ids(const char *s) {
  std::set<uint32_t> out;
  if (!s)
    return out;
  std::string v(s);
  size_t p = 0;
  while (p < v.size()) {
    size_t q = v.find(',', p);
    if (q == std::string::npos)
      q = v.size();
    std::string tok = v.substr(p, q - p);
    if (!tok.empty() && tok != "all")
      out.insert((uint32_t)std::strtoul(tok.c_str(), nullptr, 0));
    p = q + 1;
  }
  return out;
}

struct Flags {
  bool enabled;
  bool stat;
  bool prof;
  bool lockstep_all;
  std::set<uint32_t> only, skip, lockstep;
  Flags() {
    const char *e = std::getenv("NWII_REL_AOT");
    enabled = g_module_count > 0 && !(e && e[0] == '0');
    stat = std::getenv("NWII_REL_STAT") != nullptr;
    prof = std::getenv("NWII_REL_PROF") != nullptr;
    only = parse_ids(std::getenv("NWII_REL_AOT_ONLY"));
    skip = parse_ids(std::getenv("NWII_REL_AOT_SKIP"));
    const char *ls = std::getenv("NWII_REL_LOCKSTEP");
    lockstep_all = ls && std::string(ls) == "all";
    lockstep = parse_ids(ls);
  }
  bool allowed(uint32_t id) const {
    if (!only.empty() && !only.count(id))
      return false;
    return !skip.count(id);
  }
};
const Flags g_flags;

constexpr uint32_t kQueueHead = 0x800030C8u;
constexpr uint32_t kQueueTail = 0x800030CCu;
constexpr int kMaxLive = 256;

struct Live {
  uint32_t lo, hi;  // guest range of the executable section
  uint32_t hdr;     // module header (OSModuleInfo) address
  uint32_t id;
  const ModuleDesc *m;
  const SectionDesc *s;
};

// All modules in the guest queue (AOT or not), for the sampler.
struct QueueEntry {
  uint32_t lo, hi, id;
};

Live g_live[kMaxLive];
int g_nlive = 0;
QueueEntry g_queue[kMaxLive];
std::atomic<int> g_nqueue{0};
uint32_t g_tail_seen = 0xFFFFFFFFu;
uint32_t g_recheck = 0; // >0: a module was mid-link at the last scan
int g_last_hit = 0;
std::vector<const ModuleDesc *> g_by_id;
// (id, base) pairs whose image hash already passed / failed
std::vector<std::pair<uint64_t, bool>> g_hash_cache;
uint32_t g_dol_lo = 0, g_dol_hi = 0;

struct Stats {
  uint64_t enter_calls = 0, enter_native = 0, enter_miss = 0, loop_cont = 0;
  uint64_t call_rel = 0, call_dol = 0, call_fallback = 0;
  uint64_t rescans = 0, registered = 0, hash_fail = 0;
  uint64_t lockstep_checks = 0, lockstep_diffs = 0, lockstep_skipped = 0;
} g_st;
std::atomic<bool> g_in_native{false};
CPUContext *g_ctx_for_sampler = nullptr;

inline const uint8_t *host(CPUContext &ctx, uint32_t addr, uint32_t size = 4) {
  return ctx.mmu.get_ptr(addr, size);
}
inline uint32_t gread32(CPUContext &ctx, uint32_t addr) {
  const uint8_t *p = host(ctx, addr);
  return p ? rd32(p) : 0;
}

void init_tables() {
  static bool done = false;
  if (done)
    return;
  done = true;
  uint32_t max_id = 0;
  for (uint32_t i = 0; i < g_module_count; ++i)
    max_id = std::max(max_id, g_modules[i]->id);
  g_by_id.assign(max_id + 1, nullptr);
  for (uint32_t i = 0; i < g_module_count; ++i)
    g_by_id[g_modules[i]->id] = g_modules[i];
  if (g_dol_func_count) {
    g_dol_lo = g_dol_funcs[0].addr;
    g_dol_hi = g_dol_funcs[g_dol_func_count - 1].addr + 4;
  }
  if (g_flags.enabled || g_flags.stat)
    std::cout << "[RELAOT] " << g_module_count << " recompiled REL modules, "
              << g_dol_func_count << " DOL entry points; "
              << (g_flags.enabled ? "enabled" : "DISABLED (NWII_REL_AOT=0)")
              << std::endl;
}

bool hash_ok(CPUContext &ctx, const ModuleDesc *m, const SectionDesc *s, uint32_t base) {
  const uint64_t key = (uint64_t)m->id << 32 | base;
  for (const auto &kv : g_hash_cache)
    if (kv.first == key)
      return kv.second;
  const uint8_t *p = host(ctx, base, s->size);
  const bool ok = p && image_hash(p, s->size) == s->hash;
  if (!ok) {
    ++g_st.hash_fail;
    std::cout << "[RELAOT] module " << m->id << " (" << m->name << ") sec "
              << s->index << " @0x" << std::hex << base << std::dec
              << ": image hash mismatch, left to the interpreter" << std::endl;
  }
  if (g_hash_cache.size() > 4096)
    g_hash_cache.clear();
  g_hash_cache.emplace_back(key, ok);
  return ok;
}

void rescan(CPUContext &ctx) {
  ++g_st.rescans;
  g_tail_seen = gread32(ctx, kQueueTail);
  g_recheck = 0;
  int nlive = 0, nq = 0;
  bool pending = false;
  uint32_t mod = gread32(ctx, kQueueHead);
  for (int guard = 0; mod && guard < 1024; ++guard) {
    const uint8_t *h = host(ctx, mod, 0x4C);
    if (!h)
      break;
    const uint32_t id = rd32(h);
    const uint32_t nsec = rd32(h + 0x0C);
    const uint32_t sinfo = rd32(h + 0x10);
    const uint32_t next = rd32(h + 0x04);
    if (sinfo < 0x80000000u) { // OSLink has not converted the header yet
      pending = true;
      mod = next;
      continue;
    }
    // queue entry for the sampler: first executable section
    for (uint32_t s = 0; s < nsec && s < 64 && nq < kMaxLive; ++s) {
      const uint8_t *si = host(ctx, sinfo + 8 * s, 8);
      if (!si)
        break;
      const uint32_t off = rd32(si), size = rd32(si + 4);
      if ((off & 1) && size && (off & ~1u) >= 0x80000000u) {
        g_queue[nq++] = {off & ~1u, (off & ~1u) + size, id};
        break;
      }
    }
    if (g_flags.stat || g_flags.prof) { // which modules the game links (AOT or not)
      static std::set<uint32_t> seen_ids;
      if (seen_ids.insert(id).second)
        std::cout << "[RELAOT] queue: module id " << id << " first seen @0x" << std::hex << mod
                  << std::dec << std::endl;
    }
    const ModuleDesc *m = id < g_by_id.size() ? g_by_id[id] : nullptr;
    if (m && g_flags.allowed(id)) {
      for (uint32_t k = 0; k < m->nsecs && nlive < kMaxLive; ++k) {
        const SectionDesc *s = &m->secs[k];
        if (s->index >= nsec)
          continue;
        const uint8_t *si = host(ctx, sinfo + 8 * s->index, 8);
        if (!si)
          continue;
        const uint32_t off = rd32(si), size = rd32(si + 4);
        const uint32_t base = off & ~1u;
        if (!(off & 1) || base < 0x80000000u) {
          pending = true;
          continue;
        }
        if (size != s->size || !hash_ok(ctx, m, s, base))
          continue;
        bool dup = false;
        for (int j = 0; j < nlive; ++j)
          dup |= g_live[j].s == s;
        if (dup)
          continue;
        const uint8_t *hb = host(ctx, base, size);
        if (!hb)
          continue;
        bool was = false;
        for (int j = 0; j < g_nlive; ++j)
          was |= g_live[j].s == s && g_live[j].lo == base;
        if (!was) {
          ++g_st.registered;
          if (g_flags.stat)
            std::cout << "[RELAOT] linked " << m->name << " id=" << id
                      << " sec" << s->index << " @0x" << std::hex << base
                      << "-0x" << base + size << std::dec << std::endl;
        }
        *s->sb = base;
        *s->sh = hb;
        g_live[nlive++] = {base, base + size, mod, id, m, s};
      }
    }
    mod = next;
  }
  g_nlive = nlive;
  g_nqueue.store(nq, std::memory_order_release);
  g_last_hit = 0;
  if (pending)
    g_recheck = 1024; // rescan again soon: a module is mid-link
}

// Native function for guest address pc, or nullptr. *is_start: pc is the
// function's first instruction (not a mid-function resume point).
Fn lookup(CPUContext &ctx, uint32_t pc, bool *is_start = nullptr) {
  const uint32_t tail = gread32(ctx, kQueueTail);
  if (tail != g_tail_seen || (g_recheck && --g_recheck == 0))
    rescan(ctx);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const Live *L = nullptr;
    if (g_last_hit < g_nlive && pc - g_live[g_last_hit].lo <
                                    g_live[g_last_hit].hi - g_live[g_last_hit].lo)
      L = &g_live[g_last_hit];
    else
      for (int i = 0; i < g_nlive; ++i)
        if (pc - g_live[i].lo < g_live[i].hi - g_live[i].lo) {
          L = &g_live[i];
          g_last_hit = i;
          break;
        }
    if (!L)
      return nullptr;
    if (gread32(ctx, L->hdr) != L->id) { // memory reused: header gone
      if (attempt == 0) {
        rescan(ctx);
        continue;
      }
      return nullptr;
    }
    const uint32_t off = pc - L->lo;
    if (off & 3)
      return nullptr;
    const FuncEntry *f = L->s->funcs;
    uint32_t lo = 0, hi = L->s->nfuncs;
    while (lo < hi) {
      const uint32_t mid = (lo + hi) / 2;
      if (f[mid].end <= off)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo < L->s->nfuncs && off >= f[lo].start && off < f[lo].end) {
      if (off != f[lo].start) { // only at a generated entry point
        const uint32_t *e = L->s->entries + f[lo].first_entry;
        if (!std::binary_search(e, e + f[lo].n_entries, off))
          return nullptr;
      }
      if (is_start)
        *is_start = off == f[lo].start;
      return f[lo].fn;
    }
    return nullptr;
  }
  return nullptr;
}

Fn dol_lookup(uint32_t t) {
  if (t - g_dol_lo >= g_dol_hi - g_dol_lo)
    return nullptr;
  const DolFn *b = g_dol_funcs, *e = g_dol_funcs + g_dol_func_count;
  const DolFn *it = std::lower_bound(
      b, e, t, [](const DolFn &d, uint32_t a) { return d.addr < a; });
  return (it != e && it->addr == t) ? it->fn : nullptr;
}

void maybe_print_stats() {
  static auto last = std::chrono::steady_clock::now();
  static uint64_t tick = 0;
  if ((++tick & 0x3FF) != 0)
    return;
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(5))
    return;
  last = now;
  std::cout << "[RELAOT] enter=" << g_st.enter_calls << " native=" << g_st.enter_native
            << " miss=" << g_st.enter_miss << " cont=" << g_st.loop_cont
            << " call_rel=" << g_st.call_rel << " call_dol=" << g_st.call_dol
            << " call_fb=" << g_st.call_fallback << " live=" << g_nlive
            << " rescans=" << g_st.rescans << " reg=" << g_st.registered
            << " hashfail=" << g_st.hash_fail;
  if (!g_flags.lockstep.empty() || g_flags.lockstep_all)
    std::cout << " lockstep=" << g_st.lockstep_checks << " diffs="
              << g_st.lockstep_diffs << " ls_skip=" << g_st.lockstep_skipped;
  std::cout << std::endl;
}

// ---- guest-PC sampler (NWII_REL_PROF) -------------------------------------
std::atomic<bool> g_sampler_stop{false};
std::thread g_sampler;
void sampler_main();
void stop_sampler() { // atexit: before the CRT/iostreams and the context go away
  g_sampler_stop.store(true);
  if (g_sampler.joinable())
    g_sampler.join();
}
void start_sampler(CPUContext &ctx) {
  g_ctx_for_sampler = &ctx;
  g_sampler = std::thread(sampler_main);
  std::atexit(stop_sampler);
}
void sampler_main() {
  struct Bucket { uint64_t aot = 0, interp = 0; };
  std::vector<Bucket> per_id(4096);
  uint64_t dol = 0, other = 0, aot = 0, interp = 0, total = 0;
  auto last = std::chrono::steady_clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    CPUContext *ctx = g_ctx_for_sampler;
    if (g_sampler_stop.load() || !ctx || !ctx->is_running)
      return; // CPU thread gone / process exiting
    const uint32_t pc = std::atomic_ref<uint32_t>(ctx->pc).load(std::memory_order_relaxed);
    const bool native = g_in_native.load(std::memory_order_relaxed);
    ++total;
    bool found = false;
    if (g_dol_hi && pc - g_dol_lo < g_dol_hi - g_dol_lo) {
      ++dol;
      found = true;
    } else {
      const int nq = g_nqueue.load(std::memory_order_acquire);
      for (int i = 0; i < nq && !found; ++i) {
        const QueueEntry q = g_queue[i];
        if (pc - q.lo < q.hi - q.lo) {
          found = true;
          Bucket &b = per_id[q.id & 4095];
          if (native) { ++aot; ++b.aot; } else { ++interp; ++b.interp; }
        }
      }
    }
    if (!found)
      ++other;
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(5) && total) {
      last = now;
      char buf[256];
      std::snprintf(buf, sizeof buf,
                    "[RELPROF] samples=%llu dol=%.1f%% rel_aot=%.1f%% rel_interp=%.1f%% other=%.1f%% top:",
                    (unsigned long long)total, 100.0 * dol / total, 100.0 * aot / total,
                    100.0 * interp / total, 100.0 * other / total);
      std::string line = buf;
      std::vector<std::pair<uint64_t, uint32_t>> top;
      for (uint32_t id = 0; id < per_id.size(); ++id)
        if (per_id[id].aot + per_id[id].interp)
          top.emplace_back(per_id[id].aot + per_id[id].interp, id);
      std::sort(top.rbegin(), top.rend());
      for (size_t i = 0; i < top.size() && i < 8; ++i) {
        std::snprintf(buf, sizeof buf, " %u:%.1f%%(aot %.0f%%)", top[i].second,
                      100.0 * top[i].first / total,
                      100.0 * per_id[top[i].second].aot / top[i].first);
        line += buf;
      }
      std::cout << line << std::endl;
      dol = other = aot = interp = total = 0;
      for (auto &b : per_id) b = Bucket{};
    }
  }
}

} // namespace

void lockstep_check(CPUContext &ctx, Fn f, uint32_t id); // rel_aot_lockstep below

bool enter(CPUContext &ctx) {
  if (!g_flags.enabled) {
    if (g_flags.prof && !g_ctx_for_sampler) {
      init_tables();
      start_sampler(ctx);
    }
    if (g_flags.prof) {
      const uint32_t tail = gread32(ctx, kQueueTail);
      if (tail != g_tail_seen) rescan(ctx); // keep the sampler's module map current
    }
    return false;
  }
  init_tables();
  if (g_flags.prof && !g_ctx_for_sampler) {
    start_sampler(ctx);
  }
  ++g_st.enter_calls;
  if (g_flags.stat)
    maybe_print_stats();
  bool start = false;
  Fn f = lookup(ctx, ctx.pc, &start);
  if (!f) {
    ++g_st.enter_miss;
    return false;
  }
  const bool ls = !g_flags.lockstep.empty() || g_flags.lockstep_all;
  for (;;) {
    ++g_st.enter_native;
    if (ls && start) {
      // lockstep only whole calls (entry at a function start)
      uint32_t id = 0;
      for (int i = 0; i < g_nlive; ++i)
        if (ctx.pc - g_live[i].lo < g_live[i].hi - g_live[i].lo) id = g_live[i].id;
      if (g_flags.lockstep_all || g_flags.lockstep.count(id)) {
        lockstep_check(ctx, f, id);
        return true;
      }
    }
    g_in_native.store(true, std::memory_order_relaxed);
    f(ctx);
    g_in_native.store(false, std::memory_order_relaxed);
    if (!ctx.is_running)
      return true;
    const uint32_t pc = ctx.pc;
    f = lookup(ctx, pc, &start);
    if (!f)
      return true;
    // The dispatcher polls once per iteration; do the same between two
    // native dispatches so interrupts are not deferred longer than before.
    if (process_pending_callbacks(ctx) || ctx.pc != pc)
      return true;
    ++g_st.loop_cont;
  }
}

bool call(CPUContext &ctx, uint32_t target, uint32_t ret) {
  ctx.pc = target;
  Fn f = dol_lookup(target);
  if (f) {
    ++g_st.call_dol;
  } else {
    f = lookup(ctx, target);
    if (!f) {
      ++g_st.call_fallback;
      return false;
    }
    ++g_st.call_rel;
  }
  f(ctx);
  return ctx.pc == ret;
}

// ---- lockstep (NWII_REL_LOCKSTEP) ------------------------------------------
// Runs one top-level native entry and the interpreter from the same snapshot
// (registers + MEM1 + MEM2) and compares the architectural state at the
// moment both return to the entry's LR with the entry's stack pointer. Only
// self-contained calls are compared: if the interpreted run leaves the
// registered REL ranges for anything but that return, the check is skipped
// (state restored, native result kept). Diagnostic only: copies 88 MB per
// check, capped at NWII_REL_LOCKSTEP_MAX (default 2000) checks.
namespace {
struct RegSnap {
  std::array<uint32_t, 32> gpr;
  std::array<double, 32> fpr, ps1;
  uint32_t cr[8];
  uint32_t lr, ctr, xer, pc, msr, fpscr;
  uint64_t inst;
  std::array<uint32_t, 8> gqr;
};
uint32_t pack_cr(const CPUContext &c, int i) {
  return (uint32_t)c.cr[i].lt << 3 | (uint32_t)c.cr[i].gt << 2 |
         (uint32_t)c.cr[i].eq << 1 | (uint32_t)c.cr[i].so;
}
RegSnap snap(const CPUContext &c) {
  RegSnap s;
  s.gpr = c.gpr;
  s.fpr = c.fpr;
  s.ps1 = c.ps1;
  for (int i = 0; i < 8; ++i)
    s.cr[i] = pack_cr(c, i);
  s.lr = c.lr; s.ctr = c.ctr; s.xer = c.xer; s.pc = c.pc; s.msr = c.msr;
  s.fpscr = c.fpscr; s.inst = c.inst_count;
  for (int i = 0; i < 8; ++i) s.gqr[i] = c.gqr[i];
  return s;
}
void restore(CPUContext &c, const RegSnap &s) {
  c.gpr = s.gpr;
  c.fpr = s.fpr;
  c.ps1 = s.ps1;
  for (int i = 0; i < 8; ++i) {
    c.cr[i].lt = (s.cr[i] >> 3) & 1; c.cr[i].gt = (s.cr[i] >> 2) & 1;
    c.cr[i].eq = (s.cr[i] >> 1) & 1; c.cr[i].so = s.cr[i] & 1;
  }
  c.lr = s.lr; c.ctr = s.ctr; c.xer = s.xer; c.pc = s.pc; c.msr = s.msr;
  c.fpscr = s.fpscr; c.inst_count = s.inst;
  for (int i = 0; i < 8; ++i) c.gqr[i] = s.gqr[i];
}
bool same_bits(double a, double b) { return std::memcmp(&a, &b, 8) == 0; }
// True if executing insn twice (interpreted pass + native pass) could have a
// side effect outside the MEM1/MEM2 snapshot: hardware registers / the GX
// write-gather pipe (physical 0x0C000000-0x0FFFFFFF), the locked cache,
// syscalls, MSR/SPR writes. Such calls are not compared.
bool unsafe_for_lockstep(const CPUContext &c, uint32_t insn) {
  const uint32_t op = insn >> 26, rA = (insn >> 16) & 31, rB = (insn >> 11) & 31;
  const uint32_t base = rA ? c.gpr[rA] : 0;
  uint32_t ea = 0;
  bool mem = false;
  if (op >= 32 && op <= 55) { ea = base + (uint32_t)(int32_t)(int16_t)(insn & 0xFFFF); mem = true; }
  else if (op >= 56 && op <= 61) { int32_t d = insn & 0xFFF; if (d & 0x800) d -= 0x1000; ea = base + (uint32_t)d; mem = true; }
  else if (op == 31 || op == 4) {
    const uint32_t xo = (insn >> 1) & 0x3FF;
    if (op == 31 && (xo == 146 || xo == 467 || xo == 210 || xo == 242)) return true; // mtmsr/mtspr/mtsr
    ea = base + c.gpr[rB];
    mem = op == 4 ? (xo == 1014 || ((insn >> 1) & 0x3F) == 6 || ((insn >> 1) & 0x3F) == 7 ||
                     ((insn >> 1) & 0x3F) == 38 || ((insn >> 1) & 0x3F) == 39)
                  : true; // any other X-form: check its rA+rB conservatively
  } else if (op == 17 || (op == 19 && ((insn >> 1) & 0x3FF) == 50)) return true; // sc, rfi
  if (!mem) return false;
  const uint32_t p = ea & 0x3FFFFFFFu;
  return (p >= 0x0C000000u && p < 0x10000000u) || ea >= 0xE0000000u;
}
} // namespace

void lockstep_check(CPUContext &ctx, Fn f, uint32_t id) {
  static const uint64_t max_checks = [] {
    const char *m = std::getenv("NWII_REL_LOCKSTEP_MAX");
    return m ? std::strtoull(m, nullptr, 0) : 2000ull;
  }();
  static std::vector<uint8_t> m1_0, m2_0, m1_i, m2_i;
  static std::set<uint32_t> skip_fn; // entry pcs that are not self-contained
  const uint32_t entry = ctx.pc;
  if (g_st.lockstep_checks >= max_checks || skip_fn.count(entry)) {
    g_in_native.store(true, std::memory_order_relaxed);
    f(ctx);
    g_in_native.store(false, std::memory_order_relaxed);
    return;
  }
  const RegSnap s0 = snap(ctx);
  m1_0 = ctx.mmu.mem1;
  m2_0 = ctx.mmu.mem2;
  // 1) interpreter (interpret_one only; no polls, no native helpers)
  const uint32_t ret = ctx.lr, sp = ctx.gpr[1];
  bool contained = true;
  uint64_t steps = 0;
  while (!(ctx.pc == ret && ctx.gpr[1] == sp)) {
    bool in_rel = false;
    for (int i = 0; i < g_nlive && !in_rel; ++i)
      in_rel = ctx.pc - g_live[i].lo < g_live[i].hi - g_live[i].lo;
    if (!in_rel || ++steps > 50000000ull) {
      contained = false;
      break;
    }
    const uint8_t *ip = ctx.mmu.get_ptr(ctx.pc, 4);
    if (!ip || unsafe_for_lockstep(ctx, rd32(ip))) {
      contained = false;
      break;
    }
    ++ctx.inst_count;
    interpret_one(ctx);
  }
  if (!contained) {
    ++g_st.lockstep_skipped;
    skip_fn.insert(entry);
    restore(ctx, s0);
    ctx.mmu.mem1 = m1_0;
    ctx.mmu.mem2 = m2_0;
    g_in_native.store(true, std::memory_order_relaxed);
    f(ctx);
    g_in_native.store(false, std::memory_order_relaxed);
    return;
  }
  const RegSnap si = snap(ctx);
  m1_i = ctx.mmu.mem1;
  m2_i = ctx.mmu.mem2;
  // 2) native from the same snapshot
  restore(ctx, s0);
  ctx.mmu.mem1 = m1_0;
  ctx.mmu.mem2 = m2_0;
  g_in_native.store(true, std::memory_order_relaxed);
  f(ctx);
  // a native run may stop early (poll redirected it): only compare when it
  // also returned to the entry's LR
  g_in_native.store(false, std::memory_order_relaxed);
  if (!(ctx.pc == ret && ctx.gpr[1] == sp)) {
    ++g_st.lockstep_skipped;
    return; // native result kept, nothing to compare
  }
  ++g_st.lockstep_checks;
  const RegSnap sn = snap(ctx);
  std::string diff;
  char buf[160];
  for (int i = 0; i < 32; ++i) {
    if (si.gpr[i] != sn.gpr[i]) {
      std::snprintf(buf, sizeof buf, " r%d i=%08x n=%08x", i, si.gpr[i], sn.gpr[i]);
      diff += buf;
    }
    if (!same_bits(si.fpr[i], sn.fpr[i]) || !same_bits(si.ps1[i], sn.ps1[i])) {
      std::snprintf(buf, sizeof buf, " f%d i=%g/%g n=%g/%g", i, si.fpr[i], si.ps1[i],
                    sn.fpr[i], sn.ps1[i]);
      diff += buf;
    }
  }
  for (int i = 0; i < 8; ++i)
    if (si.cr[i] != sn.cr[i]) {
      std::snprintf(buf, sizeof buf, " cr%d i=%x n=%x", i, si.cr[i], sn.cr[i]);
      diff += buf;
    }
  if (si.lr != sn.lr || si.ctr != sn.ctr || si.xer != sn.xer || si.inst != sn.inst) {
    std::snprintf(buf, sizeof buf, " lr %x/%x ctr %x/%x xer %x/%x inst %llu/%llu", si.lr,
                  sn.lr, si.ctr, sn.ctr, si.xer, sn.xer, (unsigned long long)si.inst,
                  (unsigned long long)sn.inst);
    diff += buf;
  }
  auto memdiff = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b,
                     uint32_t base) {
    const size_t n = std::min(a.size(), b.size());
    if (std::memcmp(a.data(), b.data(), n) == 0)
      return;
    for (size_t i = 0; i < n; ++i)
      if (a[i] != b[i]) {
        std::snprintf(buf, sizeof buf, " mem@%08x i=%02x n=%02x", (uint32_t)(base + i), a[i], b[i]);
        diff += buf;
        break;
      }
  };
  memdiff(m1_i, ctx.mmu.mem1, 0x80000000u);
  memdiff(m2_i, ctx.mmu.mem2, 0x90000000u);
  if (!diff.empty()) {
    ++g_st.lockstep_diffs;
    std::cout << "[RELAOT-LOCKSTEP] DIFF module " << id << " entry 0x" << std::hex
              << entry << std::dec << ":" << diff << std::endl;
  } else if (g_flags.stat && (g_st.lockstep_checks & 255) == 1) {
    std::cout << "[RELAOT-LOCKSTEP] ok checks=" << g_st.lockstep_checks << std::endl;
  }
}

} // namespace relaot
} // namespace runtime
} // namespace nwii
