#pragma once
#include "runtime/gx_state.h"
#include <cstdlib>
#include <string>

namespace nwii::runtime::gx {

struct GeneratedShader {
    std::string vertex_source;
    std::string fragment_source;
};

GeneratedShader GenerateTEVShader(const GXState& state, uint8_t prim_type);

// Indirect texturing (LIGHT7): true when the generated fragment shader uses the
// indirect / fixed-point coordinate path; the texmaps its indirect stages read
// (they must be bound like TEV-stage maps).
bool TEVShaderUsesIndirect(const GXState& state);
uint8_t TEVIndirectMapsUsed(const GXState& state);

// Texcoord scale (TEX7, BP 0x30-0x3F): bit n set = TEV stage n of a shader
// without indirect samples its map with texcoord scale != texmap size (the
// generated lookup then applies uTexDim[tc].zw / uTexDim[map].xy). 0 otherwise.
uint16_t TEVTexCoordScaleMismatch(const GXState& state);

// LIGHT17: Dolphin integer TEV (PixelShaderGen WriteTevRegular: 8-bit a/b/c,
// s11 d, c scaled to 0..256, rounding bias, scale inside the lerp) and integer
// channel lighting. false only with NWII_TEV_FLOAT=1 (old float path, A/B).
inline bool TEVIntegerMath() {
    static const bool float_tev = std::getenv("NWII_TEV_FLOAT") != nullptr;
    return !float_tev;
}

// LIGHT17 (OPT-IN, NWII_COLOR_SCREENLINEAR=1): rasterised colours interpolated
// linearly in screen space with CPU clipping to the viewport + near plane, as
// Dolphin's SOFTWARE renderer does (colour slopes without 1/w, Clipper.cpp).
// Default = perspective-correct colours, as Dolphin's hardware backends (real
// Flipper behaviour not verified; see LIGHT17.md for the 39-scene A/B).
inline bool TEVColorNoPerspective() {
    static const bool on = std::getenv("NWII_COLOR_SCREENLINEAR") != nullptr;
    return on;
}

// LIGHT17: EFB storage precision of the current draw (Dolphin SW EfbInterface:
// RGBA6_Z24 keeps 6 bits per channel after blending + 2x2 Bayer dither,
// RGB565_Z16 5/6/5; reads expand by bit replication). 0 = 8-bit store
// (RGB8_Z24 / Z24 / YUV formats, or NWII_EFB_TRUECOLOR=1). Otherwise bits 0-1
// format (1 RGBA6, 2 RGB565), bit 2 dither (RGBA6 + BP 0x41 bit 2), bit 3
// read-modify-write (blend or logic op: the renderer re-quantises the stored
// result, the shader must not). Part of the shader UID.
inline uint32_t TEVOutputQuantKey(const GXState& s) {
    static const bool truecolor = std::getenv("NWII_EFB_TRUECOLOR") != nullptr;
    if (truecolor) return 0;
    const uint32_t pf = s.bp[0x43] & 7;
    if (pf != 1 && pf != 2) return 0;
    const uint32_t cm = s.bp[0x41];
    uint32_t k = pf;
    if (pf == 1 && ((cm >> 2) & 1)) k |= 4;          // dither (RGBA6 only, as Dolphin SW)
    const bool blend = cm & 1;
    const bool logic = !blend && ((cm >> 1) & 1) && ((cm >> 12) & 0xF) != 3; // COPY = plain store
    if (blend || logic) k |= 8;
    return k;
}

} 
