// FREEZE8: default-on freeze watchdog and loud interpreter-stop report. Diagnostics only: it reads guest
// memory/registers and host thread state, never writes guest state.
//
// - Watchdog thread: when no new game frame (g_stat_frames, counted by the GX frame cutter) appears for
//   NWII_FREEZE_SECONDS (default 8) while the process is alive, it writes one "[FREEZE8]" block to stdout
//   (= the speelsessie log): guest PC/LR/CTR/MSR, interpreter vs native, REL module + offset + symbol, guest
//   back chain, a PC histogram of the stall, OS threads (state, saved PC/LR), interrupt/poll counters, GX FIFO
//   state, the game's stage/event (provider in ss_boot.cpp) and the host call stacks of all threads (DbgHelp
//   + the exe's .pdb). While the stall lasts a short reminder follows every 60 s; "frames resumed" when it ends.
//   It also flushes stdout every 2 s so a killed process still leaves a complete log, and reports (then ends)
//   a process whose CPU thread does not stop within 15 s after shutdown was requested. A stall inside
//   PPCHalt/OSPanic/OSFatal/... gets a "hint" line (guest-side fatal path). Every NWII_FREEZE_HEARTBEAT seconds
//   (default 30, 0 = off) one heartbeat line + the game's stage/Link/event line is logged, so a session that is
//   stuck while still rendering can be located too.
//   NWII_FREEZE_WATCHDOG=0 disables it. Cost: one 20 ms sleep/sample loop on its own thread.
// - report_fatal(): ss_boot.cpp's [SS fatal] path gets the same location report + message box.
// - g_interp_unsupported_hook: an unsupported instruction / taken trap in the PPC interpreter gets the same
//   location report and, in an interactive session (no NWII_RUN_SECONDS / NWII_HEADLESS / replay), an
//   SDL_ShowSimpleMessageBox, before the interpreter stops the process (fail fast, as before).
#include "runtime/cpu_context.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

extern uint64_t g_stat_frames;
extern uint64_t g_stat_draws;
extern uint64_t g_stat_xfb_copies;
extern std::mutex g_fifo_mutex;
extern std::vector<uint8_t> g_hw_fifo;

