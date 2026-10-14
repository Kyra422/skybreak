#pragma once
// HDTEX16: Dolphin-compatible custom ("HD") texture names. Pure C++ (no GL, no OS), header-only so the
// unit tests (outputs/pc-tests/hdtex16) and offline tools use exactly the runtime's code.
//
// Name rules (Dolphin VideoCommon TextureInfo.cpp CalculateTextureName / HiresTextures.cpp, master 2026):
//   tex1_<w>x<h>[_m]_<texhash>[_<tluthash>]_<fmt>
//   w, h      raw GX size (TEXIMAGE0 width+1 / height+1, not block aligned)
//   _m        TEXMODE0 mip filter != none (the "mipmaps enabled" bit; NOT the number of levels)
//   texhash   XXH64, seed 0, over the BASE level only: the block-aligned size
//             TexDecoder_GetTextureSizeInBytes(AlignUp(w, bw), AlignUp(h, bh), fmt); %016x
//   tluthash  C4/C8/C14X2 only: XXH64 seed 0 over the USED palette range, 2*(max+1-min) bytes from
//             tlut + 2*min; min/max of the indices over the whole base level (C4 both nibbles, C8 bytes,
//             C14X2: Dolphin's Common::swap16(u8) quirk = (byte << 8) & 0x3fff of every even byte); %016x
//   fmt       GX texture format number in decimal (0..14)
// Lookup order (HiresTexture GetNameArbPair): exact, "<base>_<texhash>_$_<fmt>" (any palette),
// "<base>_$[_<tluthash>]_<fmt>" (any texel data). A pack file "<name>_arb.png" is stored under <name> with
// the arbitrary-mipmap flag; "<name>_mip<N>.png" next to it is mip level N (first missing N ends the chain).
// Dumps: "<name>.png" and, for mipmapped textures, "<name>_mip<N>.png" (Dolphin DumpMipTextures default on);
// "_arb" only when Dolphin's ArbitraryMipmapDetection is on (default OFF).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace nwii::runtime::gx::hdtex {

