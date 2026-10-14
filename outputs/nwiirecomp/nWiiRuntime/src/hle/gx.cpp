
#include <map>
#include "runtime/cpu_context.h"

namespace nwii::runtime { extern CPUContext *g_ctx_ptr; }

#include "runtime/gx/fifo_parser.h"
#include "runtime/gx/renderer.h"
#include "runtime/hw/hw.h"
#include "runtime/gx_state.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <atomic>
#include <iterator>
#include <chrono>
#include <cstring>
#include <cinttypes>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <glad/glad.h>
#include <condition_variable>
#include <fstream>
#include <thread>
#include "runtime/gx/gx_capture.h"
#include "runtime/gx/frame_pipeline.h"
#include "runtime/texture_cache_gl.h"

using namespace nwii::runtime;
using namespace nwii::runtime::gx;

GXState nwii::runtime::gx::g_state = {};

static std::unique_ptr<nwii::runtime::gx::IRenderer> g_renderer = nullptr;
static void* g_window = nullptr;

void GX_SetWindow(void* window) { g_window = window; }

void GX_Init() {
    g_renderer = nwii::runtime::gx::IRenderer::Create();
    if (g_renderer) g_renderer->Initialize(nullptr);
}

namespace nwii::runtime {
    extern MMU* g_mmu;
}

std::mutex g_fifo_mutex;
std::vector<uint8_t> g_hw_fifo;

// Display lists are read out of guest RAM when the FIFO is parsed, which is
// long after the game issued the call -- and the game double-buffers those
// buffers, so by then the bytes can already belong to the next frame. The
// GXCallDisplayList seam copies them at the moment of the call; the parser
// takes the copy when one exists.
static std::mutex g_dl_snap_mtx;
static std::map<uint64_t, std::vector<uint8_t>> g_dl_snap;

namespace nwii::runtime {
void GX_DL_Snapshot(uint32_t addr, uint32_t size) {
    if (!nwii::runtime::g_mmu || size == 0 || size > (8u << 20)) return;
    std::vector<uint8_t> buf(size);
    if (const uint8_t* p = nwii::runtime::g_mmu->get_ptr(0x80000000u | (addr & 0x1FFFFFFF), size))
        std::memcpy(buf.data(), p, size);
    else
        for (uint32_t i = 0; i < size; ++i) buf[i] = nwii::runtime::g_mmu->read8(addr + i);
    std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
    if (g_dl_snap.size() > 1024) g_dl_snap.clear();
    g_dl_snap[((uint64_t)(addr & 0x1FFFFFFF) << 24) ^ size] = std::move(buf);
}

bool GX_DL_Fetch(uint32_t addr, uint32_t size, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
    auto it = g_dl_snap.find(((uint64_t)(addr & 0x1FFFFFFF) << 24) ^ size);
    if (it == g_dl_snap.end() || it->second.size() != size) return false;
    out = it->second;
    return true;
}
} // namespace nwii::runtime


static std::vector<nwii::runtime::gx::GXCommand> s_carry;

uint64_t g_stat_frames = 0;
uint64_t g_stat_draws = 0;


uint64_t g_stat_parse_us = 0;
uint64_t g_stat_render_us = 0;

void GX_GetClearColor(unsigned char rgba[4]) {
    rgba[0] = (unsigned char)(g_state.clearAR & 0xFF);
    rgba[1] = (unsigned char)(g_state.clearGB >> 8);
    rgba[2] = (unsigned char)(g_state.clearGB & 0xFF);
    rgba[3] = (unsigned char)(g_state.clearAR >> 8);
}

void GX_GetXfb(uint32_t* addr, unsigned* w, unsigned* h, unsigned* stride) {
    *addr   = g_state.xfbAddr;
    *w      = g_state.xfbW;
    *h      = g_state.xfbH;
    *stride = g_state.xfbStride;
}

// FIFO bytes drained but not yet parsed (an incomplete trailing command).
static std::vector<uint8_t> s_parse_buf;
// PERF13: the WGP buffer swapped out by the last drain (guarded by s_parse_mtx).
static std::vector<uint8_t> s_drained;

// M0.4 tooling, defined at the end of this file. All are no-ops unless
// NWII_GXCAPTURE / NWII_GXFRAMELOG is set or gxreplay drives the FIFO.
static std::vector<uint8_t>* s_cap_chunk_sink = nullptr; // drained bytes, while recording
static void CapTickBegin();
static void CapTickEnd(uint64_t frame);
static void CapBeforeDecode(uint64_t frame);
static void CapAfterRender(uint64_t frame, size_t dropped_draws, uint64_t skipped);
// INTERP16 tooling hook (null by default; only the offline prototype in
// outputs/pc-tests/interp16 sets it): sees each cut frame before decode.
namespace nwii::runtime::gx {
void (*g_interpFrameHook)(std::vector<GXCommand>& frame, uint64_t frame_no) = nullptr;
}

// ---------------------------------------------------------------------------
// PERF2: FIFO parsing can also run on the CPU thread while the guest idles.
//
// The parser raises the PE DrawDone/token interrupts at the command's stream
// position (fifo_parser.cpp ParseBP). With parsing only in the host loop, a
// guest waiting in GXDrawDone slept until the next host-loop iteration, i.e.
// until the next SDL_GL_SwapWindow returned (~30-37 ms in the PERF2 baseline),
// and the game ran at host_iters/2..4 fps with the CPU idle 50-90%.
// Dolphin flushes the GPU when the CPU idles (CoreTiming::Idle ->
// Fifo::FlushGpu under idle skipping, and single-core mode runs the GPU in
// step with the CPU), so the commands the guest already wrote are processed
// before the idle guest waits any longer. GX_SyncOnIdle() does that: the poll
// calls it from a Dolphin-pattern idle loop (ios.cpp), and it drains and
// parses whatever the guest has written. Frame cutting, decode and GL
// rendering stay in the host loop (ProcessGXFifoImpl), unchanged.
//
// s_parse_mtx guards the parse-time state shared by both callers: s_parse_buf,
// s_carry, and the parser's own parse-time shadows (g_parse_*, CP/VAT/array
// state in g_state, the DL cache), none of which decode/render read. The CPU
// thread only try-locks it. Off with NWII_GX_IDLE_SYNC=0, and always off while
// an NWII_GXCAPTURE capture is configured (its chunks are per host tick).
// ---------------------------------------------------------------------------
static std::mutex s_parse_mtx;
static size_t s_scan_pos = 0;            // s_carry entries already scanned for frame ends
static std::vector<size_t> s_frame_ends; // their indices in s_carry (BP 0x52 bit 14)
// PERF13: s_fifo_pending lives in the parse hand-off (frame_pipeline.h ParseHandoff).
static nwii::runtime::gx::ParseHandoff s_handoff;
static std::atomic<bool>& s_fifo_pending = s_handoff.pending; // set per WGP byte, cleared by the drain
static std::atomic<bool> s_pw_on{false}; // PERF13 parse worker running (see PwStartOnce)
static const bool s_idle_sync_off = [] {
    const char* e = std::getenv("NWII_GX_IDLE_SYNC");
    return e && e[0] == '0';
}();
static bool CapConfigured();
uint64_t g_stat_xfb_copies = 0; // guest EFB->XFB copies seen by the frame cutter (PERF2)
uint64_t g_stat_idle_parses = 0; // GX_SyncOnIdle calls that parsed something

// ---------------------------------------------------------------------------
// GXF12: every guest frame is cut, decoded and rendered on its own.
//
// Before GXF12 each ProcessGXFifo call cut at the LAST completed frame in the
// carry and dropped the draws of every older one. The host loop ran GX
// (~36 ms per Skyloft frame) and a blocking vsync swap (~30 ms here) in
// series, so two guest frames were usually complete per call: the game ran at
// 30 fps and the window showed 14-16 (GXF12.md). Now presentation runs on its
// own thread (main.cpp), the host loop is woken as soon as the CPU thread's
// idle parse completes a frame (GX_WaitWork), and decode is parallel, so each
// frame is normally cut alone. Two completed frames in one cut now mean the
// host was busy for more than a frame period; only then is the older one
// merged (counted in g_stat_frames_merged, never silent).
// CutPolicy::PerFrame (frame_pipeline.h) cuts the OLDEST completed frame and
// lets NWII_GX_BACKLOG further completed frames wait. The default backlog 0
// cuts the newest, as the old cutter did: indexed vertex arrays are read from
// guest RAM at decode time, and a frame decoded while the guest already
// writes the frame after next can see its double-buffered arrays overwritten.
// NWII_GX_BACKLOG=1 trades that for showing every frame through a short host
// stall. NWII_GX_CUT=merge is the old cutter; gxreplay uses it for pre-GXF12
// captures (the manifest records the policy).
// ---------------------------------------------------------------------------
using nwii::runtime::gx::CutPolicy;
static CutPolicy s_cut_policy = [] {
    const char* e = std::getenv("NWII_GX_CUT");
    return e && std::strcmp(e, "merge") == 0 ? CutPolicy::Merge : CutPolicy::PerFrame;
}();
static int s_max_backlog = [] {
    const char* e = std::getenv("NWII_GX_BACKLOG");
    const int v = e && *e ? std::atoi(e) : 0;
    return v < 0 ? 0 : v > 8 ? 8 : v;
}();
uint64_t g_stat_frames_merged = 0; // guest frames whose draws a cut dropped (GXF12)
uint64_t g_stat_gxf12_cut_us = 0, g_stat_gxf12_decode_us = 0, g_stat_gxf12_render_us = 0,
         g_stat_gxf12_free_us = 0, g_stat_gxf12_lock_us = 0; // host time per stage
uint64_t g_stat_gxf12_backlog_max = 0; // most completed frames seen in the carry at once
uint64_t g_stat_gxf12_wakeups = 0;     // idle parses that completed a frame (host woken)
uint64_t g_stat_gxf12_lockwait_us = 0, g_stat_gxf12_hostparse_us = 0; // host side
std::atomic<uint64_t> g_stat_gxf12_idleparse_us{0};                    // CPU thread (idle)
// Last idle parse (steady_clock ns), for the host drain policy below.
static std::atomic<int64_t> s_last_idle_parse_ns{0};
static int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
// GXF12: the host loop no longer waits in the swap, so it reached the FIFO
// first and parsed the guest's commands itself, serially with GX (~8 ms per
// Skyloft frame), work the CPU thread does for free while the guest idles.
// While idle parses keep coming (one within 20 ms), the host leaves the drain
// to them and only cuts; otherwise (guest CPU-bound, idle sync off, capture
// or replay) it drains as before. NWII_GXF12_HOST_DRAIN=1: always drain.
static bool HostShouldDrain() {
    static const bool always = [] {
        const char* e = std::getenv("NWII_GXF12_HOST_DRAIN");
        return e && *e == '1';
    }();
    if (always || s_idle_sync_off || CapConfigured()) return true;
    // PERF13: with the parse worker running it drains (within ~1 ms, or per
    // kPwKickBytes), so the host only cuts.
    if (s_pw_on.load(std::memory_order_relaxed)) return false;
    const int64_t last = s_last_idle_parse_ns.load(std::memory_order_relaxed);
    return last == 0 || SteadyNs() - last > 20000000;
}