namespace nwii::runtime {
extern CPUContext* g_ctx_ptr;
extern void (*g_interp_unsupported_hook)(const CPUContext&, uint32_t, const char*, uint32_t);
extern volatile int g_interp_depth;
extern volatile uint64_t g_interp_budget_exceeded;
bool in_recompiled_code(uint32_t pc);
namespace hw {
extern std::atomic<uint32_t> pi_intsr;
extern std::atomic<uint32_t> pi_intmr;
}

namespace freeze {
using GameInfoFn = std::string (*)(CPUContext&);
void note_cpu_thread(CPUContext& ctx);
void set_game_info(GameInfoFn fn);

namespace {

std::atomic<GameInfoFn> g_game_info{nullptr};
std::atomic<bool> g_stop{false};
// The CPUContext is owned by main() and freed right after the CPU thread has been joined. The watchdog only reads
// it while holding g_ctx_guard with g_cpu_alive set; the CPU thread clears g_cpu_alive under the same lock when it
// exits (thread_local destructor), so no read can race the teardown.
std::mutex g_ctx_guard;
bool g_cpu_alive = false;
struct CpuThreadExit {
    ~CpuThreadExit() {
        std::lock_guard<std::mutex> lock(g_ctx_guard);
        g_cpu_alive = false;
    }
};
std::atomic<unsigned long> g_cpu_tid{0};
#ifdef _WIN32
const unsigned long g_main_tid = GetCurrentThreadId(); // static init runs on the main thread
#else
const unsigned long g_main_tid = 0;
#endif

std::string hex(uint64_t v, int w = 8) {
    std::ostringstream o;
    o << "0x" << std::hex << std::uppercase << std::setw(w) << std::setfill('0') << v;
    return o.str();
}

bool env_off(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0';
}

std::string exe_dir() {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string d = path;
    const size_t s = d.find_last_of("\\/");
    return s == std::string::npos ? std::string(".") : d.substr(0, s);
#else
    return ".";
#endif
}

// ------------------------------------------------------------------ guest symbols
struct Sym {
    uint32_t lo, hi;
    std::string name;
};
struct Symbols {
    std::vector<Sym> dol;
    std::map<uint32_t, std::string> modname;
    std::map<uint64_t, std::vector<Sym>> rel; // (id << 8 | section) -> functions
    std::string source;
};

Symbols& symbols() {
    static Symbols s = [] {
        Symbols r;
        std::vector<std::string> candidates;
        if (const char* e = std::getenv("NWII_FREEZE_SYMBOLS")) candidates.push_back(e);
        const std::string d = exe_dir();
        candidates.push_back(d + "/../../pc-tests/freeze8/freeze_symbols.txt"); // outputs/pc-prototype/{bin,speel}
        candidates.push_back(d + "/freeze_symbols.txt");
        candidates.push_back("../pc-tests/freeze8/freeze_symbols.txt");
        for (const auto& c : candidates) {
            std::ifstream f(c);
            if (!f) continue;
            r.source = c;
            std::string kind;
            while (f >> kind) {
                if (kind == "D") {
                    std::string a, n, name;
                    f >> a >> n >> name;
                    const uint32_t lo = (uint32_t)std::strtoul(a.c_str(), nullptr, 16);
                    r.dol.push_back({lo, lo + (uint32_t)std::strtoul(n.c_str(), nullptr, 16), name});
                } else if (kind == "M") {
                    uint32_t id;
                    std::string name;
                    f >> id >> name;
                    r.modname[id] = name;
                } else if (kind == "F") {
                    uint32_t id, sec;
                    std::string a, n, name;
                    f >> id >> sec >> a >> n >> name;
                    const uint32_t lo = (uint32_t)std::strtoul(a.c_str(), nullptr, 16);
                    r.rel[((uint64_t)id << 8) | sec].push_back(
                        {lo, lo + (uint32_t)std::strtoul(n.c_str(), nullptr, 16), name});
                } else {
                    std::string rest;
                    std::getline(f, rest);
                }
            }
            break;
        }
        auto by_lo = [](const Sym& a, const Sym& b) { return a.lo < b.lo; };
        std::sort(r.dol.begin(), r.dol.end(), by_lo);
        for (auto& [k, v] : r.rel) std::sort(v.begin(), v.end(), by_lo);
        return r;
    }();
    return s;
}

const Sym* find_sym(const std::vector<Sym>& v, uint32_t x) {
    auto it = std::upper_bound(v.begin(), v.end(), x, [](uint32_t a, const Sym& s) { return a < s.lo; });
    if (it == v.begin()) return nullptr;
    --it;
    return x < it->hi ? &*it : nullptr;
}

bool guest_ram(uint32_t p, uint32_t size) {
    return (p >= 0x80000000u && uint64_t(p) + size <= 0x81800000u) ||
           (p >= 0x90000000u && uint64_t(p) + size <= 0x94000000u);
}

// "main.dol Foo+0x10" / "REL d_a_xNP#123 sec1+0x1A4 Bar+0x20" / "?" for a guest address.
// Walks the original OSLink module list (__OSModuleInfoList 0x800030C8; OSModuleInfo id +0, next +4,
// numSections +0xC, sectionInfo +0x10 = absolute after OSLink; section = {offset|exec, size}).
std::string describe(CPUContext& ctx, uint32_t pc) {
    Symbols& s = symbols();
    std::ostringstream o;
    if (in_recompiled_code(pc)) {
        o << "main.dol";
        if (const Sym* f = find_sym(s.dol, pc)) o << " " << f->name << "+0x" << std::hex << (pc - f->lo);
        return o.str();
    }
    uint32_t m = ctx.mmu.read32(0x800030C8);
    for (unsigned n = 0; n < 1024 && guest_ram(m, 0x20); ++n) {
        const uint32_t id = ctx.mmu.read32(m), count = ctx.mmu.read32(m + 0xC), info = ctx.mmu.read32(m + 0x10);
        for (uint32_t i = 0; i < count && i < 64 && guest_ram(info + i * 8, 8); ++i) {
            const uint32_t off = ctx.mmu.read32(info + i * 8), size = ctx.mmu.read32(info + i * 8 + 4);
            const uint32_t base = off & ~1u;
            if (!base || pc < base || pc - base >= size) continue;
            auto mn = s.modname.find(id);
            o << "REL " << (mn != s.modname.end() ? mn->second : std::string("module")) << "#" << std::dec << id
              << " sec" << i << "+0x" << std::hex << (pc - base);
            auto fv = s.rel.find(((uint64_t)id << 8) | i);
            if (fv != s.rel.end())
                if (const Sym* f = find_sym(fv->second, pc - base))
                    o << " " << f->name << "+0x" << (pc - base - f->lo);
            if (!(off & 1)) o << " (non-exec section!)";
            return o.str();
        }
        m = ctx.mmu.read32(m + 4);
    }
    if (pc >= 0x80000000u && pc < 0x80003000u) return "low memory (exception vectors / OS globals)";
    return guest_ram(pc, 4) ? "guest RAM outside DOL text and linked RELs" : "not guest RAM";
}

std::string backchain(CPUContext& ctx, uint32_t sp, int depth) {
    std::ostringstream o;
    for (int i = 0; i < depth && guest_ram(sp, 8); ++i) {
        const uint32_t parent = ctx.mmu.read32(sp);
        if (!guest_ram(parent, 8) || parent <= sp) break;
        const uint32_t ret = ctx.mmu.read32(parent + 4);
        o << "[FREEZE8]   #" << std::dec << i << " " << hex(ret) << " " << describe(ctx, ret) << "\n";
        sp = parent;
    }
    return o.str();
}

// Known guest-side "stop" functions: a stall inside them is the game's own fatal path, not an emulator hang.
std::string hint_for(CPUContext& ctx, const std::vector<uint32_t>& pcs) {
    static const char* const kStops[][2] = {
        {"PPCHalt", "the game halted the CPU (after OSPanic/OSFatal): a guest-side fatal error, see OSReport output above"},
        {"OSPanic", "the game called OSPanic (guest assertion)"},
        {"OSFatal", "the game called OSFatal (fatal error screen)"},
        {"__OSUnhandledException", "unhandled guest exception (DSI/ISI/program/alignment)"},
        {"OSDefaultExceptionHandler", "unhandled guest exception (OSDefaultExceptionHandler)"},
        {"__DVDShowFatalMessage", "DVD fatal error message (disc read failure)"},
    };
    Symbols& s = symbols();
    for (uint32_t pc : pcs) {
        const Sym* f = in_recompiled_code(pc) ? find_sym(s.dol, pc) : nullptr;
        if (!f) continue;
        for (const auto& k : kStops)
            if (f->name == k[0]) return std::string("[FREEZE8] hint: ") + k[1] + "\n";
    }
    (void)ctx;
    return {};
}

std::string mode_of(CPUContext& ctx, uint32_t pc) {
    if (in_recompiled_code(pc)) return "native (recompiled main.dol)";
    if (g_interp_depth > 0) return "PPC interpreter (REL code)";
    return "native REL AOT or HLE";
}

// ------------------------------------------------------------------ output
void write_block(const std::string& text) {
    // Flush the CRT buffers first (bounded wait: a hung thread may hold the stdout lock), then write the block
    // straight to the OS handle so it reaches the log even then.
    auto flushed = std::make_shared<std::atomic<bool>>(false);
    std::thread([flushed] {
        std::fflush(stdout);
        std::fflush(stderr);
        flushed->store(true);
    }).detach();
    for (int i = 0; i < 50 && !flushed->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
#else
    std::fwrite(text.data(), 1, text.size(), stdout);
#endif
}

// ------------------------------------------------------------------ host stacks
#ifdef _WIN32
struct HostStack {
    DWORD tid = 0;
    CONTEXT ctx{};
    std::vector<uint8_t> stack;
    DWORD64 stack_lo = 0;
    std::string note;
    std::vector<DWORD64> pcs;
};

thread_local const HostStack* t_walk = nullptr;

BOOL CALLBACK read_mem(HANDLE, DWORD64 base, PVOID buf, DWORD size, LPDWORD read) {
    const HostStack* hs = t_walk;
    if (hs && base >= hs->stack_lo && base + size <= hs->stack_lo + hs->stack.size()) {
        std::memcpy(buf, hs->stack.data() + (base - hs->stack_lo), size);
        if (read) *read = size;
        return TRUE;
    }
    SIZE_T got = 0;
    const BOOL ok = ReadProcessMemory(GetCurrentProcess(), (LPCVOID)base, buf, size, &got);
    if (read) *read = (DWORD)got;
    return ok;
}

// Suspend -> copy registers + the live stack page range -> resume. Nothing that can take a CRT/heap lock
// runs while the thread is suspended (the buffer is allocated before).
void snapshot(HostStack& hs) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, hs.tid);
    if (!h) { hs.note = "OpenThread failed"; return; }
    hs.stack.resize(256 * 1024);
    hs.ctx.ContextFlags = CONTEXT_FULL;
    size_t copied = 0;
    if (SuspendThread(h) != (DWORD)-1) {
        if (GetThreadContext(h, &hs.ctx)) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery((LPCVOID)hs.ctx.Rsp, &mbi, sizeof mbi)) {
                const uint64_t end = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
                const size_t want = (size_t)std::min<uint64_t>(hs.stack.size(), end - hs.ctx.Rsp);
                SIZE_T got = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)hs.ctx.Rsp, hs.stack.data(), want, &got))
                    copied = got;
            }
        } else {
            hs.note = "GetThreadContext failed";
        }
        ResumeThread(h);
    } else {
        hs.note = "SuspendThread failed";
    }
    CloseHandle(h);
    hs.stack.resize(copied);
    hs.stack_lo = hs.ctx.Rsp;
}

