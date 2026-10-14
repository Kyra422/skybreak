#include "runtime/cpu_context.h"
#include "input/input_manager.h"
#include "runtime/config.h"
#include "runtime/nand_fs.h"
#include "../input/wiimote_speaker.h"
#include "../platform/audio_out.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace nwii::runtime;
using namespace nwii::runtime::input;

extern "C" {

void PADInit(CPUContext& ctx) {}
void WPADInit(CPUContext& ctx) {}
void KPADInit(CPUContext& ctx) {}

void PADRead(CPUContext& ctx) {
    uint32_t pad_status_array_addr = ctx.gpr[3];
    GameCubePadState state = InputManager::get().get_gcpad_state(0);

    ctx.mmu.write16(pad_status_array_addr, state.buttons);
    ctx.mmu.write8(pad_status_array_addr + 2, (uint8_t)state.stick_x);
    ctx.mmu.write8(pad_status_array_addr + 3, (uint8_t)state.stick_y);
    ctx.mmu.write8(pad_status_array_addr + 4, (uint8_t)state.substick_x);
    ctx.mmu.write8(pad_status_array_addr + 5, (uint8_t)state.substick_y);
    ctx.mmu.write8(pad_status_array_addr + 6, state.trigger_l);
    ctx.mmu.write8(pad_status_array_addr + 7, state.trigger_r);
    ctx.mmu.write8(pad_status_array_addr + 8, state.buttons & 0x0100 ? 0xFF : 0);
    ctx.mmu.write8(pad_status_array_addr + 9, state.buttons & 0x0200 ? 0xFF : 0);
    ctx.mmu.write8(pad_status_array_addr + 10, state.err);
    ctx.mmu.write8(pad_status_array_addr + 11, 0);

    for (int i = 1; i < 4; i++) {
        uint32_t offset = pad_status_array_addr + (i * 12);
        ctx.mmu.write16(offset, 0);
        ctx.mmu.write8(offset + 10, (uint8_t)-1); 
    }
}

void WPADRead(CPUContext& ctx) {
    int32_t chan = (int32_t)ctx.gpr[3];
    uint32_t data_ptr = ctx.gpr[4];

    if (chan >= 0 && chan < 4 && data_ptr != 0) {
        WiimoteState state = InputManager::get().get_wiimote_state(chan);
        ctx.mmu.write32(data_ptr + 0x00, state.err);
        ctx.mmu.write32(data_ptr + 0x04, state.buttons);
    }
    ctx.gpr[3] = 0; 
}

void KPADRead(CPUContext& ctx) {
    int32_t chan = (int32_t)ctx.gpr[3];
    uint32_t data_array_ptr = ctx.gpr[4];
    uint32_t length = ctx.gpr[5];

    int read_count = 0;
    if (chan >= 0 && chan < 4 && data_array_ptr != 0 && length > 0) {
        WiimoteState state = InputManager::get().get_wiimote_state(chan);

        if (state.err == 0) {

            

            

            
            static uint32_t prev_buttons[4] = {};
            uint32_t hold    = state.buttons;
            uint32_t trigger = hold & ~prev_buttons[chan];  
            uint32_t release = ~hold & prev_buttons[chan];  
            prev_buttons[chan] = hold;

            ctx.mmu.write32(data_array_ptr + 0x00, hold);
            ctx.mmu.write32(data_array_ptr + 0x04, trigger);
            ctx.mmu.write32(data_array_ptr + 0x08, release);
            
            ctx.mmu.write32(data_array_ptr + 0x0C, 0);
            ctx.mmu.write32(data_array_ptr + 0x10, 0);
            ctx.mmu.write32(data_array_ptr + 0x14, 0);
            
            for (int b = 0x18; b < 0x48; b += 4)
                ctx.mmu.write32(data_array_ptr + b, 0);
            
            ctx.mmu.write32(data_array_ptr + 0x48, 0);
            
            ctx.mmu.write32(data_array_ptr + 0x4C, 0);

            read_count = 1;
        }
    }
    ctx.gpr[3] = read_count;
}

} 