// GXF12: a cut frame's decoded vertices (~45 MB in Skyloft) are freed on a
// background thread instead of the host loop (~2.5 ms per frame).
// PERF13: the emptied frame vectors (their elements destroyed here) are kept
// as spares (at most 2), and the cut hands one back to the carry: the carry
// keeps a frame's capacity instead of growing from ~4k commands every frame
// (reallocation moves were ~12% of the CPU thread's idle parse in Skyloft).
struct GxFrameFreer {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::vector<nwii::runtime::gx::GXCommand>> q;
    std::vector<std::vector<nwii::runtime::gx::GXCommand>> spares; // empty, with capacity
    GxFrameFreer() {
        std::thread([this] {
            for (;;) {
                std::vector<std::vector<nwii::runtime::gx::GXCommand>> work;
                {
                    std::unique_lock<std::mutex> lk(m);
                    cv.wait(lk, [&] { return !q.empty(); });
                    work.swap(q);
                }
                for (auto& v : work) v.clear(); // the frees happen here
                std::lock_guard<std::mutex> lk(m);
                for (auto& v : work)
                    if (spares.size() < 2 && v.capacity() >= 4096) spares.push_back(std::move(v));
                // `work` (and any vector not kept) is released at scope end
            }
        }).detach();
    }
};
static GxFrameFreer* FrameFreer() {
    static GxFrameFreer* f = new GxFrameFreer(); // never destroyed (its thread outlives main)
    return f;
}
static void FreeFrameAsync(std::vector<nwii::runtime::gx::GXCommand>&& frame) {
    GxFrameFreer* f = FrameFreer();
    {
        std::lock_guard<std::mutex> lk(f->m);
        f->q.push_back(std::move(frame));
    }
    f->cv.notify_one();
}
// An empty vector with the largest spare capacity (or a fresh one).
static std::vector<nwii::runtime::gx::GXCommand> TakeSpareCarry() {
    GxFrameFreer* f = FrameFreer();
    std::lock_guard<std::mutex> lk(f->m);
    std::vector<nwii::runtime::gx::GXCommand> v;
    size_t best = f->spares.size();
    for (size_t i = 0; i < f->spares.size(); ++i)
        if (best == f->spares.size() || f->spares[i].capacity() > f->spares[best].capacity()) best = i;
    if (best != f->spares.size()) {
        v.swap(f->spares[best]);
        f->spares.erase(f->spares.begin() + (ptrdiff_t)best);
    }
    return v;
}
static const bool s_keep_carry_capacity = [] {
    const char* e = std::getenv("NWII_GX_CARRY_REUSE"); // PERF13 A/B: 0 = old growth
    return !(e && e[0] == '0');
}();

// Host-loop wake-up: the CPU thread's idle parse saw an EFB->XFB copy.
static std::mutex s_work_mtx;
static std::condition_variable s_work_cv;
static std::atomic<uint64_t> s_work_seq{0};
static void NotifyFrameWork() {
    {
        std::lock_guard<std::mutex> lk(s_work_mtx);
        s_work_seq.fetch_add(1, std::memory_order_relaxed);
    }
    s_work_cv.notify_one();
}
static bool HasXfbCopy(size_t from) {
    for (size_t i = from; i < s_carry.size(); ++i) {
        const auto& c = s_carry[i];
        if (c.type == nwii::runtime::gx::GXCommandType::BPRegister && c.reg == 0x52 &&
            (c.val & 0x4000))
            return true;
    }
    return false;
}

// Drain the WGP bytes and parse them into s_carry. Caller holds s_parse_mtx.
static void DrainAndParseLocked() {
    // PERF13: swap the WGP buffer out under the FIFO lock (O(1), it keeps its
    // capacity) and copy it outside, so the CPU thread's WGP writes wait less.
    std::vector<uint8_t>& drained = s_drained;
    {
        std::lock_guard<std::mutex> lock(g_fifo_mutex);
        drained.swap(g_hw_fifo); // g_hw_fifo gets the previous, cleared buffer
        // release: a reader that sees this clear also sees the PERF13 worker's
        // busy flag (set before this drain), see ParseHandoff::IdleSync.
        s_fifo_pending.store(false, std::memory_order_release);
    }
    if (!drained.empty()) {
        s_parse_buf.insert(s_parse_buf.end(), drained.begin(), drained.end());
        if (s_cap_chunk_sink)
            s_cap_chunk_sink->insert(s_cap_chunk_sink->end(), drained.begin(), drained.end());
        drained.clear();
    }
    if (s_parse_buf.empty()) return;

    auto t0 = std::chrono::steady_clock::now();
    // PERF13: parse straight onto the carry. The parser only appends, so this
    // is the same sequence as parsing into a fresh vector and moving it over,
    // without growing that vector from empty and moving every command twice
    // (~9% of the CPU thread in Skyloft was GXCommand moves).
    nwii::runtime::gx::FifoParser::Parse(s_parse_buf, s_carry);
    auto t1 = std::chrono::steady_clock::now();
    g_stat_parse_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    if (s_parse_buf.size() > (4u << 20))
        s_parse_buf.clear();
}

// ---------------------------------------------------------------------------
// PERF13: FIFO parse worker.
//
// GX_SyncOnIdle parsed the whole frame's FIFO on the CPU thread once the guest
// went idle: ~11 ms of every Skyloft frame (F000 outdoor 9.4% real idle left
// on the CPU thread, GXF12 r4), serial with the guest's own ~20 ms, so busy
// moments dropped to 26 fps. A worker now parses the bytes as they arrive
// (woken every kPwKickBytes written, or after 1 ms), overlapping the guest.
// Guest-visible semantics are kept:
//  - When the guest idles, GX_SyncOnIdle still makes sure EVERY byte written
//    so far is parsed (and its PE DrawDone/token interrupts raised) before the
//    idle skip may advance the clock: it waits for the worker's chunk in
//    flight and parses any rest itself, as before (ParseHandoff in
//    frame_pipeline.h, unit tested under stress in outputs/pc-tests/perf13).
//  - The worker may raise a PE interrupt while the guest still runs, as the
//    host drain already did and as Dolphin's GPU thread does (dual core).
//  - The parser's input, state and output are unchanged: the same bytes are
//    parsed in the same order under s_parse_mtx, only in other chunks (the
//    parser keeps an incomplete trailing command for the next chunk).
// Started at the first live WGP data (never in gxreplay, which fills
// g_hw_fifo directly, nor with a capture configured or NWII_GX_IDLE_SYNC=0).
// NWII_GX_PARSE_WORKER=0 restores the CPU-thread idle parse.
// ---------------------------------------------------------------------------
static constexpr size_t kPwKickBytes = 4096;
static const bool s_pw_wanted = [] {
    const char* e = std::getenv("NWII_GX_PARSE_WORKER");
    return !(e && e[0] == '0');
}();
static std::atomic<bool> s_pw_stop{false};  // process exit: the worker stops parsing
std::atomic<uint64_t> g_stat_perf13_worker_us{0}, g_stat_perf13_worker_parses{0},
    g_stat_perf13_idle_waits{0};
struct PwSignal { // never destroyed: the detached worker may wait on it at exit
    std::mutex m;
    std::condition_variable cv;
    bool kick = false;
};
static PwSignal* PwSig() {
    static PwSignal* s = new PwSignal();
    return s;
}
static void ParseWorkerMain() {
    PwSignal* sig = PwSig();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(sig->m);
            sig->cv.wait_for(lk, std::chrono::milliseconds(1), [&] { return sig->kick; });
            sig->kick = false;
        }
        bool frame_done = false;
        const int64_t t0 = SteadyNs();
        const bool ran = s_fifo_pending.load(std::memory_order_acquire);
        s_handoff.WorkerRound(s_parse_mtx, [&] {
            const size_t before = s_carry.size();
            DrainAndParseLocked();
            const bool grew = s_carry.size() != before;
            frame_done = grew && HasXfbCopy(before);
            return grew;
        }, &s_pw_stop);
        if (s_pw_stop.load(std::memory_order_acquire)) return;
        if (!ran) continue;
        g_stat_perf13_worker_us.fetch_add((uint64_t)((SteadyNs() - t0) / 1000), std::memory_order_relaxed);
        g_stat_perf13_worker_parses.fetch_add(1, std::memory_order_relaxed);
        if (frame_done) { // GXF12: wake the host loop for the new frame
            ++g_stat_gxf12_wakeups;
            NotifyFrameWork();
        }
    }
}
// Stops the parse worker for good: once this returns it never parses again (a
// parse in progress has finished: the stop is set under s_parse_mtx, which
// WorkerRound re-checks). Bounded: never hangs on a thread that stopped
// holding the lock. UIT14: main() calls it right after joining the CPU thread,
// BEFORE the CPUContext (guest RAM: the parser reads display lists and indexed
// loads through g_mmu) is freed; before that only the static destructors below
// stopped the worker, i.e. after ctx was gone (exit 0xC0000005 at teardown).
void GX_StopParseWorker() {
    const bool tail = s_handoff.pending.load(std::memory_order_acquire);
    const bool in_flight = s_handoff.busy.load(std::memory_order_acquire);
    const auto t0 = std::chrono::steady_clock::now();
    nwii::runtime::gx::ParseHandoff::StopWorker(s_parse_mtx, s_pw_stop);
    // NWII_CRASH_REPORT=1 (diagnostic, first call only): was the worker still busy when the guest RAM would have
    // been freed? (a FIFO tail not parsed yet, or a parse in flight = the window of the exit 0xC0000005)
    static std::atomic<bool> reported{false};
    static const bool report = [] { const char* e = std::getenv("NWII_CRASH_REPORT"); return e && e[0] == '1'; }();
    if (report && s_pw_on.load(std::memory_order_acquire) && !reported.exchange(true)) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[UIT14] exit: parse worker stopped (FIFO tail pending=" << tail << ", parse in flight="
                  << in_flight << ", waited " << ms << " ms)" << std::endl;
    }
}
// Exit: static destructors (s_carry, s_parse_buf) must not run under a parse.
// Declared after them, so destroyed before them.
static struct PwExitGuard {
    ~PwExitGuard() { GX_StopParseWorker(); }
} s_pw_exit_guard;
static void PwStartOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!s_pw_wanted || s_idle_sync_off || CapConfigured()) return;
        std::thread(ParseWorkerMain).detach();
        s_pw_on.store(true, std::memory_order_release);
        std::cout << "[PERF13] FIFO parse worker on (NWII_GX_PARSE_WORKER=0 disables)\n";
    });
}
// CPU thread, after a WGP write grew the undrained FIFO from `before` bytes
// to `after`: wake the worker each kPwKickBytes.
static inline void PwAfterWrite(size_t before, size_t after) {
    if (!s_pw_wanted || before / kPwKickBytes == after / kPwKickBytes) return;
    PwStartOnce();
    PwSignal* sig = PwSig();
    {
        std::lock_guard<std::mutex> lk(sig->m);
        sig->kick = true;
    }
    sig->cv.notify_one();
}
// GX_SyncOnIdle with the worker on (CPU thread, guest idle).
static bool PwSyncOnIdle() {
    static uint64_t seen_gen = 0; // CPU thread only
    bool frame_done = false, waited = false;
    // No clock read on the common path (nothing pending or in flight): the
    // idle loop calls this at every idle iteration.
    const bool maybe = s_handoff.pending.load(std::memory_order_relaxed) ||
                       s_handoff.busy.load(std::memory_order_relaxed);
    const int64_t t0 = maybe ? SteadyNs() : 0;
    const bool parsed = s_handoff.IdleSync(s_parse_mtx, [&] {
        const size_t before = s_carry.size();
        DrainAndParseLocked();
        const bool grew = s_carry.size() != before;
        frame_done = grew && HasXfbCopy(before);
        return grew;
    }, seen_gen, &waited);
    if (waited && t0) { // lock wait (a chunk in flight) + the CPU thread's own parse
        g_stat_gxf12_idleparse_us.fetch_add((uint64_t)((SteadyNs() - t0) / 1000), std::memory_order_relaxed);
        g_stat_perf13_idle_waits.fetch_add(1, std::memory_order_relaxed);
    }
    if (frame_done) {
        ++g_stat_gxf12_wakeups;
        NotifyFrameWork();
    }
    if (parsed) ++g_stat_idle_parses;
    return parsed;
}