void walk(HostStack& hs, int max_frames) {
    if (!hs.note.empty()) return;
    STACKFRAME64 f{};
    f.AddrPC.Offset = hs.ctx.Rip;
    f.AddrPC.Mode = AddrModeFlat;
    f.AddrStack.Offset = hs.ctx.Rsp;
    f.AddrStack.Mode = AddrModeFlat;
    f.AddrFrame.Offset = hs.ctx.Rbp;
    f.AddrFrame.Mode = AddrModeFlat;
    CONTEXT c = hs.ctx;
    t_walk = &hs;
    for (int i = 0; i < max_frames; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), nullptr, &f, &c, read_mem,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (!f.AddrPC.Offset) break;
        hs.pcs.push_back(f.AddrPC.Offset);
    }
    t_walk = nullptr;
}

std::string host_symbol(DWORD64 pc) {
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
    std::memset(buf, 0, sizeof buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 511;
    DWORD64 disp = 0;
    std::ostringstream o;
    IMAGEHLP_MODULE64 mi{};
    mi.SizeOfStruct = sizeof mi;
    const char* mod = SymGetModuleInfo64(GetCurrentProcess(), pc, &mi) ? mi.ModuleName : "?";
    if (SymFromAddr(GetCurrentProcess(), pc, &disp, si))
        o << mod << "!" << si->Name << "+0x" << std::hex << disp;
    else
        o << mod << "!" << hex(pc, 12);
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof line;
    DWORD ldisp = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), pc, &ldisp, &line)) {
        std::string file = line.FileName ? line.FileName : "";
        const size_t s = file.find_last_of("\\/");
        o << " (" << (s == std::string::npos ? file : file.substr(s + 1)) << ":" << std::dec << line.LineNumber << ")";
    }
    return o.str();
}

