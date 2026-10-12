// DSP interface (0xCC005000): mailboxes, CSR, ARAM DMA, AI DMA — with an
// AXWii ucode HLE behind the mailbox (AUDIO track, roadmap M5.1-M5.3).
//
// Mailbox: Dolphin DSPHLE semantics (MailHandler.cpp): a queue of DSP->CPU
// mails; a mail pushed "with interrupt" raises the DSP interrupt when it is the
// first in the queue, otherwise flags the previous front so the interrupt is
// raised when the CPU reads that one. __DSPHandler (dsp_task.s 0x803CDFD8)
// busy-waits for mail, so an interrupt is never raised without a mail queued.
// CPU->DSP mails go to dsp::AxWiiHle (ROM bootstrap, then the AX ucode).
//
// AI DMA: Dolphin DSP.cpp semantics. Starting a DMA (enable with no transfer
// in flight) latches address/length, sends that buffer to the host and raises
// AID shortly after; every NumBlocks*32 bytes of play time (8 stereo frames per
// block at the AI_CR DMA rate, in TIMEBASE ticks, i.e. virtual time) the
// registers are re-latched (the game's AIInitDMA from the AID callback), the
// next buffer is sent and AID is raised again. Blocks-left reads the remaining
// count (zero-based, like Dolphin) from the scheduled end time.
#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "../hle/dsp/ax_wii_hle.h"
#include "../platform/audio_out.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <vector>

namespace nwii::runtime {
    extern MMU* g_mmu;
    extern CPUContext *g_ctx_ptr;
    extern uint64_t get_os_time();
}

#include "runtime/event_scheduler.h"