namespace nwii::runtime {
// CPU thread, guest idle. Returns true if new commands were parsed.
bool GX_SyncOnIdle() {
    if (s_pw_on.load(std::memory_order_acquire) && !s_idle_sync_off && !CapConfigured())
        return PwSyncOnIdle();
    if (s_idle_sync_off || !s_fifo_pending.load(std::memory_order_relaxed))
        return false;
    if (CapConfigured()) return false;
    std::unique_lock<std::mutex> lk(s_parse_mtx, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    const size_t before = s_carry.size();
    const int64_t t0 = SteadyNs();
    DrainAndParseLocked();
    const int64_t t1 = SteadyNs();
    g_stat_gxf12_idleparse_us.fetch_add((uint64_t)((t1 - t0) / 1000), std::memory_order_relaxed);
    s_last_idle_parse_ns.store(t1, std::memory_order_relaxed);
    const bool parsed = s_carry.size() != before;
    if (parsed) ++g_stat_idle_parses;
    const bool frame_done = parsed && HasXfbCopy(before);
    lk.unlock();
    if (frame_done) { // GXF12: wake the host loop for the new frame
        ++g_stat_gxf12_wakeups;
        NotifyFrameWork();
    }
    return parsed;
}
} // namespace nwii::runtime

// GXF12, host thread. Completed guest frames still waiting in the carry after
// the last ProcessGXFifo call (the host loop calls again at once while > 0).
int GX_PendingFrames() { return (int)s_frame_ends.size(); }
// GXF12, host thread. Wake-up sequence; read it BEFORE ProcessGXFifo and pass
// it to GX_WaitWork, so a frame finished meanwhile is not slept through.
uint64_t GX_WorkSeq() { return s_work_seq.load(std::memory_order_relaxed); }
// Waits until an idle parse completes a frame (sequence != seen) or timeout.
bool GX_WaitWork(uint64_t seen, int timeout_ms) {
    std::unique_lock<std::mutex> lk(s_work_mtx);
    return s_work_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
        return s_work_seq.load(std::memory_order_relaxed) != seen;
    });
}

// PERF13: [PERF13] line every 5 s with NWII_GXF12_STATS=1 or NWII_PROFILE
// (host thread): where the FIFO parse ran, per guest frame (XFB copy).
static void Perf13MaybeReport() {
    static const bool on = std::getenv("NWII_GXF12_STATS") || std::getenv("NWII_PROFILE");
    if (!on) return;
    static int64_t t0 = SteadyNs();
    static uint64_t a_xfb = 0, a_wus = 0, a_wn = 0, a_iw = 0, a_ius = 0;
    const int64_t now = SteadyNs();
    if (now - t0 < 5000000000LL) return;
    const double dt = (double)(now - t0) / 1e9;
    t0 = now;
    const uint64_t xfb = g_stat_xfb_copies;
    const uint64_t wus = g_stat_perf13_worker_us.load(std::memory_order_relaxed);
    const uint64_t wn = g_stat_perf13_worker_parses.load(std::memory_order_relaxed);
    const uint64_t iw = g_stat_perf13_idle_waits.load(std::memory_order_relaxed);
    const uint64_t ius = g_stat_gxf12_idleparse_us.load(std::memory_order_relaxed);
    const double nf = xfb > a_xfb ? (double)(xfb - a_xfb) : 1.0;
    std::printf("[PERF13] worker=%s ms/frame worker_parse=%.2f idle_parse=%.2f | worker chunks/frame=%.1f "
                "idle_waits/s=%.0f\n",
                s_pw_on.load() ? "on" : "off", (wus - a_wus) / 1000.0 / nf, (ius - a_ius) / 1000.0 / nf,
                (wn - a_wn) / nf, (iw - a_iw) / dt);
    std::fflush(stdout);
    a_xfb = xfb; a_wus = wus; a_wn = wn; a_iw = iw; a_ius = ius;
}

// INTERP17: frame interpolation pipeline (60 fps through in-between frames; off by default).
#include "gx/interp_live.h" // (relative to src/hle: gxreplay has no src include path)