std::string host_stacks() {
    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
                tids.push_back(te.th32ThreadID);
        CloseHandle(snap);
    }
    // main and CPU thread first
    std::stable_sort(tids.begin(), tids.end(), [](DWORD a, DWORD b) {
        auto rank = [](DWORD t) { return t == g_main_tid ? 0 : t == g_cpu_tid.load() ? 1 : 2; };
        return rank(a) < rank(b);
    });
    if (tids.size() > 32) tids.resize(32);
    std::vector<HostStack> threads(tids.size());
    for (size_t i = 0; i < tids.size(); ++i) threads[i].tid = tids[i];
    for (auto& t : threads) snapshot(t);
    std::ostringstream o;
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
                  SYMOPT_NO_PROMPTS);
    const bool sym = SymInitialize(proc, exe_dir().c_str(), TRUE) != FALSE;
    if (!sym) o << "[FREEZE8] host: SymInitialize failed (" << GetLastError() << "), raw addresses only\n";
    for (auto& t : threads) {
        const bool important = t.tid == g_main_tid || t.tid == g_cpu_tid.load();
        walk(t, important ? 28 : 8);
        o << "[FREEZE8] host thread " << std::dec << t.tid
          << (t.tid == g_main_tid ? " (main: SDL/GX/present)" : t.tid == g_cpu_tid.load() ? " (CPU: guest)" : "")
          << (t.note.empty() ? "" : " " + t.note) << "\n";
        const size_t shown = important ? t.pcs.size() : std::min<size_t>(t.pcs.size(), 4);
        for (size_t i = 0; i < shown; ++i)
            o << "[FREEZE8]     " << (sym ? host_symbol(t.pcs[i]) : hex(t.pcs[i], 12)) << "\n";
    }
    if (sym) SymCleanup(proc);
    return o.str();
}
#else
std::string host_stacks() { return "[FREEZE8] host stacks: not implemented on this platform\n"; }
#endif

