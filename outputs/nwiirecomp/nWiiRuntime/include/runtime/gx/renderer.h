#pragma once
#include <vector>
#include <memory>
#include <cstdint>
#include "runtime/gx_command.h"

namespace nwii::runtime::gx {

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void Initialize(void* window) = 0;
    virtual void Render(const std::vector<GXCommand>& commands) = 0;
    virtual void Present() = 0;

    static std::unique_ptr<IRenderer> Create();
};

// RENDER1 (roadmap M1.3): EFB and presentation geometry.
// The EFB is 640x528 like the hardware's (EGG keeps its work buffer at
// (640-w, 528-h)). The host binds an FBO of this size (RGBA8 colour texture +
// depth texture) before ProcessGXFifo; the renderer uses whatever draw
// framebuffer is bound and reads its real size, so an old 640x480 FBO still
// works. GX row 0 is the top row: GL row = height - 1 - GX row.
constexpr int kEfbWidth = 640;
constexpr int kEfbHeight = 528;
// What the VI shows: the source rect of the last XFB copy (BP 0x52 bit 14),
// scaled into a renderer-owned 640x480 framebuffer at copy time. Window
// presentation and screenshots read this, not the EFB.
constexpr int kPresentWidth = 640;
constexpr int kPresentHeight = 480;
// RES9 (roadmap M7.3): internal resolution (Dolphin "internal resolution").
// The host may bind an EFB of (kEfbWidth*s) x (kEfbHeight*s), s = 1..
// kMaxInternalScale. The renderer derives s from the bound colour attachment
// (width an exact multiple of kEfbWidth, else 1) and scales the XF viewport,
// BP scissor, copy-clear and EFB copy source rects by s. EFB->texture copies
// are rendered at s times their GX size; GX texture coordinates are normalised
// by the GX size, so sampling is unchanged. The present framebuffer is
// (kPresentWidth*s) x (kPresentHeight*s). s = 1 is bit-identical to before.
constexpr int kMaxInternalScale = 6;
// s of the bound EFB as of the last Render (1 before the first one).
int InternalScale();
// GL framebuffer holding the last XFB copy; 0 before the first one.
unsigned PresentFramebuffer();
// Size of the present framebuffer (kPresent* times the internal scale).
void PresentSize(int& w, int& h);
// Reads the presented image (kPresentWidth x kPresentHeight RGBA, GL row
// order = bottom row first). Black before the first XFB copy. Restores the
// read-framebuffer binding. False if no GL context was ever used. At internal
// scale s > 1 this is the s x s box-filtered (640x480-equivalent) image, so
// screenshots, probes and goldens keep their native size.
bool ReadPresented(std::vector<uint8_t>& rgba);
// The presented image at the present framebuffer's full size (w x h).
bool ReadPresentedFull(std::vector<uint8_t>& rgba, int& w, int& h);

// PEEK10 (roadmap M3.6): CPU access to the EFB (physical 0x08000000 window,
// Dolphin MMU EFB_Read/EFB_Write; the address decode and value conversions
// live in hw_pe.cpp). The renderer snapshots the EFB at native size right
// before each EFB->XFB copy while the guest peeks and downloads it
// asynchronously; see renderer_gl.cpp. x, y are GX EFB pixels (top-left
// origin), clamped to the EFB. Thread-safe (CPU thread).
// EfbPeek: depth -> *z = GL depth (GX Z / 2^24) of the newest snapshot;
// colour -> rgba = RGBA8. *pixel_format = BP 0x43 & 7 at snapshot time.
// Returns false (outputs untouched) before the first snapshot exists.
bool EfbPeek(bool depth, int x, int y, float* z, uint8_t rgba[4], uint32_t* pixel_format);
// Queues a poke (depth: 24-bit Z; colour: 0xAARRGGBB) for the render thread
// and patches the published snapshot.
void EfbPoke(bool depth, int x, int y, uint32_t value);
// Render thread: publish a finished download (wait = block up to 100 ms).
void EfbAccessPoll(bool wait);
struct EfbAccessStats {
    uint64_t peeks_z = 0, peeks_c = 0;   // EfbPeek calls
    uint64_t peeks_before_snapshot = 0;  // ... answered with "no snapshot yet"
    uint64_t pokes = 0;
    uint64_t snapshots = 0;              // downloads started (at XFB copies)
    uint64_t published = 0;              // downloads completed
    uint64_t busy_skips = 0;             // XFB copies skipped: download in flight
};
EfbAccessStats GetEfbAccessStats();

// Renderer counters, cumulative (gxreplay / [GXPROF] report deltas).
struct RendererStats {
    uint64_t draws = 0;          // DrawPrimitive commands seen
    uint64_t flushes = 0;        // glDrawArrays batches
    uint64_t vertices = 0;       // vertices submitted
    uint64_t state_syncs = 0;    // draws that re-evaluated dirty state
    uint64_t program_binds = 0;  // glUseProgram
    uint64_t uniform_uploads = 0;// uniform groups uploaded
    uint64_t tex_binds = 0;      // glBindTexture for draws
    uint64_t tex_lookups = 0;    // texture-cache lookups (per-frame memo misses)
    uint64_t compiles = 0;       // shader programs compiled
    uint64_t compile_us = 0;     // time spent compiling
    uint64_t copies = 0;         // EFB->texture copies
    uint64_t xfb_copies = 0;     // EFB->XFB copies
    uint64_t clears = 0;         // copy-with-clear (ClearScreen) executions
    uint64_t skipped = 0;        // draws skipped (shader not compiled yet)
};
const RendererStats& GetRendererStats();

}
