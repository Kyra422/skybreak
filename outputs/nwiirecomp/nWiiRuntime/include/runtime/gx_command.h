#pragma once
#include <cstdint>
#include <vector>
#include <memory>

namespace nwii::runtime::gx {

enum class GXCommandType {
    BPRegister,
    CPRegister,
    XFRegister,
    DrawPrimitive
};

struct VertexData {
    float pos[3];
    float norm[3];
    float color[4];
    float tex[8][2];

    bool has_pos = false;
    bool has_norm = false;
    // Position/normal already transformed to view space at decode time (using
    // the per-draw matrix snapshot); the renderer must not transform again.
    bool pre_xf = false;
    bool has_color = false;
    bool has_tex[8] = {false};

    uint8_t posMtxIdx = 0;
    uint8_t texMtxIdx[8] = {0};

    // Untransformed (model-space) position and normal: the XF texgen inputs
    // for source rows GEOM and NORMAL (M1.8). pos/norm above may already be
    // transformed at decode (pre_xf).
    float raw_pos[3] = {0.0f, 0.0f, 0.0f};
    float raw_norm[3] = {0.0f, 0.0f, 0.0f};
};

// Deferred draw payload: raw vertex bytes plus the parse-time state needed to
// decode them later. Most draws are frame-skipped and never decoded, so the
// parser only snapshots; FifoParser::DecodeDraw() decodes survivors.
struct DrawRaw;

struct GXCommand {
    GXCommandType type;

    uint16_t reg;
    uint32_t val;

    uint16_t length;


    uint8_t prim_type;
    std::vector<VertexData> vertices;
    std::shared_ptr<DrawRaw> raw;

    std::vector<float> payload;
};

} 