namespace nwii::runtime::hw {

static uint16_t dsp_mbox_cpu_hi = 0;
static uint32_t ar_mmaddr = 0;
static uint32_t ar_araddr = 0;
static uint32_t ar_cnt = 0;
// DSPCSR bit layout (Dolphin UDSPControl): RES=0x0001, PIINT=0x0002, HALT=0x0004,
// AID=0x0008, AID_MSK=0x0010, ARAM=0x0020, ARAM_MSK=0x0040, DSP=0x0080,
// DSP_MSK=0x0100, DMA_STATE=0x0200, INIT_CODE=0x0400, INIT=0x0800.

// Trace switches, read once at start-up (M0.3: no std::getenv per mailbox
// write and no function-local static guard per AR register access).
static const bool g_dsp_trace = std::getenv("NWII_DSPTRACE") != nullptr;
static const bool g_ar_trace = std::getenv("NWII_ARTRACE") != nullptr;
// NWII_AX_STATS=1: one line per ~1000 AX frames (voices, frames, AI buffers).
static const bool g_ax_stats = [] { const char *e = std::getenv("NWII_AX_STATS"); return e && e[0] == '1'; }();


static constexpr uint16_t CSR_INT_BITS = 0x00A8;
static uint16_t dsp_control = 0x0800;
static uint16_t ar_size = 0;
static uint16_t ar_mode = 0;
static uint16_t ar_refresh = 0;

static void dsp_update_pi();

// ---- AXWii HLE + mail queue ---------------------------------------------------
struct QueuedMail {
    uint32_t value;
    bool irq;
};
static std::deque<QueuedMail> s_mail_queue;
static uint32_t s_last_mail = 0;

// NWII_AX_TRACE=1: keep the last 64 DSP/AI events and dump them once when the
// AX frame loop stalls or the CPU busy-waits on an empty DSP mailbox.
static const bool g_ax_trace = [] { const char *e = std::getenv("NWII_AX_TRACE"); return e && e[0] == '1'; }();
struct TraceEv { uint64_t tb; char kind; uint32_t a, b; };
static TraceEv s_trace[64];
static uint32_t s_trace_n = 0;
static void trace(char kind, uint32_t a, uint32_t b = 0) {
    if (!g_ax_trace) return;
    // Collapse repeated B/A pairs with nothing in between (stalled frame loop).
    if ((kind == 'B' || kind == 'A') && s_trace_n >= 2 && s_trace[(s_trace_n - 1) & 63].kind == 'A' &&
        s_trace[(s_trace_n - 2) & 63].kind == 'B') {
        if (kind == 'A') s_trace[(s_trace_n - 1) & 63].b++;
        return;
    }
    s_trace[s_trace_n++ & 63] = {g_ctx_ptr ? g_ctx_ptr->read_timebase() : 0, kind, a, b};
}
static void trace_dump(const char *why) {
    std::fprintf(stderr, "[AXTRACE] dump (%s): W=cpu mail Q=queue(irq) R=read lo(left) E=empty-hi streak C=csr(new) A=AID B=AI buf(blocks)\n", why);
    if (g_ctx_ptr)
        std::fprintf(stderr, "[AXTRACE] pc=%08x lr=%08x msr=%08x csr=%04x queue=%zu front=%08x\n", g_ctx_ptr->pc,
                     g_ctx_ptr->lr, g_ctx_ptr->msr, dsp_control, s_mail_queue.size(),
                     s_mail_queue.empty() ? 0u : s_mail_queue.front().value);
    const uint32_t n = std::min<uint32_t>(s_trace_n, 64);
    for (uint32_t i = s_trace_n - n; i != s_trace_n; ++i) {
        const TraceEv &e = s_trace[i & 63];
        std::fprintf(stderr, "[AXTRACE] tb=%llu %c %08x %08x\n", (unsigned long long)e.tb, e.kind, e.a, e.b);
    }
}
static uint64_t s_empty_hi_streak = 0;

// NWII_WATCH=<hex addr> (os_hle_stubs.cpp) catches PPC stores to one word; host-side
// writers (this file's AX HLE and ARAM DMA, DI/IPC DMA) bypass it. When NWII_WATCH is
// set, poll the word around every host-side write done here and at each AI buffer
// end, and log who changed it ("by AX HLE" / "by ARAM DMA" / "outside DSP").
// Diagnostic only (startup-cached, off by default); guest memory is only read.
static const uint32_t g_dsp_watch_pa = [] {
    const char *e = std::getenv("NWII_WATCH");
    return e ? (uint32_t)(std::strtoul(e, nullptr, 16) & 0x3FFFFFFCu) : 0u;
}();
static void dsp_watch_poll(const char *who) {
    if (!g_dsp_watch_pa || !g_mmu) return;
    static bool init = false;
    static uint32_t last = 0;
    const uint8_t *p = g_mmu->get_ptr(g_dsp_watch_pa, 4);
    if (!p) return;
    const uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    if (init && v != last)
        std::fprintf(stderr, "[DSPWatch] 0x%08x: 0x%08x -> 0x%08x %s (tb=%llu)\n", g_dsp_watch_pa | 0x80000000u, last, v,
                     who, (unsigned long long)EventScheduler::get().now());
    init = true;
    last = v;
}

static dsp::DspMemory dsp_memory() {
    dsp::DspMemory m;
    if (g_mmu) {
        m.mem1 = g_mmu->mem1.data();
        m.mem1_size = (uint32_t)g_mmu->mem1.size() - 8;
        m.mem2 = g_mmu->mem2.data();
        m.mem2_size = (uint32_t)g_mmu->mem2.size() - 8;
    }
    return m;
}

static dsp::AxWiiHle &ax_hle() {
    static dsp::AxWiiHle hle(dsp_memory());
    static bool coef_loaded = [] {
        const char *p = std::getenv("NWII_DSP_COEF");
        if (p && *p)
            std::fprintf(stderr, "[AXHLE] DSP coef table %s: %s\n", p,
                         dsp::load_polyphase_table(p) ? "loaded" : "FAILED (using built-in cubic)");
        return true;
    }();
    (void)coef_loaded;
    return hle;
}

static void raise_dsp_interrupt() {
    dsp_control |= 0x0080;
    dsp_update_pi();
}

// Dolphin CMailHandler::PushMail.
static void queue_mails(const std::vector<dsp::Mail> &mails) {
    for (const dsp::Mail &m : mails) {
        if (m.irq) {
            if (s_mail_queue.empty()) raise_dsp_interrupt();
            else s_mail_queue.front().irq = true;
        }
        s_mail_queue.push_back({m.value, false});
        trace('Q', m.value, (m.irq ? 0x80000000u : 0) | (uint32_t)s_mail_queue.size());
        if (g_dsp_trace)
            std::cout << "[DSPm] queue mail 0x" << std::hex << m.value << (m.irq ? " +irq" : "")
                      << std::dec << "\n";
    }
}

// Deliver replies after `delay` timebase ticks (models DSP execution time).
static void deliver_mails(std::vector<dsp::Mail> mails, uint64_t delay) {
    if (mails.empty()) return;
    if (delay == 0 || !g_ctx_ptr) { queue_mails(mails); return; }
    EventScheduler::get().schedule_after(delay, [mails](CPUContext &, uint64_t) { queue_mails(mails); });
}

static void dsp_reset_mailbox() {
    // CSR reset / init-code start: back to the ROM bootstrap with exactly one
    // mail pending and no interrupt (the value is ignored by __DSP_boot_task
    // and __OSInitAudioSystem; kept as the previous runtime's 0xDCD10000).
    ax_hle().set_memory(dsp_memory());
    ax_hle().reset_to_rom();
    s_mail_queue.clear();
    s_mail_queue.push_back({0xDCD10000u, false});
}

static void ar_dma_execute() {
    bool dir = (ar_cnt & 0x80000000) != 0;
    uint32_t count = ar_cnt & 0x7FFFFFFF;
    uint32_t mm = ar_mmaddr & 0x01FFFFFF;
    uint32_t ar_a = ar_araddr & 0x00FFFFFF;
    if (g_mmu && count > 0) {
        uint8_t *mem1 = nwii::runtime::g_mmu->mem1.data();
        uint8_t *mem2 = nwii::runtime::g_mmu->mem2.data();
        uint32_t mem1_size = nwii::runtime::g_mmu->mem1.size();
        uint32_t mem2_size = nwii::runtime::g_mmu->mem2.size();
        if (mm < mem1_size && ar_a < mem2_size) {
            dsp_watch_poll("outside DSP (before ARAM DMA)");
            uint32_t copy_count = std::min({count, mem1_size - mm, mem2_size - ar_a});
            if (!dir) std::memcpy(mem2 + ar_a, mem1 + mm, copy_count);
            else std::memcpy(mem1 + mm, mem2 + ar_a, copy_count);
            dsp_watch_poll("BY ARAM DMA");
        }
    }
    EventScheduler::get().schedule_after(
        (uint64_t)count / 2 + 64, [](CPUContext&, uint64_t) {
            ar_cnt = 0;
            dsp_control |= 0x0020;
            dsp_update_pi();
        });
}

// ---- AI DMA -----------------------------------------------------------------------
static uint32_t adma_addr = 0;
static uint16_t adma_control = 0;
static bool s_adma_running = false;
static uint32_t s_adma_cur_blocks = 0;
static uint64_t s_adma_due = 0;      // scheduler time at which the current buffer ends
static uint64_t s_adma_event_id = 0;
static uint64_t s_adma_buffers = 0;
static uint64_t s_adma_stale = 0;  // buffers not sent to the host (CPU stalled)
static uint64_t s_hle_ns = 0;      // NWII_AX_STATS: host time spent in the AX HLE
static uint64_t s_adma_ext = 0;    // NWII_AX_STATS: AI buffers not written by an AX OUTPUT (THP mix)
static uint32_t s_adma_ext_last = 0;

// The host device is fed by audio_out (own SDL callback device). main.cpp's
// legacy 32 kHz queue device is kept silent until that block is removed.
size_t dsp_audio_pull(int16_t* out, size_t frames) {
    std::memset(out, 0, frames * 2 * sizeof(int16_t));
    return 0;
}

static uint64_t adma_block_ticks() {
    const uint64_t rate = ai_dma_sample_rate();
    const uint64_t tb = g_ctx_ptr ? g_ctx_ptr->tb_freq : 40500000;
    return std::max<uint64_t>(1, tb * 8 / rate);  // 32 bytes = 8 stereo frames
}

static void adma_send_buffer() {
    const uint32_t frames = s_adma_cur_blocks * 8;
    if (!g_mmu || frames == 0) return;
    const uint8_t *p = g_mmu->get_ptr(adma_addr & 0x3FFFFFE0u, frames * 4);
    static std::vector<int16_t> buf;
    buf.resize(frames * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        // AI DMA stream is R, L big-endian (Dolphin Mixer / AXWii OutputSamples).
        const int16_t r = p ? (int16_t)((p[4 * i] << 8) | p[4 * i + 1]) : 0;
        const int16_t l = p ? (int16_t)((p[4 * i + 2] << 8) | p[4 * i + 3]) : 0;
        buf[2 * i] = l;
        buf[2 * i + 1] = r;
    }
    audio::push_samples(buf.data(), frames, ai_dma_sample_rate());
    s_adma_buffers++;
    if (g_ax_stats && !ax_hle().stats().is_output_addr(adma_addr)) {
        s_adma_ext++;
        s_adma_ext_last = adma_addr & 0x3FFFFFE0u;
    }
    if (g_ax_stats && (s_adma_buffers % 2000) == 0) {
        const auto &st = ax_hle().stats();
        const auto os = audio::stats();
        std::fprintf(stderr,
                     "[AXHLE] ai_bufs=%llu stale=%llu ext=%llu@%08x hle_us/frame=%.1f rate=%u ax_frames=%llu outputs=%llu voices(last/max)=%u/%u unk_cmds=%llu "
                     "host: pushed=%llu played=%llu fill=%u underruns=%llu dropped=%llu\n",
                     (unsigned long long)s_adma_buffers, (unsigned long long)s_adma_stale,
                     (unsigned long long)s_adma_ext, s_adma_ext_last,
                     st.frames ? s_hle_ns / 1000.0 / (double)st.frames : 0.0, ai_dma_sample_rate(),
                     (unsigned long long)st.frames,
                     (unsigned long long)st.outputs, st.last_voices, st.max_voices,
                     (unsigned long long)st.unknown_cmds, (unsigned long long)os.pushed,
                     (unsigned long long)os.played, os.fill, (unsigned long long)os.underruns,
                     (unsigned long long)os.dropped);
    }
}

static void adma_raise_aid() {
    trace('A', dsp_control);
    dsp_control |= 0x0008;
    dsp_update_pi();
}

static void adma_schedule_end();

static void adma_buffer_end(CPUContext &, uint64_t) {
    s_adma_event_id = 0;
    dsp_watch_poll("outside DSP (at AI buffer end)");
    if (!s_adma_running) return;
    if (!(adma_control & 0x8000)) {
        s_adma_running = false;
        return;
    }
    // Re-latch the (possibly re-programmed) registers, like Dolphin's
    // UpdateAudioDMA when remaining_blocks_count reaches zero.
    s_adma_cur_blocks = adma_control & 0x7FFF;
    trace('B', adma_addr, s_adma_cur_blocks);
    // If the AID raised at the previous buffer start is still un-acked, the CPU
    // has not run for a whole buffer (interrupts off across a clock jump, e.g.
    // PERF2's pre-existing jump in IpcReplyHandler): the latched buffer is a
    // stale replay. Hardware would play it again (buzz); the host stream gets
    // nothing instead (underrun -> silence). Guest-visible state is unchanged.
    if ((dsp_control & 0x0018) == 0x0018) s_adma_stale++;  // AID pending and unmasked
    else adma_send_buffer();
    adma_raise_aid();
    adma_schedule_end();
    if (g_ax_trace) {  // stall detector: no command list for 100 AI buffers
        static uint64_t last_frames = 0, same = 0;
        static bool dumped = false;
        const uint64_t f = ax_hle().stats().frames;
        same = (f == last_frames && ax_hle().in_ax()) ? same + 1 : 0;
        last_frames = f;
        if (same == 100 && !dumped) { dumped = true; trace_dump("AX frame loop stalled for 100 AI buffers"); }
    }
}

static void adma_schedule_end() {
    const uint64_t blocks = std::max<uint32_t>(1, s_adma_cur_blocks);
    const uint64_t delay = blocks * adma_block_ticks();
    s_adma_due = EventScheduler::get().now() + delay;
    s_adma_event_id = EventScheduler::get().schedule_after(delay, adma_buffer_end);
}

static void adma_control_written(uint16_t val) {
    adma_control = val;
    if ((val & 0x8000) && !s_adma_running) {
        s_adma_running = true;
        s_adma_cur_blocks = val & 0x7FFF;
        adma_send_buffer();
        // Dolphin schedules this AID 200 CPU cycles after the start.
        EventScheduler::get().schedule_after(16, [](CPUContext &, uint64_t) { adma_raise_aid(); });
        adma_schedule_end();
    } else if (!(val & 0x8000) && s_adma_running) {
        s_adma_running = false;
        if (s_adma_event_id) EventScheduler::get().cancel(s_adma_event_id);
        s_adma_event_id = 0;
    }
}

static uint16_t adma_blocks_left() {
    if (!s_adma_running) return 0;
    const uint64_t now = EventScheduler::get().now();
    const uint64_t per = adma_block_ticks();
    uint64_t remaining = s_adma_due > now ? (s_adma_due - now + per - 1) / per : 1;
    remaining = std::clamp<uint64_t>(remaining, 1, std::max<uint32_t>(1, s_adma_cur_blocks));
    return (uint16_t)(remaining - 1);  // zero-based (Dolphin DSP.cpp AUDIO_DMA_BLOCKS_LEFT)
}

// three DSP subsystem sources — Dolphin's (csr >> 1) & csr trick.
static void dsp_update_pi() {
    if ((dsp_control >> 1) & dsp_control & CSR_INT_BITS)
        trigger_pi_interrupt(0x40);
    else
        clear_pi_interrupt(0x40);
}

int dsp_pending_os_interrupt() {
    uint16_t pending = (dsp_control >> 1) & dsp_control & CSR_INT_BITS;
    if (pending & 0x0080) return 7;
    if (pending & 0x0020) return 6;
    if (pending & 0x0008) return 5;
    return 7;
}

static uint16_t dsp_mail_read_hi() {
    if (g_ax_trace) {
        if (s_mail_queue.empty()) {
            if (++s_empty_hi_streak == 200000) { trace('E', (uint32_t)s_empty_hi_streak, dsp_control); trace_dump("busy-wait on empty DSP mailbox"); }
        } else {
            s_empty_hi_streak = 0;
        }
    }
    return s_mail_queue.empty() ? 0 : (uint16_t)(s_mail_queue.front().value >> 16);
}

static uint16_t dsp_mail_read_lo() {
    if (s_mail_queue.empty()) { trace('R', 0xEEEEEEEEu, 0); return (uint16_t)s_last_mail; }
    const QueuedMail m = s_mail_queue.front();
    s_mail_queue.pop_front();
    trace('R', m.value, (m.irq ? 0x80000000u : 0) | (uint32_t)s_mail_queue.size());
    s_last_mail = m.value;
    if (m.irq) raise_dsp_interrupt();  // interrupt for the next queued mail
    if (g_dsp_trace)
        std::cout << "[DSPm] mail read 0x" << std::hex << m.value << " csr=0x" << dsp_control << std::dec
                  << " left=" << s_mail_queue.size() << "\n";
    return (uint16_t)m.value;
}

static void cpu_mail_written(uint32_t mail) {
    dsp::AxWiiHle &hle = ax_hle();
    hle.set_memory(dsp_memory());
    std::vector<dsp::Mail> replies;
    trace('W', mail, hle.in_ax());
    dsp_watch_poll("outside DSP (before CPU mail)");
    if (g_ax_stats) {  // host cost of the HLE (command lists) for the stats line
        const auto t0 = std::chrono::steady_clock::now();
        hle.handle_mail(mail, replies);
        s_hle_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    } else {
        hle.handle_mail(mail, replies);
    }
    dsp_watch_poll("BY AX HLE (command list / PB writeback)");
    if (replies.empty()) return;
    const uint64_t tb = g_ctx_ptr ? g_ctx_ptr->tb_freq : 40500000;
    // A processed command list ends with DSP_YIELD: model ~0.25 ms of DSP time.
    // Boot (DSP_INIT) and other replies keep the previous runtime's 1 ms.
    const bool frame_done = replies.back().value == dsp::DSP_YIELD;
    deliver_mails(std::move(replies), frame_done ? tb / 4000 : tb / 1000);
}

void register_dsp(MMIODispatcher& dispatcher) {{
    dispatcher.register_region(0xCC005000, 0xCC0050FF,
        [](uint32_t addr) -> uint32_t {
            if (addr == 0xCC005000 || addr == 0xCC005002) return dsp_mbox_cpu_hi;
            if (addr == 0xCC005004) return dsp_mail_read_hi();
            if (addr == 0xCC005006) return dsp_mail_read_lo();
            if (addr == 0xCC005008 || addr == 0xCC00500A) return dsp_control;
            if (addr == 0xCC005012) return ar_size;
            if (addr == 0xCC005016) return ar_mode | 0x0001;
            if (addr == 0xCC00501A) return ar_refresh;
            if (addr == 0xCC005020) return ar_mmaddr;
            if (addr == 0xCC005024) return ar_araddr;
            if (addr == 0xCC005028) return ar_cnt;
            if (addr == 0xCC005030) return adma_addr >> 16;
            if (addr == 0xCC005032) return adma_addr & 0xFFFF;
            if (addr == 0xCC005036) return adma_control;
            if (addr == 0xCC00503A) return adma_blocks_left();
            return 0;
        },
        [](uint32_t addr, uint32_t val) {
            const bool artrace = g_ar_trace;
            if (artrace && (addr & 0xFFF0) >= 0x5010 && (addr & 0xFFF0) < 0x5030)
                std::cout << "[ARw] 0x" << std::hex << (addr & 0xFFFF)
                          << " = 0x" << val << std::dec << "\n";
            if (addr == 0xCC005000 || addr == 0xCC005002) {
                // CPU->DSP mailbox. The HLE consumes a mail as soon as its low
                // half is written, so the CPU always sees the box as read.
                static uint16_t s_cpu_mail_hi = 0;
                const bool completed = (addr == 0xCC005002);
                if (addr == 0xCC005000)
                    s_cpu_mail_hi = val & 0xFFFF;
                dsp_mbox_cpu_hi = (val & 0x7FFF);
                if (g_dsp_trace)
                    std::cout << "[DSPm] cpu mail write @" << std::hex
                              << (addr & 0xFFFF) << " val=0x" << val
                              << " csr=0x" << dsp_control << std::dec << "\n";
                if (completed)
                    cpu_mail_written(((uint32_t)s_cpu_mail_hi << 16) | (val & 0xFFFF));
            } else if (addr == 0xCC005004 || addr == 0xCC005006) {
                // DSP->CPU mailbox is read-only for the CPU.
            } else if (addr == 0xCC005008 || addr == 0xCC00500A) {
                uint16_t val16 = val & 0xFFFF;
                if (g_dsp_trace)
                    std::cout << "[DSPm] csr write val=0x" << std::hex
                              << (val & 0xFFFF) << " csr=0x" << dsp_control
                              << std::dec << "\n";
                uint16_t kept_ints = dsp_control & CSR_INT_BITS & ~val16;
                bool was_init = (dsp_control & 0x0800) != 0;
                bool is_init = (val16 & 0x0800) != 0;
                dsp_control = kept_ints | (val16 & ~(CSR_INT_BITS | 0x0001));
                trace('C', val16, dsp_control);
                // RES (reset) or the end of DSPInit: ROM bootstrap with one mail.
                if ((val16 & 0x0001) || (was_init && !is_init))
                    dsp_reset_mailbox();
                dsp_update_pi();
            } else if (addr == 0xCC005012) {
                ar_size = val & 0xFFFF;
            } else if (addr == 0xCC005016) {
                ar_mode = val & 0xFFFF;
            } else if (addr == 0xCC00501A) {
                ar_refresh = val & 0xFFFF;
            } else if (addr == 0xCC005030) {
                adma_addr = (adma_addr & 0x0000FFFF) | ((val & 0xFFFF) << 16);
            } else if (addr == 0xCC005032) {
                adma_addr = (adma_addr & 0xFFFF0000) | (val & 0xFFFF);
            } else if (addr == 0xCC005036) {
                adma_control_written((uint16_t)val);
            } else if (addr == 0xCC005020) {
                ar_mmaddr = val;
            } else if (addr == 0xCC005024) {
                ar_araddr = val;
            } else if (addr == 0xCC005028) {
                ar_cnt = val;
                ar_dma_execute();
            }
        },
        // 16-bit halves of the 32-bit ARAM DMA registers
        // (Dolphin DSP.cpp: MMADDR_H/L, ARADDR_H/L, CNT_H/L).
        [](uint32_t addr) -> uint16_t {
            if (addr == 0xCC005020) return (uint16_t)(ar_mmaddr >> 16);
            if (addr == 0xCC005022) return (uint16_t)ar_mmaddr;
            if (addr == 0xCC005024) return (uint16_t)(ar_araddr >> 16);
            if (addr == 0xCC005026) return (uint16_t)ar_araddr;
            if (addr == 0xCC005028) return (uint16_t)(ar_cnt >> 16);
            if (addr == 0xCC00502A) return (uint16_t)ar_cnt;
            return (uint16_t)MMIODispatcher::get().read32(addr);
        },
        [](uint32_t addr, uint16_t val) {
            const bool artrace = g_ar_trace;
            if (artrace && (addr & 0xFFFF) >= 0x5020 && (addr & 0xFFFF) < 0x5030)
                std::cout << "[ARw16] 0x" << std::hex << (addr & 0xFFFF)
                          << " = 0x" << val << std::dec << "\n";
            if      (addr == 0xCC005020) ar_mmaddr = (ar_mmaddr & 0x0000FFFF) | ((uint32_t)val << 16);
            else if (addr == 0xCC005022) ar_mmaddr = (ar_mmaddr & 0xFFFF0000) | val;
            else if (addr == 0xCC005024) ar_araddr = (ar_araddr & 0x0000FFFF) | ((uint32_t)val << 16);
            else if (addr == 0xCC005026) ar_araddr = (ar_araddr & 0xFFFF0000) | val;
            else if (addr == 0xCC005028) ar_cnt = (ar_cnt & 0x0000FFFF) | ((uint32_t)val << 16);
            else if (addr == 0xCC00502A) {
                ar_cnt = (ar_cnt & 0xFFFF0000) | val;
                ar_dma_execute();
            }
            else MMIODispatcher::get().write32(addr, val);
        }
    );
}}

void dsp_trigger_interrupt() {
    if (!s_mail_queue.empty())  // never without a mail (__DSPHandler busy-waits for one)
        raise_dsp_interrupt();
}

}
