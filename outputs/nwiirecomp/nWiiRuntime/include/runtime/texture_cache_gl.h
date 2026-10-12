#pragma once

#include <glad/glad.h>
#include "runtime/cpu_context.h"
#include "runtime/gx_state.h"

#include <unordered_map>
#include <unordered_set> // HDTEX17
#include <cstdint>
#include <cstdlib>

struct Color {
    uint8_t r, g, b, a;
};

struct Image {
    void *data;
    int width;
    int height;
    int mipmaps;
    int format;
};

inline Image GenImageColor(int width, int height, Color color) {
    Image img;
    img.width = width;
    img.height = height;
    img.data = malloc(width * height * sizeof(Color));
    for (int i=0; i<width*height; i++) ((Color*)img.data)[i] = color;
    return img;
}
inline void UnloadImage(Image img) { free(img.data); }

#define BLANK Color{0,0,0,0}
#define MAGENTA Color{255,0,255,255}

namespace nwii::runtime::hle {

enum class TextureFormat {
    I4 = 0, I8 = 1, IA4 = 2, IA8 = 3, RGB565 = 4, RGB5A3 = 5,
    RGBA8 = 6, C4 = 8, C8 = 9, C14X2 = 10, CMPR = 14
};

struct TextureKey {
    uint32_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t tlut_offset;
    uint32_t tlut_format;
    uint32_t data_hash;
    uint32_t levels; // TEX7: mip levels uploaded from RAM (1 = base only)
    uint32_t mipf = 0; // HDTEX16: TEXMODE0 mip filter on (part of the Dolphin name: "_m")

    bool operator==(const TextureKey& other) const {
        return addr == other.addr && width == other.width &&
               height == other.height && format == other.format &&
               tlut_offset == other.tlut_offset &&
               tlut_format == other.tlut_format &&
               data_hash == other.data_hash && levels == other.levels &&
               mipf == other.mipf;
    }
};

struct TextureKeyHash {
    std::size_t operator()(const TextureKey& k) const {
        std::size_t h = k.addr;
        h = h * 31 + k.width;
        h = h * 31 + k.height;
        h = h * 31 + k.format;
        h = h * 31 + k.tlut_offset;
        h = h * 31 + k.tlut_format;
        h = h * 31 + k.data_hash;
        h = h * 31 + k.levels;
        h = h * 31 + k.mipf;
        return h;
    }
};

// HDTEX16: custom ("HD") texture replacement and texture dumping (Dolphin names) plug in here.
// src/hle/gx/hd_textures.cpp installs the hook at start (runtime only); without a hook (gxreplay, the
// unit tests that compile this file alone) the cache behaves exactly as before.
struct TexLoadInfo {
    const uint8_t* base = nullptr; // guest RAM of the base level (nullptr: not mapped)
    uint32_t width = 0, height = 0, format = 0;
    uint32_t tlut_offset = 0, tlut_format = 0;
    bool mip_filter = false;       // TEXMODE0 mip filter != none (Dolphin "_m")
    uint32_t levels = 1;           // mip levels the cache decodes from RAM
    uint32_t addr = 0;             // HDTEX17: guest address of the base level (NWII_HDTEX_LOG diagnostics)
};
struct TexDecodedLevel {
    const uint8_t* rgba; // w*h*4, top row first
    uint32_t w, h;
};
class TextureReplaceHook {
public:
    virtual ~TextureReplaceHook() = default;
    enum : unsigned { kReplaced = 1u, kWantDecoded = 2u, kShared = 4u };
    // A cache miss on the GL thread; `tex` is generated and bound to GL_TEXTURE_2D on the active unit.
    // kReplaced: the hook uploaded a complete custom image into `tex` (the cache then skips its own
    // upload). kWantDecoded: call on_decoded with the RAM levels (dumping).
    // HDTEX17 kShared: *shared = a HOOK-OWNED GL texture that already holds the complete custom image (one per pack
    // entry, shared by every cache entry that maps to it); the cache deletes `tex`, uses *shared instead and never
    // deletes it (clear() skips it; the hook deletes it once it stayed unused for a whole cache generation).
    virtual unsigned on_new_texture(const TexLoadInfo& info, GLuint tex, GLuint* shared) = 0;
    virtual void on_decoded(const TexLoadInfo& info, const TexDecodedLevel* levels, uint32_t n) = 0;
    // Start of every rendered frame (GL thread, before the first draw): deferred custom uploads.
    virtual void frame_begin() = 0;
};
void SetTextureReplaceHook(TextureReplaceHook* hook);
// Renderer flags in the `levels` value it hands to SetGXSamplerFromMode for a custom texture:
// bits 0-7 = GL levels of the bound texture, kTexLevelsCustom = custom texture (Dolphin max_lod 255),
// bits 16-23 = signed LOD bias offset in 1/32 LOD (custom "_arb" mipmaps: log2(EFB scale / custom scale)).
constexpr uint32_t kTexLevelsCustom = 0x100;
struct CustomTexInfo {
    uint32_t levels = 0;     // GL levels of the custom image (0 = not a custom texture)
    float scale = 1.0f;      // custom width / native width
    bool arb = false;        // pack name had "_arb" (arbitrary mipmaps)
    uint32_t hybrid_base = 0; // HDTEX17: log2(HD scale) + 1 when levels below the original size are the game's own
                              // mips (hd_textures.cpp ApplyOrigMips); 0 = all levels from the HD image
};

class TextureCache {
public:
    static TextureCache& get();

    // levels > 1 (TEX7): also decode and upload mip levels 1..levels-1, which
    // GX stores consecutively after the base level (each padded to whole tiles).
    // mip_filter (HDTEX16): TEXMODE0 mip filter on; only used for the Dolphin texture name.
    GLuint get_texture(CPUContext& ctx, const gx::TexStage& stage, uint32_t levels = 1,
                       bool mip_filter = false);

    // HDTEX16: custom texture bookkeeping (set by the replacement hook; cleared with the cache).
    void set_custom(GLuint tex, const CustomTexInfo& info) { custom_[tex] = info; }
    const CustomTexInfo* custom_info(GLuint tex) const {
        if (custom_.empty()) return nullptr;
        auto it = custom_.find(tex);
        return it == custom_.end() ? nullptr : &it->second;
    }
    uint64_t custom_count() const { return custom_.size(); }

    // EFB copy-to-texture: the renderer registers the GL texture holding an
    // EFB copy under the guest RAM address the game targeted; a texture stage
    // whose base_addr matches samples that copy directly (guest RAM was never
    // written, decoding it would show garbage).
    void register_efb_texture(uint32_t addr, GLuint tex);
    GLuint find_efb_texture(uint32_t addr) const;

    void clear();
    size_t size() const { return cache.size(); } // SOAK14 diagnostic ([STAT] texcache=)

private:
    TextureCache() = default;

    std::unordered_map<TextureKey, GLuint, TextureKeyHash> cache;
    std::unordered_map<uint32_t, GLuint> efb_textures;
    std::unordered_map<GLuint, CustomTexInfo> custom_; // HDTEX16
    std::unordered_set<GLuint> shared_;                // HDTEX17: hook-owned names in `cache` (never deleted here)

    Image decode_texture(CPUContext& ctx, const gx::TexStage& stage);

    void decode_i4(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_i8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_ia4(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_ia8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_rgb565(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_rgb5a3(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_rgba8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_cmpr(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out);
    void decode_paletted(CPUContext& ctx, const gx::TexStage& stage, Color* out);
};

uint32_t texture_data_size(uint32_t width, uint32_t height, uint32_t format);

} 
