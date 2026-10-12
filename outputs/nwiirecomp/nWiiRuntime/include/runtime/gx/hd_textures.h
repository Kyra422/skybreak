#pragma once
// HDTEX16: Dolphin-compatible custom ("HD") textures and texture dumping for the GL texture cache.
//
// HdTexInit() (main.cpp, after the launcher) reads [graphics] (runtime/hdtex_config.h) + env, indexes
// the pack folders (recursive, tex1_*.png / .dds, Dolphin names, see dolphin_texname.h) and installs
// a TextureReplaceHook in texture_cache_gl.cpp when a pack is active or dumping is on; otherwise
// nothing changes. Per new texture (cache miss, GL thread): Dolphin name -> pack lookup (exact,
// "_$_" tlut wildcard, "_$" texture wildcard, then the "_m"-toggled name unless NWII_HDTEX_STRICT=1).
// Pack files whose "_m"-toggled twin has the same size + write time (hard links) share one entry.
// A hit is decoded on worker threads (WIC PNG / DDS) into a RAM cache; the GL thread uploads it into
// the cache's GL texture within a per-frame byte budget (the original stays visible until then).
// GX coordinate math is untouched: texcoords are normalised with the ORIGINAL size (uTexDim), wrap /
// LOD bias / min LOD come from the GX registers; the sampler gets max LOD 255/16 for custom textures
// (Dolphin GetSamplerState). Mipmaps: the pack's _mipN files; else generated (glGenerateMipmap) when
// the original texture is mipmapped (NWII_HDTEX_GENMIPS=0 = Dolphin: base level only).
// EFB copies never reach the hook (TextureCache::get_texture returns them before any lookup).
//
// Env (override the settings): NWII_HD_TEXTURES=0/1, NWII_HDTEX_PRELOAD=0/1, NWII_TEX_DUMP=0/1,
// NWII_TEX_DUMP_DIR=<dir>, NWII_TEXTURE_PACK_DIR=<dir> (only this folder), NWII_HDTEX_UPLOAD_MB (per
// frame, default 32), NWII_HDTEX_UPLOAD_MS (per frame, default 3; one upload always proceeds),
// NWII_HDTEX_CACHE_MB (RAM cache, default 1536; preload: 1/4 of the RAM, 1536..8192), NWII_HDTEX_THREADS
// (default 2; preload: cores/4, 2..6; workers run below normal priority),
// NWII_HDTEX_GENMIPS=0, NWII_HDTEX_STRICT=1, NWII_HDTEX_SYNC=1 (decode on the GL thread at the miss:
// tests), NWII_HDTEX_LOG=1 (one line per hit/miss name).
// HDTEX17: an uploaded custom image is a hook-owned GL texture per pack entry (and mip variant), shared by every
// texture-cache entry that maps to it and kept across texture-cache clears, so a clear (cache full) or a second
// address / "_m" twin of the same image costs no upload and shows no low-res frames; a shared texture that stayed
// unused for a whole cache generation is deleted. NWII_HDTEX_SHARE=0 = HDTEX16 behaviour (upload into the cache's
// own texture). Diagnostics: NWII_HDTEX_ARBCHECK=1 logs hits whose ORIGINAL has arbitrary (artist-made) mips.
// HDTEX17 also: hybrid mip chain (HD levels down to the original size, then the game's own mips; the sampler keeps the
// GX max LOD in original units; NWII_HDTEX_ORIGMIPS=0 = all generated), CMPR originals: RGB 0 under alpha 0 like the
// original data (NWII_HDTEX_CMPR_ALPHA0=0 = off), a built-in SOUE01 exclusion list of originals whose upscale looks
// wrong (NWII_HDTEX_EXCLUDE=none | name,name,...).
#include <cstdint>
#include <string>
#include <vector> // CTRL18-UI

namespace nwii::runtime::hle {

void HdTexInit();

// CTRL18-UI: controller glyph sets (runtime/gx/glyph_sets.h). `folder` = a set folder under
// <game root>/textures/glyphs/<game id>/ ("ps-hd", "kbm", ...), "" = the original Wii pictures. Thread-safe; the switch
// happens at the next frame start (GL thread) and clears the texture cache once. A call before HdTexInit decides the
// start set. HdTexGlyphSet() = the active folder ("" = none), HdTexGlyphSets() = the folders found at start.
void HdTexSetGlyphSet(const std::string& folder);
std::string HdTexGlyphSet();
std::vector<std::string> HdTexGlyphSets();
uint64_t HdTexGlyphHits(); // new textures replaced from the active glyph set (tests)

struct HdTexStats {
    bool active = false, dump = false;
    uint64_t pack_files = 0, pack_textures = 0, alias_files = 0; // alias: "_m" twin = same file (shared entry)
    uint64_t hits[4] = {0, 0, 0, 0}; // per lookup stage (exact, wild_tlut, wild_tex, m_toggle), per new texture
    uint64_t misses = 0;
    uint64_t uploads = 0, uploads_immediate = 0, upload_bytes = 0;
    uint64_t decoded = 0, decode_failed = 0, pending = 0;
    uint64_t ram_bytes = 0, vram_bytes = 0;
    uint64_t dumped = 0, dump_dropped = 0;
    double index_ms = 0, max_upload_ms_frame = 0;
    uint64_t shared_textures = 0, shared_bytes = 0, shared_evicted = 0, cache_clears = 0, reuploads = 0; // HDTEX17
};
HdTexStats HdTexGetStats();
std::string HdTexStatLine();

} // namespace nwii::runtime::hle