// Returns the STAT frame number this call cut (0 if it cut none).
static uint64_t ProcessGXFifoImpl() {
    Perf13MaybeReport();
    const auto lock_t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> parse_lock(s_parse_mtx);
    const auto lock_t1 = std::chrono::steady_clock::now();
    if (HostShouldDrain())
        DrainAndParseLocked();
    const auto lock_t2 = std::chrono::steady_clock::now();
    // GXF12 lock_us = lock wait + the host's own drain/parse (split below).
    g_stat_gxf12_lock_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        lock_t2 - lock_t0).count();
    g_stat_gxf12_lockwait_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        lock_t1 - lock_t0).count();
    g_stat_gxf12_hostparse_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        lock_t2 - lock_t1).count();

    static const bool s_sample = std::getenv("NWII_SAMPLE") != nullptr;
    if (s_sample && !s_parse_buf.empty()) {
        static size_t last_size = 0;
        static int stuck_ticks = 0, dumps = 0;
        if (s_parse_buf.size() == last_size) {
            if (++stuck_ticks == 60 && dumps < 8) {
                dumps++;
                stuck_ticks = 0;
                std::cout << "[GX] stalled remainder " << std::dec
                          << s_parse_buf.size() << " bytes, head:" << std::hex;
                for (size_t i = 0; i < s_parse_buf.size() && i < 24; i++)
                    std::cout << " " << (unsigned)s_parse_buf[i];
                std::cout << std::dec << "\n";
            }
        } else {
            last_size = s_parse_buf.size();
            stuck_ticks = 0;
        }
    }


    // "sometimes only Press any button" frames. Dolphin has no such boundary


    // Frame ends (EFB->XFB copies) are found incrementally: only commands
    // added since the last call are scanned (PERF2; the host loop now calls
    // this hundreds of times per second, and a full rescan of s_carry each
    // time cost ~50% of the host thread).
    if (s_scan_pos > s_carry.size()) { s_scan_pos = 0; s_frame_ends.clear(); }
    for (; s_scan_pos < s_carry.size(); ++s_scan_pos) {
        const auto &c = s_carry[s_scan_pos];
        if (c.type == nwii::runtime::gx::GXCommandType::BPRegister &&
            c.reg == 0x52 && (c.val & 0x4000)) {
            s_frame_ends.push_back(s_scan_pos);
            ++g_stat_xfb_copies;
        }
    }
    if (s_frame_ends.empty())
        return 0;
    const auto cut_t0 = std::chrono::steady_clock::now();
    if (s_frame_ends.size() > g_stat_gxf12_backlog_max)
        g_stat_gxf12_backlog_max = s_frame_ends.size();
    // GXF12: PerFrame cuts the oldest completed frame; Merge (old cutter,
    // NWII_GX_CUT=merge, pre-GXF12 captures) cuts the newest and drops the
    // older frames' draws. See frame_pipeline.h PlanCut.
    const nwii::runtime::gx::CutPlan plan =
        nwii::runtime::gx::PlanCut(s_frame_ends, s_cut_policy, s_max_backlog);
    g_stat_frames_merged += plan.merged;
    // Take [0, cut end] out of the carry (later frames stay), dropping the
    // draws of merged frames (dropped_draws).
    size_t dropped_draws = 0;
    std::vector<nwii::runtime::gx::GXCommand> frame = nwii::runtime::gx::TakeCut(
        s_carry, s_frame_ends, s_scan_pos, plan,
        [](const nwii::runtime::gx::GXCommand& c) {
            return c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive;
        },
        &dropped_draws);
    if (s_keep_carry_capacity) {
        // PERF13: the next frame is about as large as this one. Give the carry
        // that capacity now (a recycled buffer), so the parser never regrows it.
        const size_t want = frame.size() + frame.size() / 4 + 4096;
        if (s_carry.capacity() < want) {
            std::vector<nwii::runtime::gx::GXCommand> big = TakeSpareCarry();
            if (big.capacity() < want) big.reserve(want);
            big.insert(big.end(), std::make_move_iterator(s_carry.begin()),
                       std::make_move_iterator(s_carry.end()));
            s_carry.swap(big); // `big` (the small tail buffer) is freed here
        }
    }
    parse_lock.unlock(); // decode and render use only `frame` and render-time state
    g_stat_gxf12_cut_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - cut_t0).count();

    // NWII_FRAMESIG: per-frame signature (draw count + TEV colour/konst regs
    // + alpha test). If the WARNING "press" blink exists in our command
    // stream, some field here must alternate between frames.
    static const bool framesig = std::getenv("NWII_FRAMESIG") != nullptr;
    if (framesig && g_stat_frames < 240) {
        size_t draws = 0, verts = 0;
        for (const auto &c : frame)
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
                draws++;
                verts += c.raw ? c.raw->count : c.vertices.size();
            }
        // Sampled hash of guest MEM1: distinguishes "game logic frozen" from
        // "logic running but the screen is legitimately static".
        uint32_t ramsig = 0;
        if (nwii::runtime::g_mmu) {
            for (uint32_t a = 0x80003000; a < 0x81800000; a += 4096)
                ramsig = ramsig * 33 + nwii::runtime::g_mmu->read32(a);
        }
        // Hash the actual draw payloads and XF matrix payloads: counts alone
        // can't distinguish "static screen" from "animation we drop".
        uint32_t drawsig = 0, xfsig = 0;
        for (const auto &c : frame) {
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive && c.raw)
                for (size_t i = 0; i < c.raw->bytes.size(); i += 4)
                    drawsig = drawsig * 33 + c.raw->bytes[i];
            if (c.type == nwii::runtime::gx::GXCommandType::XFRegister)
                for (float f : c.payload) {
                    uint32_t u; std::memcpy(&u, &f, 4);
                    xfsig = xfsig * 33 + u;
                }
        }
        uint32_t tevsig = 0;
        for (int r = 0xE0; r <= 0xE7; r++) tevsig = tevsig * 33 + g_state.bp[r];
        std::cout << "[FRAMESIG] f=" << g_stat_frames << " draws=" << draws
                  << " verts=" << verts << " tev=" << std::hex << tevsig
                  << " draw=" << drawsig << " xf=" << xfsig
                  << " ram=" << ramsig
                  << " alpha=" << g_state.bp[0xF3] << " cmode=" << g_state.bp[0x41]
                  << std::dec << "\n";
    }

    ++g_stat_frames;
    if (FILE* fl = nwii::runtime::gx::FrameLogFile(nullptr)) {
        size_t nd = 0;
        for (const auto& c : frame)
            nd += c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive;
        std::fprintf(fl, "f=%" PRIu64 " FRAME cmds=%zu draws=%zu dropped_draws=%zu carry_left=%zu\n",
                     g_stat_frames, frame.size(), nd, dropped_draws, s_carry.size());
    }
    CapBeforeDecode(g_stat_frames);
    if (nwii::runtime::gx::g_interpFrameHook) // INTERP16 tooling hook (never set in the game)
        nwii::runtime::gx::g_interpFrameHook(frame, g_stat_frames);
    // INTERP17: record + plan of the in-between frame start on the interpolation worker (off: no-op)
    interp17::Pipeline::Get().OnCut(frame, g_stat_frames, g_window != nullptr, &FreeFrameAsync);
    const uint64_t skipped_before = nwii::runtime::gx::RendererSkippedDraws();
    // Decode only the surviving frame's draws (stale frames' draws were
    // dropped above and their vertices are never built).
    // GXF12: the draws are decoded by a small worker pool (DecodeDraw touches
    // only its own command and reads guest RAM; NWII_GX_DECODE_THREADS=0 or
    // NWII_SKINDBG decode serially), and not at all without a renderer
    // (headless: nothing ever reads the vertices).
    auto d0 = std::chrono::steady_clock::now();
    {
        static std::vector<nwii::runtime::gx::GXCommand*> draws;
        draws.clear();
        for (auto &c : frame)
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive)
                draws.push_back(&c);
        g_stat_draws += draws.size();
        if (g_window) {
            static nwii::runtime::gx::ParallelFor* pool = [] {
                int n = (int)std::thread::hardware_concurrency() / 8;
                n = n < 1 ? 1 : n > 4 ? 4 : n;
                if (const char* e = std::getenv("NWII_GX_DECODE_THREADS"); e && *e)
                    n = std::atoi(e) - 1; // total decode threads, including this one
                if (std::getenv("NWII_SKINDBG")) n = 0; // its counters are not atomic
                n = n < 0 ? 0 : n > 15 ? 15 : n;
                // Never destroyed: the workers block on its condition variable
                // until the process exits.
                return new nwii::runtime::gx::ParallelFor(n);
            }();
            static uint64_t* interp_vh; // INTERP17: per-draw vertex hashes (null when off)
            interp_vh = interp17::Pipeline::Get().VhashSink();
            pool->Run(draws.size(), 16, [](size_t b, size_t e) {
                for (size_t i = b; i < e; ++i) {
                    nwii::runtime::gx::FifoParser::DecodeDraw(*draws[i]);
                    if (interp_vh)
                        interp_vh[i] = nwii::runtime::gx::interp::VertexHash(*draws[i]);
                }
            });
        }
    }
    auto d1 = std::chrono::steady_clock::now();
    g_stat_gxf12_decode_us +=
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(d1 - d0).count();
    uint64_t decode_us =
        std::chrono::duration_cast<std::chrono::microseconds>(d1 - d0).count();
    g_stat_parse_us += decode_us;

    // [GXPROF]: once a second, split the drain cost by cause. Prints in
    // headless too (the [STAT] line is window-only), so the 3D-scene
    // bottleneck can be measured without a display.
    {
        namespace fp = nwii::runtime::gx;
        extern uint64_t g_prof_decode_us_accum;
        g_prof_decode_us_accum += decode_us;
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= 2) {
            last = now;
            std::cout << "[GXPROF] parse+decode_ms=" << g_stat_parse_us / 1000
                      << " (decode_ms=" << g_prof_decode_us_accum / 1000 << ")"
                      << " draws_snap=" << fp::g_prof_draws
                      << " draw_bytes=" << fp::g_prof_draw_bytes
                      << " dl_calls=" << fp::g_prof_dl_calls
                      << " dl_bytes=" << fp::g_prof_dl_bytes
                      << " dl_ms=" << fp::g_prof_dl_us / 1000
                      << " snap_ms=" << fp::g_prof_snap_us / 1000
                      << " dl_loadindx=" << fp::g_prof_dl_loadindx
                      << " dl_hits=" << fp::g_prof_dl_hits
                      << " unknown=" << fp::g_prof_unknown
                      << " carry=" << s_carry.size()
                      << " buf=" << s_parse_buf.size() << "\n";
            g_prof_decode_us_accum = 0;
            fp::g_prof_draws = 0;
            fp::g_prof_draw_bytes = 0;
            fp::g_prof_dl_calls = 0;
            fp::g_prof_dl_bytes = 0;
            fp::g_prof_unknown = 0;
            fp::g_prof_dl_us = 0;
            fp::g_prof_snap_us = 0;
            fp::g_prof_dl_loadindx = 0;
            fp::g_prof_dl_hits = 0;
        }
    }

    if (g_window) {
        if (!g_renderer) {
            g_renderer = nwii::runtime::gx::IRenderer::Create();
            g_renderer->Initialize(nullptr);
        }
        if (g_renderer) {
            interp17::Pipeline::Get().BeforeRender(*g_renderer, frame); // INTERP17: in-between frame
            auto r0 = std::chrono::steady_clock::now();
            g_renderer->Render(frame);
            auto r1 = std::chrono::steady_clock::now();
            g_stat_render_us +=
                std::chrono::duration_cast<std::chrono::microseconds>(r1 - r0)
                    .count();
            g_stat_gxf12_render_us +=
                (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(r1 - r0).count();
        }
    }
    CapAfterRender(g_stat_frames, dropped_draws,
                   nwii::runtime::gx::RendererSkippedDraws() - skipped_before);
    {
        const auto f0 = std::chrono::steady_clock::now();
        // decoded vertices, freed off the host loop (INTERP17: kept one frame longer while interpolating)
        interp17::Pipeline::Get().Keep(std::move(frame), &FreeFrameAsync);
        g_stat_gxf12_free_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - f0).count();
    }
    return g_stat_frames;
}

void ProcessGXFifo() {
    CapTickBegin();
    CapTickEnd(ProcessGXFifoImpl());
}

namespace nwii::runtime {

static inline void wgp_push(uint8_t b) {

    static const char* s_dump_path = std::getenv("NWII_GXDUMP");
    static const char* s_seg_path = std::getenv("NWII_WGSEG");
    if (s_dump_path) {
        static FILE* f = fopen(s_dump_path, "wb");
        static size_t n = 0;
        if (f && n < 4000000) {
            fputc(b, f);
            fflush(f);
            ++n;
        }
    }

    
    if (const char* segp = s_seg_path) {
        static FILE* sf = fopen(segp, "w");
        static uint64_t off = 0;
        static uint32_t last_pc = 0;
        extern CPUContext* g_ctx_ptr;
        uint32_t pc = g_ctx_ptr ? g_ctx_ptr->pc : 0;
        if (sf && pc != last_pc) {
            fprintf(sf, "%llx %x\n", (unsigned long long)off, pc);
            last_pc = pc;
        }
        ++off;
    }

    
    static const bool wgtrace = std::getenv("NWII_WGTRACE") != nullptr;
    if (wgtrace) {
        static int zeros = 0;
        static int logged = 0;
        static uint8_t prev = 0xFF;
        static int vcd_collect = 0;      
        static uint32_t vcd_val = 0;
        static uint8_t vcd_reg = 0;
        extern CPUContext* g_ctx_ptr;
        if (vcd_collect > 0) {
            vcd_val = (vcd_val << 8) | b;
            if (--vcd_collect == 0)
                std::cout << "[WGP] VCD 0x" << std::hex << (unsigned)vcd_reg
                          << " = 0x" << vcd_val << " lr=0x"
                          << (g_ctx_ptr ? g_ctx_ptr->lr : 0) << std::dec << "\n";
        } else if (prev == 0x08 && (b == 0x50 || b == 0x60) && g_ctx_ptr &&
                   (g_ctx_ptr->pc == 0x800bbbac)) {
            vcd_collect = 4; vcd_val = 0; vcd_reg = b;
        }
        prev = b;
        if (b == 0) {
            ++zeros;
        } else {
            if (zeros >= 24 && b >= 0x80 && logged < 16 && g_ctx_ptr) {
                std::cout << "[WGP] draw 0x" << std::hex << (unsigned)b
                          << " after " << std::dec << zeros << " zeros pc=0x"
                          << std::hex << g_ctx_ptr->pc << " lr=0x"
                          << g_ctx_ptr->lr << std::dec << "\n";
                ++logged;
            }
            zeros = 0;
        }
    }
    g_hw_fifo.push_back(b);
    s_fifo_pending.store(true, std::memory_order_relaxed);
}





// PERF13: the WGP debug taps (NWII_GXDUMP / NWII_WGSEG / NWII_WGTRACE) work
// per byte. Without them a write appends its bytes in one insert and sets
// `pending` once (wgp_push per byte was ~1.5% of the CPU thread in Skyloft).
// Same bytes, same order. Caller holds g_fifo_mutex.
static const bool s_wgp_taps = std::getenv("NWII_GXDUMP") || std::getenv("NWII_WGSEG") ||
                               std::getenv("NWII_WGTRACE");
static inline void wgp_append(const uint8_t* b, size_t n) {
    if (s_wgp_taps) {
        for (size_t i = 0; i < n; ++i) wgp_push(b[i]);
        return;
    }
    g_hw_fifo.insert(g_hw_fifo.end(), b, b + n);
    s_fifo_pending.store(true, std::memory_order_relaxed);
}

static bool wgp_redirect(uint8_t b) {
    uint32_t base, end, wptr;
    nwii::runtime::hw::pi_fifo_get(base, end, wptr);
    if (base == 0) return false; 
    uint32_t cp_base = nwii::runtime::hw::cp_fifo_base_reg();
    if ((base & 0x03FFFFFF) == (cp_base & 0x03FFFFFF))
        return false; 
    if (!nwii::runtime::g_mmu) return false;
    nwii::runtime::g_mmu->write8(0x80000000u | (wptr & 0x01FFFFFFu), b);
    wptr += 1;
    if ((wptr & 0x03FFFFFF) > (end & 0x03FFFFFF))
        wptr = base;
    nwii::runtime::hw::pi_fifo_set_wptr(wptr);
    return true;
}

void GX_WGPIPE_Write8(uint8_t val) {
    if (wgp_redirect(val)) return;
    size_t before, after; // PERF13: PwAfterWrite wakes the parse worker
    {
        std::lock_guard<std::mutex> lock(g_fifo_mutex);
        before = g_hw_fifo.size();
        wgp_append(&val, 1);
        after = g_hw_fifo.size();
    }
    PwAfterWrite(before, after);
}

void GX_WGPIPE_Write16(uint16_t val) {
    if (wgp_redirect(val >> 8)) { wgp_redirect(val & 0xFF); return; }
    size_t before, after;
    {
        std::lock_guard<std::mutex> lock(g_fifo_mutex);
        before = g_hw_fifo.size();
        const uint8_t b[2] = {(uint8_t)(val >> 8), (uint8_t)val};
        wgp_append(b, 2);
        after = g_hw_fifo.size();
    }
    PwAfterWrite(before, after);
}

void GX_WGPIPE_Write32(uint32_t val) {
    if (wgp_redirect(val >> 24)) {
        wgp_redirect((val >> 16) & 0xFF);
        wgp_redirect((val >> 8) & 0xFF);
        wgp_redirect(val & 0xFF);
        return;
    }
    static const bool s_sample32 = std::getenv("NWII_SAMPLE") != nullptr;
    if (s_sample32) {
        static uint64_t n = 0;
        ++n;
        if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 ||
            (n % 100000) == 0)
            std::cout << "[GX] WGP write32 #" << std::dec << n
                      << " fifo_bytes=" << g_hw_fifo.size() << "\n";
    }
    size_t before, after;
    {
        std::lock_guard<std::mutex> lock(g_fifo_mutex);
        before = g_hw_fifo.size();
        const uint8_t b[4] = {(uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8),
                              (uint8_t)val};
        wgp_append(b, 4);
        after = g_hw_fifo.size();
    }
    PwAfterWrite(before, after);
}