// ------------------------------------------------------------------ guest state block
struct Sample {
    uint32_t pc, lr;
    uint64_t inst;
};

std::string os_threads(CPUContext& ctx) {
    std::ostringstream o;
    const uint32_t cur = ctx.mmu.read32(0x800000E4);
    o << "[FREEZE8] os: current thread " << hex(cur) << " current context " << hex(ctx.mmu.read32(0x800000D4))
      << "\n";
    // __OSActiveThreadQueue head 0x800000DC, linked via OSThread.linkActive.next (+0x2FC). OSThread: context
    // (lr +0x84, srr0 +0x198), state +0x2C8 (1 ready 2 running 4 waiting 8 moribund), suspend +0x2CC,
    // priority +0x2D0, queue +0x2DC, mutex +0x2F0.
    uint32_t th = ctx.mmu.read32(0x800000DC);
    for (int n = 0; n < 32 && guest_ram(th, 0x310); ++n) {
        const uint32_t srr0 = ctx.mmu.read32(th + 0x198), lr = ctx.mmu.read32(th + 0x84);
        const unsigned state = ctx.mmu.read16(th + 0x2C8);
        o << "[FREEZE8]   thread " << hex(th) << (th == cur ? " *running*" : "") << " state=" << std::dec << state
          << (state == 1 ? "(ready)" : state == 2 ? "(running)" : state == 4 ? "(waiting)" : state == 8 ? "(moribund)" : "")
          << " prio=" << (int32_t)ctx.mmu.read32(th + 0x2D0) << " suspend=" << (int32_t)ctx.mmu.read32(th + 0x2CC)
          << " queue=" << hex(ctx.mmu.read32(th + 0x2DC)) << " mutex=" << hex(ctx.mmu.read32(th + 0x2F0))
          << "\n[FREEZE8]     saved pc " << hex(srr0) << " " << describe(ctx, srr0) << "\n[FREEZE8]     saved lr "
          << hex(lr) << " " << describe(ctx, lr) << "\n";
        th = ctx.mmu.read32(th + 0x2FC);
    }
    return o.str();
}

