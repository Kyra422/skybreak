#pragma once
// INTERP17: [graphics] frame interpolation keys (60 fps through in-between frames). Parsed by Config::load
// (src/core/config.cpp, load_interp17) and written by the settings UI; header-only, so gx.cpp (also linked
// into gxreplay without config.cpp) can read it. Design: work/claude-notes/INTERP16-plan.md, INTERP17.md.
//
// [graphics]
//   frame_interpolation = false        draw a computed in-between frame between every two game frames
//                                      (60 fps from the game's 30). Read every frame: the in-game menu
//                                      toggles it live. Env NWII_FRAME_INTERP=0/1 overrides.
//   frame_interpolation_2d = true      2D layers (HUD, text boxes) interpolated too, with a jump guard;
//                                      false = 2D from the newer frame. Env NWII_INTERP_2D=0/1.
//   frame_interpolation_mode = "auto"  how the in-between frame is made (env NWII_INTERP_MODE):
//                                      "rerender" = INTERP17: the frame drawn again with interpolated
//                                      matrices (exact geometry; the host CPU must fit two renders per
//                                      guest frame, else that in-between frame is skipped = 30 fps there);
//                                      "warp" = MVGEN21: the real frames' images warped on the GPU along
//                                      per-pixel motion vectors, HUD/2D from the newer frame (CPU ~ the 2D
//                                      tail only); "auto" (experimental) = rerender when it fits, else warp;
//                                      "dup" = A/B control: the same 60 Hz pacing and latency, but the
//                                      in-between frame repeats the older frame. "interp" (INTERP17's old
//                                      default value) = "auto".
#include <atomic>
#include <cstdlib>
#include <string>

namespace nwii::runtime {

struct InterpConfig {
    std::atomic<int> enabled{0}; // the setting; FrameInterpOn() applies the env override
    std::atomic<int> interp_2d{1};
    std::atomic<int> dup_mode{0}; // frame_interpolation_mode = "dup"
    std::atomic<int> mode{1};     // MVGEN21: kInterpAuto / kInterpRerender / kInterpWarp (dup: dup_mode)
};
enum : int { kInterpAuto = 0, kInterpRerender = 1, kInterpWarp = 2 }; // MVGEN21
// MVGEN21: the frame_interpolation_mode value -> (mode, dup); false = not a mode name.
inline bool ParseInterpMode(const std::string &v, int &mode, int &dup) {
    if (v == "dup") { dup = 1; mode = kInterpAuto; return true; }
    if (v == "auto") { dup = 0; mode = kInterpAuto; return true; }
    if (v == "rerender" || v == "interp") { dup = 0; mode = kInterpRerender; return true; }
    if (v == "warp") { dup = 0; mode = kInterpWarp; return true; }
    return false;
}
inline const char *InterpModeName(int mode, bool dup) {
    return dup ? "dup" : mode == kInterpRerender ? "rerender" : mode == kInterpWarp ? "warp" : "auto";
}
inline InterpConfig& interp_config() {
    static InterpConfig c;
    return c;
}
namespace interp_env {
inline int Flag(const char* name) { // -1 unset, else 0/1
    const char* e = std::getenv(name);
    if (!e || !*e) return -1;
    return *e == '0' ? 0 : 1;
}
} // namespace interp_env
// Effective switches (env overrides the setting).
inline bool FrameInterpOn() {
    static const int env = interp_env::Flag("NWII_FRAME_INTERP");
    return env >= 0 ? env == 1 : interp_config().enabled.load(std::memory_order_relaxed) != 0;
}
inline bool FrameInterp2D() {
    static const int env = interp_env::Flag("NWII_INTERP_2D");
    return env >= 0 ? env == 1 : interp_config().interp_2d.load(std::memory_order_relaxed) != 0;
}
inline bool FrameInterpDup() {
    static const int env = [] {
        const char* e = std::getenv("NWII_INTERP_MODE");
        if (!e || !*e) return -1;
        return std::string(e) == "dup" ? 1 : 0;
    }();
    return env >= 0 ? env == 1 : interp_config().dup_mode.load(std::memory_order_relaxed) != 0;
}
// MVGEN21: how the in-between frame is made (env NWII_INTERP_MODE = auto|interp|rerender|warp overrides).
inline int FrameInterpMode() {
    static const int env = [] {
        const char *e = std::getenv("NWII_INTERP_MODE");
        int m = -1, d = 0;
        if (!e || !*e || !ParseInterpMode(e, m, d) || d) return -1;
        return m;
    }();
    return env >= 0 ? env : interp_config().mode.load(std::memory_order_relaxed);
}

} // namespace nwii::runtime