void GX_WGPIPE_WriteF32(float val) {
    union { float f; uint32_t i; } u; u.f = val;
    GX_WGPIPE_Write32(u.i);
}

void GX_WGPIPE_WriteF64(double val) {
    union { double d; uint64_t i; } u; u.d = val;
    if (wgp_redirect((u.i >> 56) & 0xFF)) {
        for (int s = 48; s >= 0; s -= 8)
            wgp_redirect((u.i >> s) & 0xFF);
        return;
    }
    size_t before, after;
    {
        std::lock_guard<std::mutex> lock(g_fifo_mutex);
        before = g_hw_fifo.size();
        uint8_t b[8];
        for (int k = 0; k < 8; ++k) b[k] = (uint8_t)(u.i >> (56 - 8 * k));
        wgp_append(b, 8);
        after = g_hw_fifo.size();
    }
    PwAfterWrite(before, after);
}

} 
uint64_t g_prof_decode_us_accum = 0;

// =============================================================================
// M0.2 / M0.4 tooling: BMP images, NWII_GXFRAMELOG, NWII_GXCAPTURE and the
// replay side used by outputs/pc-tests/gxreplay. Diagnostics only: nothing
// below changes what the game sees or what is rendered.
// =============================================================================

namespace nwii::runtime {
// Replay: make GX_DL_Fetch return exactly the bytes the live parser used.
static void GX_DL_Inject(uint32_t addr, const uint8_t* p, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
    if (g_dl_snap.size() > 1024) g_dl_snap.clear();
    g_dl_snap[((uint64_t)(addr & 0x1FFFFFFF) << 24) ^ size] =
        std::vector<uint8_t>(p, p + size);
}
} // namespace nwii::runtime

