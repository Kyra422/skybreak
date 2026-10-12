#include "runtime/hw/hw.h"

namespace nwii::runtime::hw {
namespace {

constexpr uint32_t PSTAT = 0x01;
constexpr uint32_t AISFR = 0x02;
constexpr uint32_t AIINTMSK = 0x04;
constexpr uint32_t AIINT = 0x08;
constexpr uint32_t AIINTVLD = 0x10;
constexpr uint32_t SCRESET = 0x20;
constexpr uint32_t DMAFR = 0x40;
constexpr uint32_t WRITABLE_CONTROL = PSTAT | AISFR | AIINTMSK | AIINTVLD | DMAFR;
constexpr uint32_t PI_AI = 0x20;
constexpr uint64_t US_PER_SECOND = 1000000;

// Start stopped, with 48 kHz streaming and 32 kHz DMA selected.
uint32_t ai_cr = AISFR | DMAFR;
uint32_t ai_vr = 0;
uint32_t ai_scnt = 0;
uint32_t ai_it = 0;
uint64_t ai_last_us = 0;
// Fractional samples, in millionths, survive repeated sub-sample polling.
uint64_t ai_fraction = 0;

void update_interrupt() {
    if ((ai_cr & (AIINT | AIINTMSK)) == (AIINT | AIINTMSK))
        trigger_pi_interrupt(PI_AI);
    else
        clear_pi_interrupt(PI_AI);
}

void advance_counter(uint64_t now) {
    if (now <= ai_last_us)
        return;
    const uint64_t elapsed = now - ai_last_us;
    ai_last_us = now;
    if (!(ai_cr & PSTAT))
        return;

    const uint64_t rate = (ai_cr & AISFR) ? 48000 : 32000;
    // Split seconds from the remainder to avoid elapsed * rate overflowing.
    const uint64_t fraction = (elapsed % US_PER_SECOND) * rate + ai_fraction;
    const uint64_t samples = (elapsed / US_PER_SECOND) * rate + fraction / US_PER_SECOND;
    ai_fraction = fraction % US_PER_SECOND;
    const uint32_t previous = ai_scnt;
    ai_scnt += static_cast<uint32_t>(samples);

    // VLD freezes the comparator's interrupt latch; W1C remains effective.
    if (samples != 0 && !(ai_cr & AIINTVLD)) {
        uint64_t distance = static_cast<uint32_t>(ai_it - previous);
        if (distance == 0)
            distance = uint64_t{1} << 32;
        if (samples >= distance)
            ai_cr |= AIINT;
    }
}

uint32_t read_ai(uint32_t addr) {
    advance_counter(nwii::runtime::get_os_time());
    update_interrupt();
    switch (addr & 0xFF) {
    case 0x00: return ai_cr;
    case 0x04: return ai_vr;
    case 0x08: return ai_scnt;
    case 0x0C: return ai_it;
    default: return 0;
    }
}

void write_ai(uint32_t addr, uint32_t val) {
    const uint64_t now = nwii::runtime::get_os_time();
    // Settle elapsed samples at the old rate before changing control state.
    advance_counter(now);
    switch (addr & 0xFF) {
    case 0x00: {
        const bool playback_changed = ((ai_cr ^ val) & PSTAT) != 0;
        uint32_t status = ai_cr & AIINT;
        if (val & AIINT)
            status = 0;
        ai_cr = status | (val & WRITABLE_CONTROL);
        if (playback_changed) {
            ai_last_us = now;
            ai_fraction = 0;
        }
        if (val & SCRESET) {
            ai_scnt = 0;
            ai_last_us = now;
            ai_fraction = 0;
        }
        break;
    }
    case 0x04:
        ai_vr = val;
        break;
    case 0x08:
        ai_scnt = val;
        ai_last_us = now;
        ai_fraction = 0;
        break;
    case 0x0C:
        ai_it = val;
        break;
    }
    update_interrupt();
}

} // namespace

void register_ai(MMIODispatcher& dispatcher) {
    ai_cr = AISFR | DMAFR;
    ai_vr = ai_scnt = ai_it = 0;
    ai_last_us = nwii::runtime::get_os_time();
    ai_fraction = 0;
    update_interrupt();
    // GameCube/Wii MMIO addresses and their physical aliases.
    for (const uint32_t base : {0xCC006C00u, 0x0C006C00u, 0xCD006C00u, 0x0D006C00u})
        dispatcher.register_region(base, base + 0x0F, read_ai, write_ai);
}

void ai_update() {
    advance_counter(nwii::runtime::get_os_time());
    update_interrupt();
}

// AID (DMA) sample rate selected by AI_CR.DMAFR: set = 32 kHz, clear = 48 kHz
// (Dolphin AudioInterface AICR.AIDFR; libogc AUDIO_GetDSPSampleRate).
uint32_t ai_dma_sample_rate() {
    return (ai_cr & DMAFR) ? 32000 : 48000;
}

} // namespace nwii::runtime::hw
