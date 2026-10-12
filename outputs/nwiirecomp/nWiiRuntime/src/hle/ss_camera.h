#pragma once
// CTRL18-CAM: SOUE01 free camera (opt-in game hook) + per-frame tick. See ss_camera.cpp and
// work/claude-notes/CTRL18-CAM.md. Declarations for the unit tests (outputs/pc-tests/ctrl18cam).
#include <cstdint>

namespace nwii::runtime {
struct CPUContext;
}

// [hle_hooks] "8006BF50" = "SS_CamGameExecuteSeam": replaces the empty function fn_8006BF50 (a single blr) that
// dCameraGame_c::execute (0x8007EAD0) calls once per frame at 0x8007EB08, r28 = the game camera.
extern "C" void SS_CamGameExecuteSeam(nwii::runtime::CPUContext& ctx);
// [hle_hooks] "803DB1E0" = "SS_WPADControlMotor" (ss_rumble.cpp).
extern "C" void SS_WPADControlMotor(nwii::runtime::CPUContext& ctx);

namespace nwii::runtime::ss_camera {

struct Stats {
    uint64_t ticks = 0;          // guest frames (frame tick calls)
    uint64_t seam_calls = 0;     // SS_CamGameExecuteSeam calls
    uint64_t gate_open = 0;      // seam calls with the gate open
    uint64_t orbit_writes = 0;   // seam calls that wrote mView2's eye (the only game write)
    uint64_t guest_reads = 0;    // guest memory reads by the seam
    uint64_t guest_writes = 0;   // guest memory words written by the seam (3 per orbit write)
};
Stats stats();
// The frame tick (also installed as input::g_ss_frame_tick): latches this frame's orbit sample, clears a stale gate
// and drives the rumble forwarding (ss_rumble.cpp).
void frame_tick(nwii::runtime::CPUContext& ctx);
// Tests: forget all host-side state and re-read the env overrides at the next call.
void reset_for_test();
// Pure maths (host unit test): rotate eye around at by dyaw (deg, > 0 = view turns right) and dpitch (deg, > 0 =
// look up = eye lower), keeping the distance; the elevation is clamped to [kElMinDeg, kElMaxDeg] (never pushed
// further outside when it already is). Returns false (out untouched) for degenerate input.
inline constexpr float kElMinDeg = -20.0f, kElMaxDeg = 65.0f, kMaxStepDeg = 10.0f;
bool orbit_eye(const float eye[3], const float at[3], float dyaw_deg, float dpitch_deg, float out[3]);

} // namespace nwii::runtime::ss_camera

namespace nwii::runtime::ss_rumble {
// Once per guest frame (called by ss_camera::frame_tick): forwards the game's motor state to the pads.
void frame_tick();
struct Stats {
    uint64_t motor_calls = 0;    // SS_WPADControlMotor calls
    uint64_t motor_on_calls = 0; // ... with cmd != 0
    uint64_t on_frames = 0;      // frames with the game's motor on (any channel)
    uint64_t pad_requests = 0;   // requests posted to pad_hub
};
Stats stats();
void reset_for_test();
} // namespace nwii::runtime::ss_rumble