namespace nwii::runtime::gx {

// ---- BMP ---------------------------------------------------------------------
static void put16(uint8_t* p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool SaveBMP32(const std::string& path, const uint8_t* rgba, int w, int h, int mode) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    // BITMAPV4HEADER, BI_BITFIELDS, BGRA in memory -- the layout SDL_SaveBMP
    // wrote before, but alpha is now always 255 (M0.2: the EFB alpha channel
    // made viewers show transparent-black pixels as white).
    const uint32_t hdr = 14 + 108, img = (uint32_t)w * (uint32_t)h * 4;
    uint8_t fh[14] = {'B', 'M'};
    put32(fh + 2, hdr + img);
    put32(fh + 10, hdr);
    uint8_t ih[108] = {};
    put32(ih + 0, 108); put32(ih + 4, (uint32_t)w); put32(ih + 8, (uint32_t)h);
    put16(ih + 12, 1); put16(ih + 14, 32); put32(ih + 16, 3); put32(ih + 20, img);
    put32(ih + 24, 2835); put32(ih + 28, 2835);
    put32(ih + 40, 0x00FF0000u); put32(ih + 44, 0x0000FF00u);
    put32(ih + 48, 0x000000FFu); put32(ih + 52, 0xFF000000u);
    put32(ih + 56, 0x57696E20u); // 'Win '
    std::fwrite(fh, 1, sizeof fh, f);
    std::fwrite(ih, 1, sizeof ih, f);
    std::vector<uint8_t> row((size_t)w * 4);
    for (int y = 0; y < h; ++y) { // GL readback and BMP are both bottom-up
        const uint8_t* s = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            uint8_t r = s[x * 4], g = s[x * 4 + 1], b = s[x * 4 + 2];
            const uint8_t a = s[x * 4 + 3];
            if (mode == 1) r = g = b = a;
            row[x * 4] = b; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = 255;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

bool LoadBMP32(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
    const uint32_t off = get32(&d[10]);
    w = (int)get32(&d[18]);
    const int32_t hh = (int32_t)get32(&d[22]);
    const int bpp = d[28] | (d[29] << 8);
    const bool topdown = hh < 0;
    h = topdown ? -hh : hh;
    if ((bpp != 32 && bpp != 24) || w <= 0 || h <= 0) return false;
    const size_t stride = ((size_t)w * (bpp / 8) + 3) & ~(size_t)3;
    if (off + stride * h > d.size()) return false;
    rgba.assign((size_t)w * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = &d[off + stride * (topdown ? (h - 1 - y) : y)];
        uint8_t* o = &rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; ++x) {
            const uint8_t* px = s + x * (bpp / 8);
            o[x * 4] = px[2]; o[x * 4 + 1] = px[1]; o[x * 4 + 2] = px[0];
            o[x * 4 + 3] = bpp == 32 ? px[3] : 255;
        }
    }
    return true;
}

// ---- NWII_GXFRAMELOG -----------------------------------------------------------
namespace {
struct FrameLogCfg {
    bool on = false;
    bool cap = false; // NWII_GXFRAMELOG=cap: log exactly the NWII_GXCAPTURE frames
    uint64_t first = 0, count = 0;
    std::string path;
    FILE* f = nullptr;
};
FrameLogCfg& FL() {
    static FrameLogCfg c = [] {
        FrameLogCfg c;
        const char* e = std::getenv("NWII_GXFRAMELOG");
        if (!e || !*e) return c;
        if (std::strncmp(e, "cap", 3) == 0) { // into <capture dir>/live_gxframelog.txt
            c.on = c.cap = true;
            return c;
        }
        c.first = std::strtoull(e, nullptr, 10);
        const char* colon = std::strchr(e, ':');
        c.count = colon ? std::strtoull(colon + 1, nullptr, 10) : 1;
        if (!c.count) c.count = 1;
        if (const char* p = std::getenv("NWII_GXFRAMELOG_FILE"); p && *p)
            c.path = p;
        else if (const char* s = std::getenv("NWII_SCREENSHOT"); s && *s) {
            // Next to the screenshots, but outside probe.py's 'beeld-<stamp>*'
            // screenshot glob: <dir>/gxframelog-<prefix name>.txt
            const std::filesystem::path sp(s);
            c.path = (sp.parent_path() / ("gxframelog-" + sp.filename().string() + ".txt")).string();
        }
        else
            c.path = "gxframelog.txt";
        c.on = true;
        return c;
    }();
    return c;
}
FILE* CapFrameLog(uint64_t frame); // defined with the capture state below
} // namespace

FILE* FrameLogFile(uint64_t* frame) {
    FrameLogCfg& c = FL();
    if (!c.on) return nullptr;
    const uint64_t fr = ::g_stat_frames;
    if (c.cap) {
        FILE* f = CapFrameLog(fr);
        if (f && frame) *frame = fr;
        return f;
    }
    if (fr < c.first || fr >= c.first + c.count) return nullptr;
    if (!c.f) {
        c.f = std::fopen(c.path.c_str(), "w");
        if (!c.f) { c.on = false; return nullptr; }
        std::ostringstream os;
        os << "[GXFRAMELOG] frames " << c.first << ".." << (c.first + c.count - 1)
           << " -> " << c.path << "\n";
        std::cout << os.str();
    }
    if (frame) *frame = fr;
    return c.f;
}

// ---- capture file helpers --------------------------------------------------------
namespace {
constexpr uint32_t kMem1 = 24u << 20, kMem2 = 64u << 20, kPage = 4096;
constexpr uint32_t kCapVersion = 1;

struct WBuf {
    std::vector<uint8_t> b;
    void put(const void* p, size_t n) {
        const uint8_t* q = (const uint8_t*)p;
        b.insert(b.end(), q, q + n);
    }
    template <class T> void v(const T& x) { put(&x, sizeof x); }
    template <class T> void vec(const std::vector<T>& x) {
        v((uint32_t)x.size());
        if (!x.empty()) put(x.data(), x.size() * sizeof(T));
    }
};
struct RBuf {
    const uint8_t* p = nullptr;
    size_t n = 0, i = 0;
    bool ok = true;
    void get(void* d, size_t k) {
        if (!ok || i + k > n) { ok = false; std::memset(d, 0, k); return; }
        std::memcpy(d, p + i, k);
        i += k;
    }
    template <class T> T v() { T x; get(&x, sizeof x); return x; }
    template <class T> void vec(std::vector<T>& x) {
        const uint32_t c = v<uint32_t>();
        if (!ok || (size_t)c * sizeof(T) > n - i) { ok = false; return; }
        x.resize(c);
        if (c) get(x.data(), (size_t)c * sizeof(T));
    }
};

void PutCmd(WBuf& w, const GXCommand& c) {
    w.v((uint8_t)c.type); w.v(c.reg); w.v(c.val); w.v(c.length); w.v(c.prim_type);
    w.vec(c.payload);
    w.vec(c.vertices);
    w.v((uint8_t)(c.raw != nullptr));
    if (c.raw) {
        const DrawRaw& r = *c.raw;
        w.v(r.vat); w.v(r.arrayBase); w.v(r.arrayStride); w.v(r.defPosMtxIdx);
        w.v(r.defTexMtxIdx); w.v(r.need_normal); w.v(r.have_snap_mtx);
        w.v(r.snap_pos_mtx); w.v(r.snap_nrm_mtx);
        w.vec(r.snap_pos_bank); w.vec(r.snap_nrm_bank);
        w.v(r.count); w.vec(r.bytes);
    }
}
// Captures written before M1.8 have a VertexData without raw_pos/raw_norm
// (6 floats smaller). Their command lists carry raw draws only (vertices are
// decoded at replay), so they still load as long as every vertex list is
// empty; a non-empty one fails the read.
constexpr uint32_t kVertexDataPreM18 = (uint32_t)(sizeof(VertexData) - 6 * sizeof(float));
bool s_cap_vtx_legacy = false;
void GetCmd(RBuf& r, GXCommand& c) {
    c.type = (GXCommandType)r.v<uint8_t>();
    c.reg = r.v<uint16_t>(); c.val = r.v<uint32_t>(); c.length = r.v<uint16_t>();
    c.prim_type = r.v<uint8_t>();
    r.vec(c.payload);
    if (s_cap_vtx_legacy) {
        if (r.v<uint32_t>() != 0) r.ok = false;
        c.vertices.clear();
    } else {
        r.vec(c.vertices);
    }
    c.raw.reset();
    if (r.v<uint8_t>()) {
        auto d = std::make_shared<DrawRaw>();
        r.get(&d->vat, sizeof d->vat);
        r.get(d->arrayBase, sizeof d->arrayBase);
        r.get(d->arrayStride, sizeof d->arrayStride);
        d->defPosMtxIdx = r.v<uint8_t>();
        r.get(d->defTexMtxIdx, sizeof d->defTexMtxIdx);
        d->need_normal = r.v<bool>();
        d->have_snap_mtx = r.v<bool>();
        r.get(d->snap_pos_mtx, sizeof d->snap_pos_mtx);
        r.get(d->snap_nrm_mtx, sizeof d->snap_nrm_mtx);
        r.vec(d->snap_pos_bank);
        r.vec(d->snap_nrm_bank);
        d->count = r.v<uint16_t>();
        r.vec(d->bytes);
        c.raw = std::move(d);
    }
}
void PutCmds(WBuf& w, const std::vector<GXCommand>& v) {
    w.v((uint32_t)v.size());
    for (const auto& c : v) PutCmd(w, c);
}
void GetCmds(RBuf& r, std::vector<GXCommand>& v) {
    const uint32_t n = r.v<uint32_t>();
    v.clear();
    for (uint32_t i = 0; i < n && r.ok; ++i) { v.emplace_back(); GetCmd(r, v.back()); }
}
// Layout guard: a struct change makes old captures fail loudly instead of
// replaying garbage (and is the reminder to update PutCmd/GetCmd).
void PutLayout(WBuf& w) {
    w.v((uint32_t)sizeof(GXState)); w.v((uint32_t)sizeof(VATSlot));
    w.v((uint32_t)sizeof(VertexData)); w.v((uint32_t)sizeof(DrawRaw));
    w.v((uint32_t)sizeof(GXCommand));
}
bool CheckLayout(RBuf& r, std::string& err) {
    const uint32_t a = r.v<uint32_t>(), b = r.v<uint32_t>(), c = r.v<uint32_t>(),
                   d = r.v<uint32_t>(), e = r.v<uint32_t>();
    s_cap_vtx_legacy = (c == kVertexDataPreM18);
    if (a != sizeof(GXState) || b != sizeof(VATSlot) ||
        (c != sizeof(VertexData) && !s_cap_vtx_legacy) ||
        d != sizeof(DrawRaw) || e != sizeof(GXCommand)) {
        err = "struct layout differs from the capture (GXState/VATSlot/VertexData/DrawRaw/GXCommand)";
        return false;
    }
    return true;
}
bool WriteFile(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    return std::fclose(f) == 0 && ok;
}
bool ReadFile(const std::filesystem::path& p, std::vector<uint8_t>& b) {
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    b.resize(n > 0 ? (size_t)n : 0);
    const bool ok = b.empty() || std::fread(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    return ok;
}
std::string FrameName(const char* stem, uint64_t frame, const char* ext) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s_%06" PRIu64 "%s", stem, frame, ext);
    return buf;
}

// ---- live capture state --------------------------------------------------------
struct CapSpec {
    std::string name;
    bool watch = false;
    uint32_t waddr = 0, wval = 0, delay = 0;
    uint64_t first = 0; // 0 = not resolved yet (watch)
    uint32_t count = 1;
    uint32_t align = 0; // %N: shift so the last frame is a multiple of N
    int st = 0;         // 0 pending, 1 recording, 2 done
};
struct DLNote { uint32_t addr, size; std::vector<uint8_t> bytes; };
struct RAMNote { uint32_t addr; std::vector<uint8_t> bytes; };
struct CapState {
    bool cfg = false;
    std::vector<CapSpec> specs;
    std::filesystem::path base;
    int active = -1;
    std::filesystem::path dir;
    FILE* ticks = nullptr;
    uint32_t tick = 0;
    std::vector<uint8_t> chunk;
    std::vector<DLNote> dls;
    std::vector<RAMNote> rams;
    std::vector<uint8_t> prev_ram;  // last snapshot image, MEM1 then MEM2
    std::set<uint32_t> efb_dests;   // EFB->texture copy destinations seen
    std::ostringstream manifest;
    FILE* flog = nullptr;           // NWII_GXFRAMELOG=cap
};
CapState& CS();

FILE* CapFrameLog(uint64_t frame) {
    CapState& c = CS();
    if (c.active < 0 || !c.flog) return nullptr;
    const CapSpec& s = c.specs[c.active];
    return frame >= s.first && frame < s.first + s.count ? c.flog : nullptr;
}

void NoteDL(uint32_t addr, uint32_t size, const uint8_t* bytes) {
    CapState& c = CS();
    for (const auto& d : c.dls)
        if (d.addr == addr && d.size == size &&
            std::memcmp(d.bytes.data(), bytes, size) == 0)
            return;
    c.dls.push_back({addr, size, std::vector<uint8_t>(bytes, bytes + size)});
}
void NoteRAMFloats(uint32_t addr, const float* vals, int n) {
    if (n <= 0) return;
    RAMNote r{addr, std::vector<uint8_t>((size_t)n * 4)};
    for (int i = 0; i < n; ++i) { // guest RAM is big-endian
        uint32_t u;
        std::memcpy(&u, &vals[i], 4);
        r.bytes[i * 4] = u >> 24; r.bytes[i * 4 + 1] = (u >> 16) & 0xFF;
        r.bytes[i * 4 + 2] = (u >> 8) & 0xFF; r.bytes[i * 4 + 3] = u & 0xFF;
    }
    CS().rams.push_back(std::move(r));
}
void NoteEfbCopy(uint32_t dest) { CS().efb_dests.insert(dest); }
// %N: move the window later so its last frame is a multiple of N, i.e. a frame
// main.cpp also screenshots (NWII_SCREENSHOT_EVERY=N).
void AlignSpec(CapSpec& s) {
    if (!s.align || !s.first) return;
    const uint64_t last = s.first + s.count - 1;
    const uint64_t aligned = (last + s.align - 1) / s.align * s.align;
    s.first = aligned - s.count + 1;
}

CapState& CS() {
    static CapState* s = [] {
        auto* c = new CapState;
        const char* e = std::getenv("NWII_GXCAPTURE");
        if (!e || !*e) return c;
        const std::string all = e;
        size_t pos = 0;
        while (pos < all.size()) {
            size_t semi = all.find(';', pos);
            if (semi == std::string::npos) semi = all.size();
            std::string spec = all.substr(pos, semi - pos);
            pos = semi + 1;
            if (spec.empty()) continue;
            CapSpec s;
            if (size_t colon = spec.find(':'); colon != std::string::npos) {
                s.name = spec.substr(0, colon);
                spec = spec.substr(colon + 1);
            }
            if (size_t comma = spec.find(','); comma != std::string::npos) {
                s.count = (uint32_t)std::strtoul(spec.c_str() + comma + 1, nullptr, 10);
                spec = spec.substr(0, comma);
            }
            if (!s.count) s.count = 1;
            if (!spec.empty() && spec[0] == '@') {
                s.watch = true;
                char* end = nullptr;
                s.waddr = (uint32_t)std::strtoul(spec.c_str() + 1, &end, 16);
                if (end && *end == '=') s.wval = (uint32_t)std::strtoul(end + 1, &end, 16);
                if (end && *end == '+') s.delay = (uint32_t)std::strtoul(end + 1, &end, 10);
                if (end && *end == '%') s.align = (uint32_t)std::strtoul(end + 1, &end, 10);
            } else {
                char* end = nullptr;
                s.first = std::strtoull(spec.c_str(), &end, 10);
                if (end && *end == '%') s.align = (uint32_t)std::strtoul(end + 1, &end, 10);
                if (!s.first) continue;
                AlignSpec(s);
            }
            if (s.name.empty()) {
                char buf[48];
                if (s.watch) std::snprintf(buf, sizeof buf, "w%08X_%08X", s.waddr, s.wval);
                else std::snprintf(buf, sizeof buf, "f%06" PRIu64, s.first);
                s.name = buf;
            }
            c->specs.push_back(s);
        }
        if (c->specs.empty()) return c;
        if (const char* d = std::getenv("NWII_GXCAPTURE_DIR"); d && *d)
            c->base = d;
        else if (const char* sp = std::getenv("NWII_SCREENSHOT"); sp && *sp)
            c->base = std::filesystem::path(sp).parent_path() / "gxcapture";
        else
            c->base = "gxcapture";
        c->cfg = true;
        g_rendererEfbCopyHook = &NoteEfbCopy;
        std::ostringstream os;
        for (const auto& s : c->specs)
            os << "[GXCAPTURE] spec name=" << s.name
               << (s.watch ? " watch" : " frame=") << (s.watch ? std::string() : std::to_string(s.first))
               << " count=" << s.count << " -> " << (c->base / s.name).string() << "\n";
        std::cout << os.str();
        return c;
    }();
    return *s;
}

// Size of the colour attachment of the framebuffer bound to `target`
// (RENDER1: the EFB is 640x528 now; an old host FBO may still be 640x480).
void BoundFbSize(GLenum target, int& w, int& h) {
    w = kEfbWidth; h = kEfbHeight;
    GLint fb = 0;
    glGetIntegerv(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING
                                                : GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    if (!fb) return;
    GLint type = 0, name = 0, tw = 0, th = 0;
    glGetFramebufferAttachmentParameteriv(target, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    glGetFramebufferAttachmentParameteriv(target, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    if (type == GL_TEXTURE && name) {
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glBindTexture(GL_TEXTURE_2D, (GLuint)name);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    } else if (type == GL_RENDERBUFFER && name) {
        glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)name);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &tw);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &th);
    }
    if (tw > 0 && th > 0) { w = tw; h = th; }
}

// Readback of the whole currently bound read framebuffer (the EFB FBO in the
// live loop and in gxreplay), for the capture start state.
bool ReadbackEFB(std::vector<uint8_t>& px, int& w, int& h) {
    if (!g_window) return false;
    BoundFbSize(GL_READ_FRAMEBUFFER, w, h);
    px.resize((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return true;
}

// What the player sees (the last XFB copy, 640x480): live_<frame>.bmp.
bool ReadbackShown(std::vector<uint8_t>& px) {
    if (!g_window) return false;
    ReadPresented(px);
    return true;
}

struct DLVisit { WBuf* w; uint32_t n; };

void CapStart(int idx) {
    CapState& c = CS();
    CapSpec& s = c.specs[idx];
    s.st = 1;
    c.active = idx;
    c.dir = c.base / s.name;
    std::error_code ec;
    std::filesystem::create_directories(c.dir, ec);
    WBuf w;
    w.put("GXCS", 4);
    w.v(kCapVersion);
    PutLayout(w);
    w.v(::g_stat_frames);
    w.put(&g_state, sizeof g_state);
    float pos[256], nrm[96];
    ParserShadowGet(pos, nrm);
    w.put(pos, sizeof pos);
    w.put(nrm, sizeof nrm);
    // Parse-side DL cache: live cache hits re-emit commands parsed earlier, so
    // the replay needs the same entries to produce the same command stream.
    WBuf dlw;
    DLVisit vis{&dlw, 0};
    ParserDLCacheVisit(
        [](void* u, uint64_t key, uint64_t sh, uint64_t ch, const std::vector<GXCommand>& cmds) {
            auto* p = (DLVisit*)u;
            p->w->v(key); p->w->v(sh); p->w->v(ch);
            PutCmds(*p->w, cmds);
            ++p->n;
        },
        &vis);
    w.v(vis.n);
    w.put(dlw.b.data(), dlw.b.size());
    w.vec(s_parse_buf);
    PutCmds(w, s_carry);
    std::vector<uint8_t> efb;
    int efb_w = 0, efb_h = 0;
    if (ReadbackEFB(efb, efb_w, efb_h)) { w.v((uint32_t)efb_w); w.v((uint32_t)efb_h); w.put(efb.data(), efb.size()); }
    else { w.v((uint32_t)0); w.v((uint32_t)0); }
    // EFB->texture copies made before the capture window that the captured
    // frames may still sample.
    std::vector<std::pair<uint32_t, GLuint>> texs;
    if (g_window)
        for (uint32_t d : c.efb_dests)
            if (GLuint t = nwii::runtime::hle::TextureCache::get().find_efb_texture(d))
                texs.push_back({d, t});
    w.v((uint32_t)texs.size());
    if (!texs.empty()) {
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        for (auto& [d, t] : texs) {
            GLint tw = 0, th = 0;
            glBindTexture(GL_TEXTURE_2D, t);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
            std::vector<uint8_t> px((size_t)tw * th * 4);
            if (!px.empty()) glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            w.v(d); w.v((uint32_t)tw); w.v((uint32_t)th); w.put(px.data(), px.size());
        }
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    }
    WriteFile(c.dir / "start.bin", w.b);
    c.ticks = std::fopen((c.dir / "ticks.bin").string().c_str(), "wb");
    if (FL().cap) c.flog = std::fopen((c.dir / "live_gxframelog.txt").string().c_str(), "w");
    c.tick = 0;
    c.prev_ram.assign((size_t)kMem1 + kMem2, 0);
    c.manifest.str("");
    c.manifest << "version=" << kCapVersion << "\nname=" << s.name << "\nfirst_frame=" << s.first
               << "\ncount=" << s.count << "\nstart_stat_frames=" << ::g_stat_frames
               << "\nparse_buf_bytes=" << s_parse_buf.size() << "\ncarry_cmds=" << s_carry.size()
               << "\ndl_cache_entries=" << vis.n << "\nefb_textures=" << texs.size()
               << "\nefb_colour=" << (efb.empty() ? 0 : 1)
               // GXF12: how ProcessGXFifo cut frames; ReplayOpen reapplies it
               // (no line = a pre-GXF12 capture = merge).
               << "\ncut_policy=" << (s_cut_policy == CutPolicy::Merge ? "merge" : "perframe")
               << "\ncut_backlog=" << s_max_backlog << "\n";
    std::ostringstream os;
    os << "[GXCAPTURE] start " << s.name << " frames " << s.first << ".."
       << (s.first + s.count - 1) << " dir=" << c.dir.string() << "\n";
    std::cout << os.str();
}

void CapFinish() {
    CapState& c = CS();
    CapSpec& s = c.specs[c.active];
    if (c.ticks) std::fclose(c.ticks);
    c.ticks = nullptr;
    if (c.flog) std::fclose(c.flog);
    c.flog = nullptr;
    c.manifest << "ticks=" << c.tick << "\n";
    const std::string m = c.manifest.str();
    WriteFile(c.dir / "manifest.txt", std::vector<uint8_t>(m.begin(), m.end()));
    std::ostringstream os;
    os << "[GXCAPTURE] done " << s.name << " ticks=" << c.tick << "\n";
    std::cout << os.str();
    s.st = 2;
    c.active = -1;
    c.prev_ram.clear();
    c.prev_ram.shrink_to_fit();
}

// ---- replay state --------------------------------------------------------------
struct ReplayState {
    bool open = false;
    std::filesystem::path dir;
    std::vector<uint8_t> ticks;
    RBuf tr;
    std::vector<uint8_t> image; // RAM snapshot image (MEM1 then MEM2)
};
ReplayState& RS() { static ReplayState r; return r; }

void CopyImageToMMU(const std::vector<uint8_t>& img) {
    if (!g_mmu || img.size() < (size_t)kMem1 + kMem2) return;
    std::memcpy(g_mmu->mem1.data(), img.data(), kMem1);
    std::memcpy(g_mmu->mem2.data(), img.data() + kMem1, kMem2);
}
bool ApplyRamFile(const std::filesystem::path& p, std::vector<uint8_t>& img) {
    std::vector<uint8_t> b;
    if (!ReadFile(p, b)) return false;
    RBuf r{b.data(), b.size()};
    char magic[4];
    r.get(magic, 4);
    if (std::memcmp(magic, "RAMF", 4) != 0) return false;
    const uint32_t n = r.v<uint32_t>();
    for (uint32_t i = 0; i < n && r.ok; ++i) {
        const uint32_t pg = r.v<uint32_t>();
        if ((size_t)(pg + 1) * kPage > img.size()) return false;
        r.get(&img[(size_t)pg * kPage], kPage);
    }
    return r.ok;
}
} // namespace
} // namespace nwii::runtime::gx

// ---- hook points called from ProcessGXFifo ------------------------------------------
static void CapTickBegin() {
    using namespace nwii::runtime::gx;
    static const bool hooks = [] {
        g_rendererFrameLogHook = &FrameLogFile;
        return true;
    }();
    (void)hooks;
    CapState& c = CS();
    if (!c.cfg) return;
    if (c.active < 0) {
        for (size_t i = 0; i < c.specs.size(); ++i) {
            CapSpec& s = c.specs[i];
            if (s.st != 0 || !s.first) continue;
            if (::g_stat_frames + 1 == s.first) { CapStart((int)i); break; }
            if (::g_stat_frames + 1 > s.first) {
                s.st = 2;
                std::ostringstream os;
                os << "[GXCAPTURE] missed " << s.name << " (frame " << s.first << " already past)\n";
                std::cout << os.str();
            }
        }
    }
    if (c.active >= 0) {
        c.chunk.clear(); c.dls.clear(); c.rams.clear();
        s_cap_chunk_sink = &c.chunk;
        g_capNoteDL = &NoteDL;
        g_capNoteRAMFloats = &NoteRAMFloats;
    }
}

static bool CapConfigured() { return nwii::runtime::gx::CS().cfg; }

static void CapTickEnd(uint64_t frame) {
    using namespace nwii::runtime::gx;
    CapState& c = CS();
    if (!c.cfg) return;
    if (c.active >= 0) {
        s_cap_chunk_sink = nullptr;
        g_capNoteDL = nullptr;
        g_capNoteRAMFloats = nullptr;
        if (c.ticks && (!c.chunk.empty() || frame)) {
            WBuf w;
            w.put("TICK", 4);
            w.v(c.tick); w.v(frame);
            w.v((uint32_t)c.dls.size());
            for (const auto& d : c.dls) { w.v(d.addr); w.v(d.size); w.put(d.bytes.data(), d.size); }
            w.v((uint32_t)c.rams.size());
            for (const auto& r : c.rams) { w.v(r.addr); w.vec(r.bytes); }
            w.vec(c.chunk);
            std::fwrite(w.b.data(), 1, w.b.size(), c.ticks);
            ++c.tick;
        }
        const CapSpec& s = c.specs[c.active];
        if (frame && frame >= s.first + s.count - 1) CapFinish();
    }
    if (frame && nwii::runtime::g_mmu) {
        for (auto& s : c.specs) {
            if (s.st != 0 || !s.watch || s.first) continue;
            if (nwii::runtime::g_mmu->read32(s.waddr) == s.wval) {
                s.first = frame + 1 + s.delay;
                AlignSpec(s);
                std::ostringstream os;
                os << "[GXCAPTURE] watch hit " << s.name << " at frame " << frame
                   << " -> first=" << s.first << "\n";
                std::cout << os.str();
            }
        }
    }
}

static void CapBeforeDecode(uint64_t frame) {
    using namespace nwii::runtime::gx;
    ReplayState& rs = RS();
    if (rs.open) { // replay: render with the RAM image recorded for this frame
        if (ApplyRamFile(rs.dir / FrameName("ram", frame, ".bin"), rs.image))
            CopyImageToMMU(rs.image);
        return;
    }
    CapState& c = CS();
    if (!c.cfg || c.active < 0 || !nwii::runtime::g_mmu) return;
    // RAM as the renderer is about to read it (vertex arrays, textures, TLUT
    // loads): the pages that differ from the previous snapshot of this capture.
    auto& m = *nwii::runtime::g_mmu;
    std::vector<uint32_t> pages;
    for (uint32_t pg = 0; pg < (kMem1 + kMem2) / kPage; ++pg) {
        const uint8_t* src = pg < kMem1 / kPage ? &m.mem1[(size_t)pg * kPage]
                                                 : &m.mem2[(size_t)pg * kPage - kMem1];
        uint8_t* dst = &c.prev_ram[(size_t)pg * kPage];
        if (std::memcmp(src, dst, kPage) != 0) {
            std::memcpy(dst, src, kPage);
            pages.push_back(pg);
        }
    }
    FILE* f = std::fopen((c.dir / FrameName("ram", frame, ".bin")).string().c_str(), "wb");
    if (!f) return;
    const uint32_t n = (uint32_t)pages.size();
    std::fwrite("RAMF", 1, 4, f);
    std::fwrite(&n, 4, 1, f);
    for (uint32_t pg : pages) {
        std::fwrite(&pg, 4, 1, f);
        std::fwrite(&c.prev_ram[(size_t)pg * kPage], 1, kPage, f);
    }
    std::fclose(f);
    c.manifest << "ram_" << frame << "_pages=" << n << "\n";
}

static void CapAfterRender(uint64_t frame, size_t dropped_draws, uint64_t skipped) {
    using namespace nwii::runtime::gx;
    if (FILE* fl = FrameLogFile(nullptr)) {
        std::fprintf(fl, "f=%" PRIu64 " ENDFRAME dropped_draws=%zu shader_skipped_draws=%" PRIu64 "\n",
                     frame, dropped_draws, skipped);
        std::fflush(fl);
    }
    CapState& c = CS();
    if (RS().open || !c.cfg || c.active < 0) return;
    c.manifest << "frame=" << frame << " dropped_draws=" << dropped_draws
               << " shader_skipped_draws=" << skipped << "\n";
    std::vector<uint8_t> px;
    if (ReadbackShown(px)) {
        SaveBMP32((c.dir / FrameName("live", frame, ".bmp")).string(), px.data(), kPresentWidth, kPresentHeight, 0);
        SaveBMP32((c.dir / FrameName("live", frame, "_alpha.bmp")).string(), px.data(), kPresentWidth, kPresentHeight, 1);
    }
}

namespace nwii::runtime::gx {

// ---- replay API --------------------------------------------------------------------
bool ReplayOpen(const std::string& dir_s, std::string& err) {
    ReplayState& rs = RS();
    rs = ReplayState{};
    rs.dir = dir_s;
    std::vector<uint8_t> b;
    if (!ReadFile(rs.dir / "start.bin", b)) { err = "cannot read start.bin"; return false; }
    RBuf r{b.data(), b.size()};
    char magic[4];
    r.get(magic, 4);
    if (std::memcmp(magic, "GXCS", 4) != 0 || r.v<uint32_t>() != kCapVersion) {
        err = "start.bin: bad magic/version";
        return false;
    }
    if (!CheckLayout(r, err)) return false;
    {
        // GXF12: cut frames the way the live host did (ticks.bin records the
        // frame each tick cut). Captures without the line predate GXF12.
        s_cut_policy = CutPolicy::Merge;
        std::ifstream mf(rs.dir / "manifest.txt");
        std::string line;
        while (std::getline(mf, line)) {
            if (line == "cut_policy=perframe") s_cut_policy = CutPolicy::PerFrame;
            else if (line.rfind("cut_backlog=", 0) == 0) s_max_backlog = std::atoi(line.c_str() + 12);
        }
        std::ostringstream os;
        os << "[GXF12] replay cut_policy=" << (s_cut_policy == CutPolicy::Merge ? "merge" : "perframe")
           << " backlog=" << s_max_backlog << "\n";
        std::cout << os.str();
    }
    ::g_stat_frames = r.v<uint64_t>();
    r.get(&g_state, sizeof g_state);
    float pos[256], nrm[96];
    r.get(pos, sizeof pos);
    r.get(nrm, sizeof nrm);
    ParserShadowSet(pos, nrm);
    // RENDER1: the parser keeps its own BP/XF shadows (M1.2). The captured
    // g_state is the renderer's file at capture start, which is the parse
    // state at that stream position up to the few carried commands.
    ParserRegShadowSet(g_state.bp, g_state.xf);
    ParserDLCacheClear();
    const uint32_t ndl = r.v<uint32_t>();
    for (uint32_t i = 0; i < ndl && r.ok; ++i) {
        const uint64_t key = r.v<uint64_t>(), sh = r.v<uint64_t>(), ch = r.v<uint64_t>();
        std::vector<GXCommand> cmds;
        GetCmds(r, cmds);
        ParserDLCachePut(key, sh, ch, std::move(cmds));
    }
    r.vec(s_parse_buf);
    GetCmds(r, s_carry);
    s_scan_pos = 0; // PERF2: rescan the restored carry for frame ends
    s_frame_ends.clear();
    const uint32_t ew = r.v<uint32_t>(), eh = r.v<uint32_t>();
    std::vector<uint8_t> efb((size_t)ew * eh * 4);
    if (!efb.empty()) r.get(efb.data(), efb.size());
    const uint32_t ntex = r.v<uint32_t>();
    if (!r.ok) { err = "start.bin truncated"; return false; }
    if (!efb.empty()) { // into the bound draw framebuffer via a temporary FBO
        GLint draw_fb = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
        GLuint t = 0, fb = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ew, eh, 0, GL_RGBA, GL_UNSIGNED_BYTE, efb.data());
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
        // GX row 0 is the top row in both layouts, so an older 640x480 EFB
        // image goes to the top rows of a taller (528-row) EFB.
        // RES9: an EFB captured at internal scale ss (width 640*ss) replayed
        // into one of scale ds is resized by ds/ss; ss == ds is the old 1:1 blit.
        int dw = 0, dh = 0;
        BoundFbSize(GL_DRAW_FRAMEBUFFER, dw, dh);
        auto scale_of = [](int w) {
            return (w > kEfbWidth && w % kEfbWidth == 0 && w / kEfbWidth <= kMaxInternalScale)
                       ? w / kEfbWidth : 1;
        };
        const int ss = scale_of((int)ew), ds = scale_of(dw);
        const int bw = (int)ew * ds / ss, bh = (int)eh * ds / ss;
        const int dy = dh > bh ? dh - bh : 0;
        glBlitFramebuffer(0, 0, ew, eh, 0, dy, bw, dy + bh, GL_COLOR_BUFFER_BIT,
                          ss == ds ? GL_NEAREST : GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)draw_fb);
        glDeleteFramebuffers(1, &fb);
        glDeleteTextures(1, &t);
    }
    for (uint32_t i = 0; i < ntex && r.ok; ++i) {
        const uint32_t dest = r.v<uint32_t>(), tw = r.v<uint32_t>(), th = r.v<uint32_t>();
        std::vector<uint8_t> px((size_t)tw * th * 4);
        if (!px.empty()) r.get(px.data(), px.size());
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        nwii::runtime::hle::TextureCache::get().register_efb_texture(dest, t);
    }
    if (!r.ok) { err = "start.bin truncated (efb textures)"; return false; }
    if (!ReadFile(rs.dir / "ticks.bin", rs.ticks)) { err = "cannot read ticks.bin"; return false; }
    rs.tr = RBuf{rs.ticks.data(), rs.ticks.size()};
    // Initial RAM: the first recorded snapshot. Parse-time reads are replayed
    // from the journal; this only backs reads the journal does not cover.
    rs.image.assign((size_t)kMem1 + kMem2, 0);
    for (uint64_t f = ::g_stat_frames + 1; f < ::g_stat_frames + 1000; ++f) {
        const auto p = rs.dir / FrameName("ram", f, ".bin");
        if (std::filesystem::exists(p)) {
            if (!ApplyRamFile(p, rs.image)) { err = "bad " + p.string(); return false; }
            break;
        }
    }
    CopyImageToMMU(rs.image);
    // The live renderer compiles at most 12 shaders per frame against a warm
    // cache; the replay's cache is cold, so let it compile what it needs.
    RendererSetCompileBudget(1 << 30);
    g_rendererFrameLogHook = &FrameLogFile;
    rs.open = true;
    return true;
}

int64_t ReplayStep(std::string& err) {
    ReplayState& rs = RS();
    if (!rs.open) { err = "replay not open"; return -1; }
    RBuf& r = rs.tr;
    if (r.i >= r.n) return -1;
    char magic[4];
    r.get(magic, 4);
    if (!r.ok || std::memcmp(magic, "TICK", 4) != 0) { err = "ticks.bin: bad record"; return -1; }
    const uint32_t idx = r.v<uint32_t>();
    const uint64_t frame = r.v<uint64_t>();
    const uint32_t ndl = r.v<uint32_t>();
    for (uint32_t i = 0; i < ndl && r.ok; ++i) {
        const uint32_t a = r.v<uint32_t>(), sz = r.v<uint32_t>();
        if (sz > r.n - r.i) { r.ok = false; break; }
        nwii::runtime::GX_DL_Inject(a, r.p + r.i, sz);
        r.i += sz;
    }
    const uint32_t nram = r.v<uint32_t>();
    for (uint32_t i = 0; i < nram && r.ok; ++i) {
        const uint32_t a = r.v<uint32_t>();
        std::vector<uint8_t> bytes;
        r.vec(bytes);
        if (g_mmu && !bytes.empty())
            if (uint8_t* p = g_mmu->get_ptr(a, (uint32_t)bytes.size()))
                std::memcpy(p, bytes.data(), bytes.size());
    }
    std::vector<uint8_t> chunk;
    r.vec(chunk);
    if (!r.ok) { err = "ticks.bin truncated"; return -1; }
    {
        std::lock_guard<std::mutex> lock(g_fifo_mutex);
        g_hw_fifo.insert(g_hw_fifo.end(), chunk.begin(), chunk.end());
    }
    const uint64_t before = ::g_stat_frames;
    ProcessGXFifo();
    const uint64_t got = ::g_stat_frames != before ? ::g_stat_frames : 0;
    if (got != frame) {
        err = "tick " + std::to_string(idx) + ": live cut frame " + std::to_string(frame) +
              ", replay cut " + std::to_string(got);
        return -1;
    }
    return (int64_t)got;
}

void ReplayClose() { RS() = ReplayState{}; }

} // namespace nwii::runtime::gx