// ---- XXH64 (public xxHash specification, Yann Collet; written from the spec) -------------------------
namespace detail {
constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                   P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t rd64(const uint8_t* p) { // little endian, like xxHash on x86
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint64_t round(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
inline uint64_t merge(uint64_t acc, uint64_t v) { return (acc ^ round(0, v)) * P1 + P4; }
} // namespace detail

inline uint64_t XXH64(const void* data, size_t len, uint64_t seed = 0) {
    using namespace detail;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* const end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* const limit = end - 32;
        do {
            v1 = round(v1, rd64(p)); v2 = round(v2, rd64(p + 8));
            v3 = round(v3, rd64(p + 16)); v4 = round(v4, rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
    } else {
        h = seed + P5;
    }
    h += uint64_t(len);
    while (p + 8 <= end) { h = rotl(h ^ round(0, rd64(p)), 27) * P1 + P4; p += 8; }
    if (p + 4 <= end) { h = rotl(h ^ (uint64_t(rd32(p)) * P1), 23) * P2 + P3; p += 4; }
    while (p < end) { h = rotl(h ^ (uint64_t(*p) * P5), 11) * P1; ++p; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

// ---- GX texture geometry (Dolphin TexDecoder_GetBlockWidth/HeightInTexels, TexelSizeInNibbles) --------
inline uint32_t BlockW(uint32_t fmt) {
    switch (fmt) { case 0: case 1: case 2: case 8: case 9: case 14: return 8; default: return 4; }
}
inline uint32_t BlockH(uint32_t fmt) {
    switch (fmt) { case 0: case 8: case 14: return 8; case 1: case 2: case 9: return 4; default: return 4; }
}
inline uint32_t BitsPerTexel(uint32_t fmt) {
    switch (fmt) { case 0: case 8: case 14: return 4; case 1: case 2: case 9: return 8; case 6: return 32; default: return 16; }
}
inline uint32_t LevelSizeBytes(uint32_t w, uint32_t h, uint32_t fmt) {
    const uint32_t bw = BlockW(fmt), bh = BlockH(fmt);
    const uint32_t ew = (w + bw - 1) / bw * bw, eh = (h + bh - 1) / bh * bh;
    return ew * eh * BitsPerTexel(fmt) / 8;
}
inline bool IsPaletted(uint32_t fmt) { return fmt == 8 || fmt == 9 || fmt == 10; }
inline const char* FormatName(uint32_t fmt) {
    switch (fmt) {
    case 0: return "I4"; case 1: return "I8"; case 2: return "IA4"; case 3: return "IA8"; case 4: return "RGB565";
    case 5: return "RGB5A3"; case 6: return "RGBA8"; case 8: return "C4"; case 9: return "C8"; case 10: return "C14X2";
    case 14: return "CMPR"; default: return "?";
    }
}

struct NameParts {
    std::string base;   // tex1_<w>x<h>[_m]
    std::string tex;    // %016x
    std::string tlut;   // "_%016x" or ""
    std::string fmt;    // decimal
    std::string Full() const { return base + "_" + tex + tlut + "_" + fmt; }
    std::string WildTlut() const { return base + "_" + tex + "_$_" + fmt; } // any palette
    std::string WildTex() const { return base + "_$" + tlut + "_" + fmt; }  // any texel data
    // HDTEX16 extension (not Dolphin): the same name with the "_m" flag toggled, for a pack dumped
    // while the game sampled this texture with the other mip filter setting.
    std::string FullMipToggled() const {
        std::string b = base;
        if (b.size() > 2 && b.compare(b.size() - 2, 2, "_m") == 0) b.resize(b.size() - 2);
        else b += "_m";
        return b + "_" + tex + tlut + "_" + fmt;
    }
};

inline std::string Hex16(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

// Palette index range of the base level (Dolphin CalculateTextureName). Returns false when not paletted.
inline bool PaletteRange(const uint8_t* data, uint32_t size, uint32_t fmt, uint32_t& mn, uint32_t& mx) {
    if (!IsPaletted(fmt)) return false;
    uint32_t lo = 0xFFFF, hi = 0;
    if (fmt == 8) {
        for (uint32_t i = 0; i < size; ++i) {
            const uint32_t a = data[i] & 0xF, b = data[i] >> 4;
            lo = a < lo ? a : lo; lo = b < lo ? b : lo;
            hi = a > hi ? a : hi; hi = b > hi ? b : hi;
        }
    } else if (fmt == 9) {
        for (uint32_t i = 0; i < size; ++i) {
            const uint32_t a = data[i];
            lo = a < lo ? a : lo; hi = a > hi ? a : hi;
        }
    } else { // C14X2: Common::swap16(m_ptr[i]) of a u8 -> (byte << 8), & 0x3fff (Dolphin quirk, kept exactly)
        for (uint32_t i = 0; i < size; i += 2) {
            const uint32_t a = (uint32_t(data[i]) << 8) & 0x3FFF;
            lo = a < lo ? a : lo; hi = a > hi ? a : hi;
        }
    }
    mn = lo; mx = hi;
    return true;
}

// data: the base level (at least LevelSizeBytes(w, h, fmt) bytes). tmem/tmem_size: the TLUT memory and
// tlut_offset = TEXTLUT tmem_offset << 9 (Dolphin &texMem[tlutaddr]); bytes past tmem_size hash as 0.
inline NameParts CalcName(const uint8_t* data, uint32_t w, uint32_t h, uint32_t fmt, bool mip_enabled,
                          const uint8_t* tmem, size_t tmem_size, uint32_t tlut_offset) {
    NameParts n;
    n.base = "tex1_" + std::to_string(w) + "x" + std::to_string(h) + (mip_enabled ? "_m" : "");
    const uint32_t size = LevelSizeBytes(w, h, fmt);
    n.tex = Hex16(XXH64(data, size, 0));
    uint32_t mn = 0, mx = 0;
    if (PaletteRange(data, size, fmt, mn, mx) && mx + 1 > mn) {
        const size_t tl_size = size_t(2) * (mx + 1 - mn);
        const size_t start = size_t(tlut_offset) + size_t(2) * mn;
        if (tmem && start + tl_size <= tmem_size) {
            n.tlut = "_" + Hex16(XXH64(tmem + start, tl_size, 0));
        } else {
            std::vector<uint8_t> tmp(tl_size, 0);
            if (tmem && start < tmem_size) std::memcpy(tmp.data(), tmem + start, tmem_size - start);
            n.tlut = "_" + Hex16(XXH64(tmp.data(), tl_size, 0));
        }
    }
    n.fmt = std::to_string(fmt);
    return n;
}

// ---- pack file names -----------------------------------------------------------------------------
struct PackFileName {
    bool ok = false;
    std::string key;   // name used for the lookup (without _arb / _mipN / extension)
    bool arb = false;  // "_arb" before the extension (or before _mipN)
    int mip = 0;       // N of "_mipN", 0 for the base level
    bool dds = false;
};

inline std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// file: a file name without directory, e.g. "tex1_64x64_m_0123456789abcdef_14_mip2.png".
inline PackFileName ParsePackFileName(const std::string& file) {
    PackFileName r;
    const size_t dot = file.rfind('.');
    if (dot == std::string::npos) return r;
    const std::string ext = Lower(file.substr(dot));
    if (ext != ".png" && ext != ".dds") return r;
    r.dds = ext == ".dds";
    std::string stem = file.substr(0, dot);
    if (stem.compare(0, 5, "tex1_") != 0) return r;
    const size_t mp = stem.rfind("_mip");
    if (mp != std::string::npos && mp + 4 < stem.size()) {
        bool digits = true;
        for (size_t i = mp + 4; i < stem.size(); ++i) digits = digits && stem[i] >= '0' && stem[i] <= '9';
        if (digits) {
            r.mip = std::atoi(stem.c_str() + mp + 4);
            stem.resize(mp);
        }
    }
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "_arb") == 0) {
        r.arb = true;
        stem.resize(stem.size() - 4);
    }
    r.key = stem;
    r.ok = true;
    return r;
}

// Parses "tex1_<w>x<h>..." -> native size (0 on failure); used for the pack size checks.
inline bool NativeSizeFromName(const std::string& key, uint32_t& w, uint32_t& h) {
    unsigned a = 0, b = 0;
    if (std::sscanf(key.c_str(), "tex1_%ux%u", &a, &b) != 2 || !a || !b) return false;
    w = a; h = b;
    return true;
}

// ---- lookup (HiresTexture GetNameArbPair order + one HDTEX16 extension) ------------------------------
enum LookupStage : int { kMiss = -1, kExact = 0, kWildTlut = 1, kWildTex = 2, kMipToggle = 3, kStageCount = 4 };
inline const char* StageName(int s) {
    switch (s) { case kExact: return "exact"; case kWildTlut: return "wild_tlut"; case kWildTex: return "wild_tex";
                 case kMipToggle: return "m_toggle"; default: return "miss"; }
}
// map: pack key -> entry id. strict = Dolphin's three names only (no "_m" toggle).
template <class Map>
inline int FindInPack(const Map& map, const NameParts& n, bool strict, int* stage) {
    std::string names[kStageCount] = {n.Full(), n.WildTlut(), n.WildTex(), strict ? std::string() : n.FullMipToggled()};
    for (int s = 0; s < kStageCount; ++s) {
        if (names[s].empty()) continue;
        auto it = map.find(names[s]);
        if (it != map.end()) {
            if (stage) *stage = s;
            return it->second;
        }
    }
    if (stage) *stage = kMiss;
    return -1;
}

// ---- Dolphin ArbitraryMipmapDetector (TextureCacheBase.cpp), for dumps with detection on -------------
// levels: RGBA8 rows (top first) of level 0..n-1, level i = max(w>>i,1) x max(h>>i,1). The box chain from
// level 0 is compared with each stored level: RMS per channel / 2.56 (percent), averaged over the level
// pairs; arbitrary when > threshold (Dolphin default 14.0, detection itself default off).
inline bool HasArbitraryMipmaps(const std::vector<const uint8_t*>& levels, uint32_t w, uint32_t h,
                                float threshold = 14.0f) {
    if (levels.size() < 2) return false;
    std::vector<uint8_t> src(levels[0], levels[0] + size_t(w) * h * 4), dst;
    uint32_t sw = w, sh = h;
    float total = 0.0f;
    for (size_t i = 0; i + 1 < levels.size(); ++i) {
        const uint32_t dw = sw > 1 ? sw / 2 : 1, dh = sh > 1 ? sh / 2 : 1;
        dst.assign(size_t(dw) * dh * 4, 0);
        for (uint32_t y = 0; y < dh; ++y)
            for (uint32_t x = 0; x < dw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const uint32_t x0 = x * 2, y0 = y * 2;
                    const uint32_t x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
                    const uint32_t s = src[(size_t(y0) * sw + x0) * 4 + c] + src[(size_t(y0) * sw + x1) * 4 + c] +
                                       src[(size_t(y1) * sw + x0) * 4 + c] + src[(size_t(y1) * sw + x1) * 4 + c];
                    dst[(size_t(y) * dw + x) * 4 + c] = uint8_t((s + 2) / 4);
                }
        double sum = 0;
        const uint8_t* ref = levels[i + 1];
        for (size_t k = 0; k < dst.size(); ++k) {
            const int d = int(dst[k]) - int(ref[k]);
            sum += double(d) * d;
        }
        total += float(std::sqrt(sum / double(dst.size())) / 2.56);
        src.swap(dst);
        sw = dw; sh = dh;
    }
    return total / float(levels.size() - 1) > threshold;
}

} // namespace nwii::runtime::gx::hdtex