std::string guest_block(CPUContext& ctx, const std::vector<Sample>& recent) {
    std::ostringstream o;
    const uint32_t pc = ctx.pc, lr = ctx.lr;
    o << "[FREEZE8] cpu: pc=" << hex(pc) << " lr=" << hex(lr) << " ctr=" << hex(ctx.ctr) << " msr=" << hex(ctx.msr)
      << " (EE=" << ((ctx.msr >> 15) & 1) << ") r1=" << hex(ctx.gpr[1]) << " r3=" << hex(ctx.gpr[3])
      << " r4=" << hex(ctx.gpr[4]) << " r31=" << hex(ctx.gpr[31]) << " inst=" << std::dec << ctx.inst_count << "\n";
    o << "[FREEZE8] mode: " << mode_of(ctx, pc) << " (interp depth " << g_interp_depth << ", interp budget-exceeded "
      << g_interp_budget_exceeded << "x)\n";
    o << "[FREEZE8] pc  -> " << describe(ctx, pc) << "\n";
    o << "[FREEZE8] lr  -> " << describe(ctx, lr) << "\n";
    o << "[FREEZE8] ctr -> " << describe(ctx, ctx.ctr) << "\n";
    o << "[FREEZE8] guest back chain (r1):\n" << backchain(ctx, ctx.gpr[1], 14);
    {
        std::vector<uint32_t> pcs{pc, lr};
        for (const auto& s : recent) pcs.push_back(s.pc);
        o << hint_for(ctx, pcs);
    }
    if (!recent.empty()) {
        std::map<uint32_t, int> hist;
        for (const auto& s : recent) ++hist[s.pc];
        std::vector<std::pair<int, uint32_t>> top;
        for (auto& [p, n] : hist) top.push_back({n, p});
        std::sort(top.rbegin(), top.rend());
        o << "[FREEZE8] guest pc samples (" << std::dec << recent.size() << " x 20 ms, "
          << (recent.back().inst - recent.front().inst) << " instructions executed meanwhile):\n";
        for (size_t i = 0; i < top.size() && i < 12; ++i)
            o << "[FREEZE8]   " << std::dec << std::setw(4) << top[i].first << "x " << hex(top[i].second) << " "
              << describe(ctx, top[i].second) << "\n";
        o << "[FREEZE8] last pcs:";
        for (size_t i = recent.size() > 12 ? recent.size() - 12 : 0; i < recent.size(); ++i)
            o << " " << std::hex << recent[i].pc;
        o << std::dec << "\n";
    }
    o << os_threads(ctx);
    o << "[FREEZE8] irq: pending callbacks=" << ctx.pending_cb_count.load() << " in_callback=" << ctx.in_callback
      << " depth=" << ctx.callback_depth << " pi_intsr=" << hex(hw::pi_intsr.load()) << " pi_intmr="
      << hex(hw::pi_intmr.load()) << " vblank_pending=" << ctx.vblank_pending.load() << " dec_pending="
      << ctx.dec_irq_pending << "\n";
    size_t fifo = 0;
    bool fifo_locked = false;
    if (g_fifo_mutex.try_lock()) {
        fifo = g_hw_fifo.size();
        g_fifo_mutex.unlock();
    } else {
        fifo_locked = true;
    }
    o << "[FREEZE8] gx: frames=" << std::dec << g_stat_frames << " draws=" << g_stat_draws << " xfb copies="
      << g_stat_xfb_copies << " pending write-gather FIFO bytes="
      << (fifo_locked ? std::string("? (FIFO mutex held)") : std::to_string(fifo)) << "\n";
    if (GameInfoFn gi = g_game_info.load()) o << gi(ctx);
    return o.str();
}

// ------------------------------------------------------------------ interpreter stop report
bool interactive_session() {
    return !std::getenv("NWII_RUN_SECONDS") && !std::getenv("NWII_HEADLESS") && !std::getenv("NWII_SS_INPUT_REPLAY") &&
           !env_off("NWII_FREEZE_MSGBOX");
}

std::atomic<bool> g_stop_reported{false};

void stop_report(CPUContext& ctx, const std::string& title, const std::string& detail, const std::string& user_text) {
    if (g_stop_reported.exchange(true)) return;
    std::ostringstream o;
    o << "\n[FREEZE8] ===== " << title << " =====\n"
      << detail << guest_block(ctx, {}) << "[FREEZE8] ===== end (the process stops now) =====\n";
    write_block(o.str());
    if (interactive_session()) {
        const std::string m = "Skyward Sword PC is gestopt.\n\n" + user_text +
                              "\n\nDe volledige diagnose staat in het logbestand van deze speelsessie "
                              "(outputs/pc-testresultaten/speelsessies). Stuur dat bestand mee.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Skyward Sword PC - gestopt", m.c_str(), nullptr);
    }
}

void on_unsupported(const CPUContext& cctx, uint32_t insn, const char* kind, uint32_t selector) {
    CPUContext& ctx = const_cast<CPUContext&>(cctx); // reads only
    const std::string where = describe(ctx, ctx.pc);
    std::ostringstream d, u;
    d << "[FREEZE8] insn=" << hex(insn) << " at " << hex(ctx.pc) << " " << where << " (selector " << std::dec
      << selector << ")\n";
    u << "De PPC-interpreter kwam een instructie tegen die hij niet kan uitvoeren:\n  " << kind << " (insn "
      << hex(insn) << ")\n  op " << hex(ctx.pc) << ": " << where;
    stop_report(ctx, std::string("INTERPRETER STOP: ") + kind, d.str(), u.str());
}