// ---- SPK11 (roadmap M5.4): Wii Remote speaker -------------------------------------------------------------
// SOUE01 runs WPADInit as SS_WPADInit (ss_boot.cpp), which never creates the SDK's per-channel control
// blocks, so the original WPADControlSpeaker / WPADSendStreamData / WPADIsSpeakerEnabled would dereference
// a null __rvl_p_wpadcb[chan] (low memory). These hooks ([hle_hooks] 803DBA40 / 803DD2A0 / 803DB9F0) do what
// the SDK + a real remote do, with Dolphin semantics: each command applies the output reports the SDK would
// queue to an emulated speaker (src/input/wiimote_speaker.h = Dolphin SpeakerLogic) and completes with
// WPAD_ERR_OK through the guest callback; each stream report is decoded (Yamaha ADPCM / PCM8) and mixed into
// the host output (audio_out push_speaker_samples). WPADCanSendStreamData stays SS_WPADCanSendStreamData.
//
// Speaker volume: the replaced WPADInit also skips __wpadInitSub's "speaker volume = SCGetWpadSpeakerVolume()
// clamped to 0x7F" (803D91F4..803D9230, SDK byte 8057640F, read by WPADGetSpeakerVolume and by the ON config).
// EGG::AudioRmtSpeakerMgr caches that volume (.sdata 80574F68, initially 0x58) and refreshes it from
// WPADGetSpeakerVolume after every successful speaker setup; while it is non-zero, the remote sounds
// (dSoundSource::startRemoConSound, dSndSmallEffectMgr / dSndStateMgr remote SEs) are routed to the remote
// output line only. Before SPK11 the setup never completed, the cache stayed 0x58 and those sounds went to
// remote buses nobody played. The first speaker command seeds the SDK byte once, as WPADInit would:
// SYSCONF BT.SPKV (default 0x58), or 0 when [audio] remote_speaker_volume is 0 -- then the game sees
// "speaker volume 0" after the setup and plays those sounds through the TV mix instead.
// Nothing here runs unless the hooks are bound.
//
// Env (diagnostics): NWII_SPK_STATS=1 logs every speaker command and a counter line every 10 s;
// NWII_SPK_VOLUME=<0..2> overrides [audio] remote_speaker_volume; NWII_SPK_DUMP (audio_out.h);
// NWII_SPK_RAW=<file> appends every stream report as {channel, length, result 0..3, data[length]}
// (result: 0 decoded, 1 muted, 2 bad length, 3 disabled / no sample rate) for offline re-decoding.
// NWII_SPK_SELFTEST=1 (diagnostic): after the game's first PLAY on remote 0, feeds a synthetic tone sequence
// (440 Hz 1 s, 0.5 s pause, 1 kHz 1 s; Yamaha-encoded host-side) through the same report -> decoder -> mixer
// path, paced by the AI stream in virtual time like the game's 6.67 ms report alarm. Proves the live mix
// in runs where the game itself routes nothing to the remote. Never used for playing.
namespace {

constexpr int32_t kWpadOk = 0;
constexpr int32_t kWpadNoController = -1;
constexpr uint32_t kSpkConfTemplate = 0x8057E930;  // .sdata2: 00 00 D0 07 | 40 0C | 0E (WPADControlSpeaker 803DBA58..84)
constexpr uint32_t kSpkSdkVolume = 0x8057640F;     // .sbss u8: WPAD speaker volume

struct SpeakerHle {
    std::array<nwii::runtime::speaker::SpeakerLogic, 4> spk{};
    bool seeded = false;
    bool stats_on = false;
    float gain = 0.8f;  // [audio] remote_speaker_volume
    uint64_t commands = 0, reports = 0, bytes = 0, decoded = 0, muted = 0, silent = 0, bad = 0, no_ctrl = 0;
    uint64_t last_reports = 0, last_bytes = 0;
    std::FILE* raw = nullptr;
    // NWII_SPK_SELFTEST
    bool selftest = false, st_armed = false, st_done = false;
    nwii::runtime::speaker::AdpcmState st_enc{};
    double st_clock = 0.0;
    uint64_t st_n = 0, st_reports = 0;
    std::chrono::steady_clock::time_point t0{}, last{};
};

SpeakerHle& speaker_hle() {
    static SpeakerHle h = [] {
        SpeakerHle s;
        const char* st = std::getenv("NWII_SPK_STATS");
        s.stats_on = st && st[0] == '1';
        s.gain = nwii::runtime::Config::get().remote_speaker_volume;
        if (const char* v = std::getenv("NWII_SPK_VOLUME")) {
            const float f = static_cast<float>(std::atof(v));
            if (f >= 0.0f && f <= 2.0f) s.gain = f;
        }
        if (const char* r = std::getenv("NWII_SPK_RAW")) s.raw = std::fopen(r, "wb");
        const char* t = std::getenv("NWII_SPK_SELFTEST");
        s.selftest = t && t[0] == '1';
        s.t0 = s.last = std::chrono::steady_clock::now();
        return s;
    }();
    return h;
}

bool speaker_channel_connected(int32_t channel) {
    return channel >= 0 && channel < 4 && InputManager::get().get_wiimote_state(channel).err == 0;
}

uint8_t sysconf_speaker_volume() {
    std::vector<uint8_t> sc;
    size_t offset = 0, size = 0;
    if (nwii::runtime::nand::instance().read_whole_file("/shared2/sys/SYSCONF", sc) &&
        nwii::runtime::nand::sysconf_find_item(sc, "BT.SPKV", &offset, &size) && size == 1 && offset < sc.size())
        return sc[offset] < 0x7F ? sc[offset] : uint8_t(0x7F);
    return 0x58;  // default SYSCONF (Dolphin / nand_fs make_default_sysconf)
}

void seed_sdk_speaker_volume(CPUContext& ctx, SpeakerHle& h) {
    if (h.seeded) return;
    h.seeded = true;
    const uint8_t volume = h.gain > 0.0f ? sysconf_speaker_volume() : 0;
    ctx.mmu.write8(kSpkSdkVolume, volume);
    if (h.stats_on)
        std::fprintf(stderr, "[SPK] WPAD speaker volume = 0x%02X (%s), remote_speaker_volume = %.2f\n", volume,
                     h.gain > 0.0f ? "SYSCONF BT.SPKV" : "speaker off", h.gain);
}

void speaker_stats_tick(SpeakerHle& h, bool force) {
    if (!h.stats_on) return;
    const auto now = std::chrono::steady_clock::now();
    const double since = std::chrono::duration<double>(now - h.last).count();
    if (!force && since < 10.0) return;
    const auto mix = nwii::runtime::audio::speaker_stats();
    std::fprintf(stderr,
                 "[SPK] wall=%.1fs cmds=%llu reports=%llu bytes=%llu (%.0f B/s, %.1f reports/s over %.1fs) "
                 "decoded=%llu muted=%llu silent=%llu bad=%llu noctrl=%llu | mix pushed=%llu mixed=%llu "
                 "frames=%llu gaps=%llu dropped=%llu fill=%u\n",
                 std::chrono::duration<double>(now - h.t0).count(), (unsigned long long)h.commands,
                 (unsigned long long)h.reports, (unsigned long long)h.bytes,
                 since > 0 ? double(h.bytes - h.last_bytes) / since : 0.0,
                 since > 0 ? double(h.reports - h.last_reports) / since : 0.0, since,
                 (unsigned long long)h.decoded, (unsigned long long)h.muted, (unsigned long long)h.silent,
                 (unsigned long long)h.bad, (unsigned long long)h.no_ctrl, (unsigned long long)mix.pushed,
                 (unsigned long long)mix.mixed, (unsigned long long)mix.frames_touched,
                 (unsigned long long)mix.gaps, (unsigned long long)mix.dropped, mix.fill);
    h.last = now;
    h.last_reports = h.reports;
    h.last_bytes = h.bytes;
}

// NWII_SPK_SELFTEST pump (audio_out set_speaker_pump): one 20-byte report per 40 speaker samples of AI time.
void speaker_selftest_pump(size_t frames, uint32_t rate) {
    namespace spk = nwii::runtime::speaker;
    SpeakerHle& h = speaker_hle();
    if (!h.st_armed || !rate) return;
    constexpr double kRate = 6000.0, kPi = 3.14159265358979323846;
    constexpr uint64_t kEnd = 15000;  // 2.5 s at 6 kHz
    h.st_clock += double(frames) * kRate / double(rate);
    while (h.st_clock >= 40.0 && h.st_armed) {
        h.st_clock -= 40.0;
        int16_t pcm[40];
        bool silent = true;
        for (int i = 0; i < 40; ++i, ++h.st_n) {
            const double t = double(h.st_n) / kRate;
            const double f = h.st_n < 6000 ? 440.0 : (h.st_n >= 9000 && h.st_n < kEnd) ? 1000.0 : 0.0;
            pcm[i] = f > 0.0 ? int16_t(12000.0 * std::sin(2.0 * kPi * f * t)) : 0;
            silent = silent && pcm[i] == 0;
        }
        if (h.st_n >= kEnd) {
            h.st_armed = false;
            h.st_done = true;
            std::fprintf(stderr, "[SPK] self-test done: %llu reports sent through the decoder/mixer path\n",
                         (unsigned long long)h.st_reports);
        }
        if (silent) continue;  // like nw4r: all-zero blocks are not sent
        uint8_t data[20];
        for (int i = 0; i < 20; ++i)
            data[i] = uint8_t((spk::adpcm_yamaha_compress_sample(h.st_enc, pcm[2 * i]) << 4) |
                              spk::adpcm_yamaha_compress_sample(h.st_enc, pcm[2 * i + 1]));
        ++h.st_reports;
        ++h.reports;
        h.bytes += 20;
        spk::SpeakerBlock block;
        if (spk::speaker_report(h.spk[0], 20, data, &block) == spk::ReportResult::Decoded) {
            ++h.decoded;
            if (h.gain > 0.0f)
                nwii::runtime::audio::push_speaker_samples(0, block.samples.data(), block.count, block.rate,
                                                           block.gain() * h.gain);
        }
    }
}

} // namespace

