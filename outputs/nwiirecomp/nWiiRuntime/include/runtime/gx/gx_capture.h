#pragma once
// GX tooling (roadmap M0.2 / M0.4): screenshots, per-frame GX event log and
// frame capture / offline replay. Everything here is diagnostics only: with
// none of the env vars below set, the runtime behaves exactly as before.
//
//   NWII_GXFRAMELOG=<first>:<count>   ordered per-draw / per-EFB-copy log of
//                                     STAT frames [first, first+count);
//   NWII_GXFRAMELOG=cap               ... or exactly the NWII_GXCAPTURE frames,
//                                     into <capture dir>/live_gxframelog.txt
//   NWII_GXFRAMELOG_FILE=<path>       log file (default gxframelog-<NWII_SCREENSHOT name>.txt
//                                     next to the screenshots, else ./gxframelog.txt)
//   NWII_GXCAPTURE=<spec>[;<spec>...] spec = [name:]start[,count]
//                                     start = <frame> (STAT frame number) or
//                                     @<hexaddr>=<hexval>[+<delay>]: the first
//                                     rendered frame + delay after the guest u32
//                                     at addr equals val (read-only watch);
//                                     optional %N suffix moves the window so its
//                                     last frame is a multiple of N
//   NWII_GXCAPTURE_DIR=<dir>          base directory; one sub-directory per spec
//
// The capture files are written and read by gx.cpp only, so the live runtime
// and the gxreplay tool (outputs/pc-tests/gxreplay) share one implementation.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "runtime/gx_command.h"

namespace nwii::runtime::gx {

// ---- images (gx.cpp) --------------------------------------------------------
// rgba is GL readback order (row 0 = bottom). mode 0: RGB as is, alpha forced
// to 255. mode 1: the alpha channel as an opaque grey image.
bool SaveBMP32(const std::string &path, const uint8_t *rgba, int w, int h, int mode);
// Loads a 32-bit BMP written by SaveBMP32 (or SDL_SaveBMP 32bpp) into GL order RGBA.
bool LoadBMP32(const std::string &path, std::vector<uint8_t> &rgba, int &w, int &h);

// ---- frame event log (gx.cpp opens; renderer_gl.cpp writes events) ---------
// Non-null only while the frame being rendered lies inside NWII_GXFRAMELOG;
// *frame receives the STAT frame number.
FILE *FrameLogFile(uint64_t *frame);

// ---- hooks: defined (null) in the file that calls them, installed by gx.cpp,
// so fifo_parser.cpp / renderer_gl.cpp still link standalone in unit tests. --
// fifo_parser.cpp: parse-time guest-RAM reads, non-null only while a capture
// is recording the current ProcessGXFifo call.
extern void (*g_capNoteDL)(uint32_t addr, uint32_t size, const uint8_t *bytes);
extern void (*g_capNoteRAMFloats)(uint32_t addr, const float *vals, int n);
// renderer_gl.cpp: frame-log file for the frame being rendered (null = off)
// and EFB->texture copy destinations (null unless a capture is configured).
extern FILE *(*g_rendererFrameLogHook)(uint64_t *frame);
extern void (*g_rendererEfbCopyHook)(uint32_t dest);
// INTERP16 (gx.cpp, null by default; set only by outputs/pc-tests/interp16):
// called with each cut frame right before its draws are decoded and rendered
// (after the replay loaded the frame's RAM image).
extern void (*g_interpFrameHook)(std::vector<GXCommand> &frame, uint64_t frame_no);

// ---- parser shadow state (fifo_parser.cpp) ----------------------------------
void ParserShadowGet(float pos[256], float nrm[96]);
void ParserShadowSet(const float pos[256], const float nrm[96]);
using DLCacheVisitor = void (*)(void *user, uint64_t key, uint64_t state_hash,
                                uint64_t content_hash,
                                const std::vector<GXCommand> &cmds);
void ParserDLCacheVisit(DLCacheVisitor fn, void *user);
void ParserDLCacheClear();
// RENDER1 (M1.2): parse-only BP/XF register shadows (fifo_parser.cpp). The
// replay seeds them from the captured g_state; also resets both BP masks.
void ParserRegShadowSet(const uint32_t bp[256], const uint32_t xf[256]);
const uint32_t *ParserBPShadow();
const uint32_t *ParserXFShadow();
void ParserDLCachePut(uint64_t key, uint64_t state_hash, uint64_t content_hash,
                      std::vector<GXCommand> &&cmds);

// ---- renderer hooks (renderer_gl.cpp) ---------------------------------------
// Per-frame shader compile budget (live default 12). gxreplay raises it: its
// shader cache starts cold, the live one is warm.
void RendererSetCompileBudget(int n);
// Draws skipped so far because their shader was not compiled yet.
uint64_t RendererSkippedDraws();

// ---- offline replay (gx.cpp; used by outputs/pc-tests/gxreplay) -------------
// Restores the capture's start state (g_state, parser shadow + DL cache,
// unparsed FIFO bytes, parsed-but-unrendered commands, EFB colour, EFB-copy
// textures) into the current GL context / currently bound FBO. g_mmu must
// point at an MMU the replay owns.
bool ReplayOpen(const std::string &dir, std::string &err);
// Feeds the next recorded ProcessGXFifo call. Returns the STAT frame number it
// rendered, 0 if it rendered none, -1 at end of capture or on error.
int64_t ReplayStep(std::string &err);
void ReplayClose();

} // namespace nwii::runtime::gx