// ------------------------------------------------------------------ watchdog thread
void watchdog_main(double seconds, double heartbeat) {
    // Start only once the CPU thread registered itself (note_cpu_thread, first KPAD read = title screen).
    while (!g_stop.load() && !g_cpu_tid.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (g_stop.load() || !g_ctx_ptr) return;
    CPUContext& ctx = *g_ctx_ptr;
    using clk = std::chrono::steady_clock;
    std::vector<Sample> ring(1024);
    size_t head = 0, filled = 0;
    uint64_t last_frames = *(volatile uint64_t*)&g_stat_frames;
    auto last_change = clk::now(), last_flush = clk::now(), next_reminder = clk::now();
    const auto started = clk::now();
    auto next_beat = started + std::chrono::milliseconds((int64_t)(heartbeat * 1000));
    uint64_t beat_frames = 0, beat_inst = 0;
    auto shutdown_seen = clk::time_point{};
    bool armed = false, reported = false;
    uint64_t stall_start_inst = 0, stall_start_budget = 0, inst_at_change = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::unique_lock<std::mutex> alive_lock(g_ctx_guard);
        if (!g_cpu_alive) return; // CPU thread ended: the context is about to be freed, stop touching it
        const auto now = clk::now();
        ring[head] = {*(volatile uint32_t*)&ctx.pc, *(volatile uint32_t*)&ctx.lr, *(volatile uint64_t*)&ctx.inst_count};
        head = (head + 1) % ring.size();
        filled = std::min(filled + 1, ring.size());
        if (now - last_flush > std::chrono::seconds(2)) {
            std::fflush(stdout);
            last_flush = now;
        }
        // Heartbeat (NWII_FREEZE_HEARTBEAT seconds, default 30; 0 = off): where the game is, so a "stuck but
        // still rendering" session (e.g. a loading or tutorial state that never ends) can be located in the log.
        if (heartbeat > 0 && now >= next_beat && ctx.is_running.load()) {
            const uint64_t fr = *(volatile uint64_t*)&g_stat_frames, in = *(volatile uint64_t*)&ctx.inst_count;
            std::ostringstream o;
            o << "[FREEZE8] heartbeat t=" << std::dec
              << std::chrono::duration_cast<std::chrono::seconds>(now - started).count() << "s frames=" << fr << " (+"
              << (fr - beat_frames) << ") guest_inst=+" << (in - beat_inst) << " interp_budget_exceeded="
              << g_interp_budget_exceeded << " pc=" << hex(*(volatile uint32_t*)&ctx.pc) << "\n";
            if (GameInfoFn gi = g_game_info.load()) o << gi(ctx);
            write_block(o.str());
            beat_frames = fr;
            beat_inst = in;
            next_beat = now + std::chrono::milliseconds((int64_t)(heartbeat * 1000));
        }
        if (!ctx.is_running.load()) {
            // Shutdown requested: the CPU thread must stop soon, else report the hang and end the process.
            if (shutdown_seen == clk::time_point{}) shutdown_seen = now;
            if (now - shutdown_seen > std::chrono::seconds(15)) {
                std::ostringstream o;
                o << "\n[FREEZE8] ===== SHUTDOWN HANG: CPU thread still running 15 s after shutdown was requested "
                     "=====\n"
                  << host_stacks() << "[FREEZE8] ===== end (terminating the process) =====\n";
                write_block(o.str());
#ifdef _WIN32
                TerminateProcess(GetCurrentProcess(), 3);
#else
                std::_Exit(3);
#endif
            }
            continue;
        }
        const uint64_t f = *(volatile uint64_t*)&g_stat_frames;
        if (f != last_frames) {
            if (reported) {
                std::ostringstream o;
                o << "[FREEZE8] frames resumed after "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(now - last_change).count() / 1000.0
                  << " s (frame " << f << ")\n";
                write_block(o.str());
            }
            last_frames = f;
            last_change = now;
            inst_at_change = *(volatile uint64_t*)&ctx.inst_count;
            armed = true;
            reported = false;
            continue;
        }
        if (!armed) continue; // boot: nothing to compare before the first frame
        const double stalled = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_change).count() / 1000.0;
        if (!reported && stalled >= seconds) {
            std::vector<Sample> recent;
            const size_t n = std::min<size_t>(filled, (size_t)(seconds * 50));
            for (size_t i = 0; i < n; ++i) recent.push_back(ring[(head + ring.size() - n + i) % ring.size()]);
            stall_start_inst = inst_at_change; // guest instruction count when the last frame appeared
            stall_start_budget = g_interp_budget_exceeded;
            std::ostringstream o;
            o << "\n[FREEZE8] ===== FREEZE: no new game frame for " << std::fixed << std::setprecision(1) << stalled
              << " s (last frame " << std::dec << f << ") =====\n"
              << "[FREEZE8] guest instructions during the stall: " << (ctx.inst_count - stall_start_inst)
              << (ctx.inst_count == stall_start_inst ? " -> CPU thread is NOT executing guest code (host wait/deadlock?)"
                                                     : " -> guest code is running (guest wait loop or no GX output?)")
              << "\n"
              << guest_block(ctx, recent) << host_stacks() << "[FREEZE8] ===== end of freeze report =====\n";
            write_block(o.str());
            reported = true;
            next_reminder = now + std::chrono::seconds(60);
        } else if (reported && now >= next_reminder) {
            std::vector<Sample> recent;
            const size_t n = std::min<size_t>(filled, 250);
            for (size_t i = 0; i < n; ++i) recent.push_back(ring[(head + ring.size() - n + i) % ring.size()]);
            std::map<uint32_t, int> hist;
            for (const auto& s : recent) ++hist[s.pc];
            auto top = std::max_element(hist.begin(), hist.end(),
                                        [](const auto& a, const auto& b) { return a.second < b.second; });
            std::ostringstream o;
            o << "[FREEZE8] still frozen: " << std::fixed << std::setprecision(0) << stalled << " s without a frame; "
              << std::dec << (ctx.inst_count - stall_start_inst) << " guest instructions since the report, interp "
              << "budget-exceeded +" << (g_interp_budget_exceeded - stall_start_budget) << "; hottest pc "
              << (top != hist.end() ? hex(top->first) + " " + describe(ctx, top->first) : std::string("?")) << "\n";
            write_block(o.str());
            next_reminder = now + std::chrono::seconds(60);
        }
    }
}