extern "C" {

// s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback cb)
void SS_WPADControlSpeaker(CPUContext& ctx) {
    namespace spk = nwii::runtime::speaker;
    const int32_t channel = static_cast<int32_t>(ctx.gpr[3]);
    const uint32_t command = ctx.gpr[4], callback = ctx.gpr[5];
    SpeakerHle& h = speaker_hle();
    ++h.commands;
    if (!speaker_channel_connected(channel)) {
        // SDK: status WPAD_ERR_NO_CONTROLLER -> callback(chan, -1), return -1.
        ++h.no_ctrl;
        if (callback)
            ctx.queue_callback(callback, static_cast<uint32_t>(channel), static_cast<uint32_t>(kWpadNoController));
        ctx.gpr[3] = static_cast<uint32_t>(kWpadNoController);
        return;
    }
    seed_sdk_speaker_volume(ctx, h);
    uint8_t conf[7];
    for (uint32_t i = 0; i < 7; ++i) conf[i] = ctx.mmu.read8(kSpkConfTemplate + i);
    conf[4] = ctx.mmu.read8(kSpkSdkVolume);  // lbz lbl_8057640F; stb -> conf[4] (803DC7B4..D0)
    spk::SpeakerLogic& s = h.spk[size_t(channel)];
    spk::wpad_control_speaker(s, command, conf);
    if (h.selftest && !h.st_armed && !h.st_done && channel == 0 && command == spk::kSpeakerPlay) {
        h.st_armed = true;
        h.st_enc = s.adpcm;  // encoder starts in the decoder's state, so both stay in step
        nwii::runtime::audio::set_speaker_pump(speaker_selftest_pump);
        std::fprintf(stderr, "[SPK] self-test armed (NWII_SPK_SELFTEST): 440 Hz 1 s, pause 0.5 s, 1 kHz 1 s\n");
    }
    if (h.stats_on) {
        const bool adpcm = s.format() == spk::kFormatAdpcm;
        const uint32_t rate =
            s.sample_rate_reg() ? (adpcm ? 6000000u : 12000000u) / s.sample_rate_reg() * 2 : 0;
        std::fprintf(stderr, "[SPK] ch%d %s -> enabled=%d muted=%d play=%u format=%s rate=%u Hz volume=0x%02X\n",
                     channel, spk::command_name(command), s.enabled ? 1 : 0, s.muted ? 1 : 0,
                     unsigned(s.reg[8]), adpcm ? "ADPCM" : (s.format() == spk::kFormatPcm8 ? "PCM8" : "?"), rate,
                     unsigned(s.volume()));
        speaker_stats_tick(h, true);
    }
    // SDK: the command's last report (a status request) completes with WPAD_ERR_OK; an unknown command
    // queues nothing and reports the (OK) status directly.
    if (callback) ctx.queue_callback(callback, static_cast<uint32_t>(channel), static_cast<uint32_t>(kWpadOk));
    ctx.gpr[3] = static_cast<uint32_t>(kWpadOk);
}

// s32 WPADSendStreamData(s32 chan, void* p_buf, u16 len)
void SS_WPADSendStreamData(CPUContext& ctx) {
    namespace spk = nwii::runtime::speaker;
    const int32_t channel = static_cast<int32_t>(ctx.gpr[3]);
    const uint32_t buffer = ctx.gpr[4];
    const uint32_t length = ctx.gpr[5] & 0x1F;  // report 0x18 byte 0 = (len & 0x1F) << 3 (803DD318)
    SpeakerHle& h = speaker_hle();
    if (!speaker_channel_connected(channel)) {
        ++h.no_ctrl;
        ctx.gpr[3] = static_cast<uint32_t>(kWpadNoController);
        return;
    }
    ++h.reports;
    h.bytes += length;
    uint8_t data[0x20] = {};
    if (length <= uint32_t(spk::kMaxReportData))
        for (uint32_t i = 0; i < length; ++i) data[i] = ctx.mmu.read8(buffer + i);
    spk::SpeakerBlock block;
    const spk::ReportResult result =
        spk::speaker_report(h.spk[size_t(channel)], static_cast<uint8_t>(length), data, &block);
    switch (result) {
    case spk::ReportResult::Decoded:
        ++h.decoded;
        nwii::runtime::audio::push_speaker_samples(channel, block.samples.data(), block.count, block.rate,
                                                   block.gain() * h.gain);
        break;
    case spk::ReportResult::Muted: ++h.muted; break;
    case spk::ReportResult::BadLength: ++h.bad; break;
    case spk::ReportResult::Silent: ++h.silent; break;
    }
    if (h.raw) {
        const uint8_t n = length <= uint32_t(spk::kMaxReportData) ? uint8_t(length) : 0;
        const uint8_t head[3] = {uint8_t(channel), n, uint8_t(result)};
        std::fwrite(head, 1, 3, h.raw);
        std::fwrite(data, 1, n, h.raw);
        if ((h.reports & 255) == 0) std::fflush(h.raw);
    }
    speaker_stats_tick(h, false);
    ctx.gpr[3] = static_cast<uint32_t>(kWpadOk);
}

// BOOL WPADIsSpeakerEnabled(s32 chan)
void SS_WPADIsSpeakerEnabled(CPUContext& ctx) {
    const int32_t channel = static_cast<int32_t>(ctx.gpr[3]);
    ctx.gpr[3] = speaker_channel_connected(channel) && speaker_hle().spk[size_t(channel)].enabled ? 1 : 0;
}

}
