#pragma once

#include "runtime/gx_state.h"
#include <glad/glad.h>
#include <algorithm>

namespace nwii::runtime::gx {

inline GLenum GXTextureWrap(uint8_t mode) {
  switch (mode) {
    case 1: return GL_REPEAT;
    case 2: return GL_MIRRORED_REPEAT;
    default: return GL_CLAMP_TO_EDGE;
  }
}

// The caller has bound the texture on this unit. Independent sampler objects
// allow a cached texture to use different wrapping on two simultaneous stages.
inline void BindGXTextureSampler(GLuint unit, GLuint sampler, const TexStage& stage) {
  glActiveTexture(GL_TEXTURE0 + unit);
  GLint min_filter = GL_NEAREST, mag_filter = GL_NEAREST;
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_filter);
  // Preserve the texture cache's filtering, including linear EFB copies.
  glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_filter);
  glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, mag_filter);
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GXTextureWrap(stage.wrapS));
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GXTextureWrap(stage.wrapT));
  glBindSampler(unit, sampler);
}

// RENDER1 (roadmap M1.10): sampler state straight from TEXMODE0 (BP
// 0x80-0x83 / 0xA0-0xA3): wrap S bits 0-1, wrap T bits 2-3, mag filter bit 4,
// min filter bits 5-7 (bit 7 = linear base filter; the mip part is ignored
// because no mip levels are uploaded). No per-draw glGetTexParameteriv.
inline void SetGXSamplerFromMode0(GLuint sampler, uint32_t mode0) {
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GXTextureWrap(mode0 & 3));
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GXTextureWrap((mode0 >> 2) & 3));
  glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER,
                      ((mode0 >> 4) & 1) ? GL_LINEAR : GL_NEAREST);
  glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                      ((mode0 >> 7) & 1) ? GL_LINEAR : GL_NEAREST);
}

// TEX7 (roadmap M1.10 remainder): full GX sampler state, Dolphin
// SamplerState::Generate. TEXMODE0: wrap S 0-1, wrap T 2-3, mag 4, mip filter
// 5-6 (0 none, 1 point, 2/3 linear), min 7, lod_bias 9-16 (s8, 1/32 LOD);
// TEXMODE1 (BP 0x84-0x87 / 0xA4-0xA7): min_lod 0-7, max_lod 8-15 (1/16 LOD).
// levels = mip levels the bound texture really has: with 1 (EFB copies, or a
// mip-less GX mode) the min filter stays non-mip and LOD is pinned to 0.
// HDTEX16: `levels` of a custom (HD) texture = its GL levels | 0x100 (kTexLevelsCustom) | signed
// 1/32-LOD offset in bits 16-23 ("_arb" packs); Dolphin GetSamplerState: max_lod = 255 for custom
// textures, the GX min_lod / lod_bias stay (in the custom texture's own LOD, as in Dolphin).
inline void SetGXSamplerFromMode(GLuint sampler, uint32_t mode0, uint32_t mode1, uint32_t levels) {
  const bool custom = (levels & 0x100u) != 0;                  // HDTEX16
  const float custom_bias = (float)(int8_t)((levels >> 16) & 0xFF) / 32.0f; // HDTEX16
  const uint32_t hybrid = (levels >> 24) & 0xFu;                // HDTEX17: log2(HD scale) + 1, 0 = none
  levels &= 0xFFu;                                             // HDTEX16
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GXTextureWrap(mode0 & 3));
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GXTextureWrap((mode0 >> 2) & 3));
  glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER,
                      ((mode0 >> 4) & 1) ? GL_LINEAR : GL_NEAREST);
  const bool lin = (mode0 >> 7) & 1;
  const uint32_t mip = (mode0 >> 5) & 3;
  GLint minf = lin ? GL_LINEAR : GL_NEAREST;
  float min_lod = 0.0f, max_lod = 0.0f, bias = 0.0f;
  if (mip != 0 && levels > 1) {
    if (mip == 1) minf = lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else          minf = lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    const uint32_t mx = (mode1 >> 8) & 0xFF, mn = std::min<uint32_t>(mode1 & 0xFF, mx);
    min_lod = mn / 16.0f;
    max_lod = custom ? 255.0f / 16.0f : mx / 16.0f; // HDTEX16
    // HDTEX17: a hybrid chain (HD levels down to the original size, then the game's own mips) keeps the GX max LOD
    // in original-texture units: custom level log2(scale) = original level 0.
    if (custom && hybrid) max_lod = mx / 16.0f + float(hybrid - 1);
    bias = (float)(int8_t)((mode0 >> 9) & 0xFF) / 32.0f + (custom ? custom_bias : 0.0f);
  }
  glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, minf);
  glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, min_lod);
  glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, max_lod);
  glSamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, bias);
}

} // namespace nwii::runtime::gx
