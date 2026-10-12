#pragma once

#include <array>
#include <cstdint>

namespace nwii::runtime {
struct CPUContext;
namespace input { struct MotionSample; }

// A dispatcher executes one generated function/resume at ctx.pc. The bridge
// supplies a bounded return sentinel and preserves the caller's registers.
using SSMotionDispatch = void (*)(CPUContext&);

struct SSMotionGuestMemory {
    uint32_t workarea = 0;
    uint32_t workarea_size = 0;
    // Caller-owned original KPAD storage, at least 16 * 0x42 bytes per channel.
    std::array<uint32_t, 4> raw_rings{};
};

inline constexpr uint32_t kSSMotionWorkSize = 0x36E0;
inline constexpr uint32_t kSSMotionRawRingSize = 16 * 0x42;

void ss_motion_reset();
bool ss_motion_initialized();
void ss_motion_initialize(CPUContext&, const SSMotionGuestMemory&, SSMotionDispatch);

// Returns true only when a new, fresh virtual sample was consumed. The caller
// supplies a complete 0xF0 KPADStatus; original KMPLS writes its 0x3C output at
// +0xB0 and reads the existing acceleration/pointer prefix for revision.
bool ss_motion_read(CPUContext&, int32_t channel, const input::MotionSample&,
                    uint64_t now_ns, uint32_t kpad_status);
// Address of the most recently consumed raw sample, or zero after reset.
uint32_t ss_motion_latest_raw(int32_t channel);
void ss_motion_reset_channel(CPUContext&, int32_t channel);

// Handle only the library's calibration command 0x80. Other configuration
// commands belong to the caller's WPAD state machine and return false here.
// Acknowledgement is delivered through the original guest callback on the next
// fresh sample, never by writing KMPLS calibration progress or completion flags.
bool ss_motion_control_request(CPUContext&, int32_t channel, uint32_t command,
                               uint32_t guest_callback);
void ss_motion_write_calibration(CPUContext&, int32_t channel,
                                 uint32_t fast_record, uint32_t slow_record);
}

extern "C" void SS_WPADiGetMplsCalibration(nwii::runtime::CPUContext&);