struct Installer {
    Installer() {
        g_interp_unsupported_hook = &on_unsupported; // loud report + message box, then the interpreter exits
        if (env_off("NWII_FREEZE_WATCHDOG")) return;
        double seconds = 8.0, heartbeat = 30.0;
        if (const char* s = std::getenv("NWII_FREEZE_SECONDS")) seconds = std::max(1.0, std::atof(s));
        if (const char* s = std::getenv("NWII_FREEZE_HEARTBEAT")) heartbeat = std::max(0.0, std::atof(s));
        std::atexit([] { g_stop.store(true); });
        std::thread(watchdog_main, seconds, heartbeat).detach();
    }
};
Installer g_installer; // static init (main thread) of this TU, linked in via note_cpu_thread()

} // namespace

void note_cpu_thread(CPUContext&) {
#ifdef _WIN32
    if (!g_cpu_tid.load()) {
        {
            std::lock_guard<std::mutex> lock(g_ctx_guard);
            g_cpu_alive = true;
        }
        static thread_local CpuThreadExit on_exit; // destroyed when the CPU thread ends
        (void)on_exit;
        g_cpu_tid.store(GetCurrentThreadId());
        std::printf("[FREEZE8] freeze watchdog %s (NWII_FREEZE_SECONDS=%s; NWII_FREEZE_WATCHDOG=0 disables)\n",
                    env_off("NWII_FREEZE_WATCHDOG") ? "off" : "on",
                    std::getenv("NWII_FREEZE_SECONDS") ? std::getenv("NWII_FREEZE_SECONDS") : "8");
        std::fflush(stdout);
    }
#endif
}

void set_game_info(GameInfoFn fn) { g_game_info.store(fn); }

// A runtime fatal stop ([SS fatal] in ss_boot.cpp): same location report + message box, then the caller exits.
void report_fatal(CPUContext& ctx, const std::string& reason) {
    stop_report(ctx, "FATAL STOP: " + reason, "[FREEZE8] pc " + hex(ctx.pc) + " " + describe(ctx, ctx.pc) + "\n",
                "De runtime stopte het spel: " + reason + "\n  op " + hex(ctx.pc) + ": " + describe(ctx, ctx.pc));
}

} // namespace freeze
} // namespace nwii::runtime
