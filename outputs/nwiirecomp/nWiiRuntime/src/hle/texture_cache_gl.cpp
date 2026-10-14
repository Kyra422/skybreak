#include "runtime/texture_cache_gl.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector> // HDTEX16

// GX texture decoding, kept bit-exact with Dolphin's TextureDecoder:




namespace nwii::runtime::hle {

using gx::g_state;

// RENDER1: bumped whenever clear() deletes the GL textures, so the renderer's
// per-frame texture memo never hands out a dead (or recycled) GL name.
uint64_t g_texture_cache_generation = 0;
// Over-capacity clears are deferred to the next frame start (see
// TextureCacheFrameBegin): clearing mid-frame deleted textures that draws of
// the same frame still had bound.
static bool s_clear_pending = false;
static constexpr size_t kTextureCacheCap = 2048;
// HDTEX16: custom texture / dump hook (null unless hd_textures.cpp installed it).
static TextureReplaceHook* s_replace_hook = nullptr;
void SetTextureReplaceHook(TextureReplaceHook* hook) { s_replace_hook = hook; }

void TextureCacheFrameBegin() {
    if (s_clear_pending) {
        s_clear_pending = false;
        TextureCache::get().clear();
    }
    if (s_replace_hook) s_replace_hook->frame_begin(); // HDTEX16: deferred custom uploads
}

TextureCache& TextureCache::get() {
    static TextureCache instance;
    return instance;
}

void TextureCache::clear() {
    for (auto& pair : cache) {
        if (!shared_.empty() && shared_.count(pair.second)) continue; // HDTEX17: owned by the replace hook
        glDeleteTextures(1, &pair.second);
    }
    cache.clear();
    custom_.clear(); // HDTEX16
    shared_.clear(); // HDTEX17
    ++g_texture_cache_generation;
    // efb_textures deliberately survive: their GL objects are owned by the
    // renderer's copy path and re-filled every EFB copy.
}

// Keyed by the full physical address (M1.9): the old 0x01FFFFFF mask folded
// MEM2 copy destinations onto MEM1 addresses with the same low bits.
void TextureCache::register_efb_texture(uint32_t addr, GLuint tex) {
    efb_textures[addr & 0x1FFFFFFF] = tex;
}

GLuint TextureCache::find_efb_texture(uint32_t addr) const {
    auto it = efb_textures.find(addr & 0x1FFFFFFF);
    return it == efb_textures.end() ? 0 : it->second;
}

// 5/6/4/3-bit channel expansion, matching Dolphin's ConvertNTo8 tables.
static inline uint8_t c5to8(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t c6to8(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static inline uint8_t c4to8(uint32_t v) { return (uint8_t)((v << 4) | v); }
static inline uint8_t c3to8(uint32_t v) { return (uint8_t)((v << 5) | (v << 2) | (v >> 1)); }

uint32_t texture_data_size(uint32_t width, uint32_t height, uint32_t format) {
    uint32_t tw, th, bpp;
    switch ((TextureFormat)format) {
        case TextureFormat::I4:     tw = 8; th = 8; bpp = 4;  break;
        case TextureFormat::I8:     tw = 8; th = 4; bpp = 8;  break;
        case TextureFormat::IA4:    tw = 8; th = 4; bpp = 8;  break;
        case TextureFormat::IA8:    tw = 4; th = 4; bpp = 16; break;
        case TextureFormat::RGB565: tw = 4; th = 4; bpp = 16; break;
        case TextureFormat::RGB5A3: tw = 4; th = 4; bpp = 16; break;
        case TextureFormat::RGBA8:  tw = 4; th = 4; bpp = 32; break;
        case TextureFormat::C4:     tw = 8; th = 8; bpp = 4;  break;
        case TextureFormat::C8:     tw = 8; th = 4; bpp = 8;  break;
        case TextureFormat::C14X2:  tw = 4; th = 4; bpp = 16; break;
        case TextureFormat::CMPR:   tw = 8; th = 8; bpp = 4;  break;
        default:                    tw = 4; th = 4; bpp = 16; break;
    }
    uint32_t wt = (width + tw - 1) / tw;
    uint32_t ht = (height + th - 1) / th;
    return wt * ht * tw * th * bpp / 8;
}

GLuint TextureCache::get_texture(CPUContext& ctx, const gx::TexStage& stage, uint32_t levels,
                                 bool mip_filter) {
    // renderer_gl::EfbCopyFor validates GPU copies before calling this RAM
    // cache. Returning the registry entry here would resurrect a stale copy
    // rejected by an isolated interpolation frame.
    // TEX7: mip chain. Level l is max(w>>l,1) x max(h>>l,1), stored right after
    // level l-1, each level padded to whole tiles (Dolphin TextureCacheBase:
    // TexDecoder_GetTextureSizeInBytes per level). The hash covers all levels.
    if (levels < 1) levels = 1;
    if (levels > 11) levels = 11;
    uint32_t size = 0;
    for (uint32_t l = 0; l < levels; ++l)
        size += texture_data_size(std::max<uint32_t>(stage.width >> l, 1),
                                  std::max<uint32_t>(stage.height >> l, 1), stage.format);
    // TEX7: textures up to 4 KiB are hashed completely (Dolphin's sampled hash
    // also covers every block of small textures); a 32-byte stride hashed only
    // one word per CMPR/I4 tile, so CPU edits of the other 28 bytes (indices,
    // texels) kept the stale GL texture. Larger textures keep <= 4096 samples.
    uint32_t stride = size <= 4096 ? 4 : (size > 32 * 4096 ? size / 4096 : 32);
    uint32_t hash = 2166136261u;

    
    
    if (const uint8_t* p = ctx.mmu.get_ptr(stage.base_addr, size)) {
        for (uint32_t off = 0; off + 4 <= size; off += stride) {
            uint32_t w;
            std::memcpy(&w, p + off, 4);
            hash = (hash ^ w) * 16777619u;
        }
    }

    // hash exactly like the texel data (Dolphin XORs a TLUT hash into its

    
    
    uint32_t palette_entries = 0;
    if (stage.format == 8)        
        palette_entries = 16;
    else if (stage.format == 9)   
        palette_entries = 256;
    else if (stage.format == 10)  
        palette_entries = 16384;

    {
        uint32_t tl_bytes = palette_entries * 2;
        uint32_t tl_off = stage.tlut_offset;
        if (tl_off < sizeof(gx::g_state.tlutMem)) {
            uint32_t avail = (uint32_t)sizeof(gx::g_state.tlutMem) - tl_off;
            if (tl_bytes > avail) tl_bytes = avail;
            const uint8_t* tp = gx::g_state.tlutMem + tl_off;
            uint32_t i = 0;
            for (; i + 4 <= tl_bytes; i += 4) {
                uint32_t w;
                std::memcpy(&w, tp + i, 4);
                hash = (hash ^ w) * 16777619u;
            }
            for (; i < tl_bytes; ++i)
                hash = (hash ^ tp[i]) * 16777619u;
        }
    }

    // HDTEX16: the mip filter bit only matters for the Dolphin name (custom textures / dumps).
    const uint32_t mipf = (s_replace_hook && mip_filter) ? 1u : 0u;
    TextureKey key = {stage.base_addr, stage.width, stage.height,
                      stage.format, stage.tlut_offset, stage.tlut_format, hash, levels, mipf};
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    if (cache.size() > kTextureCacheCap)
        s_clear_pending = true; // cleared at the next frame start

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // HDTEX16: the hook may upload a ready custom image now (then the RAM levels are only decoded
    // when a dump wants them) or queue it for a later frame (the original is uploaded meanwhile).
    unsigned hook = 0;
    TexLoadInfo info;
    if (s_replace_hook) {
        info.base = ctx.mmu.get_ptr(stage.base_addr, texture_data_size(stage.width, stage.height, stage.format));
        info.width = stage.width;
        info.height = stage.height;
        info.format = stage.format;
        info.tlut_offset = stage.tlut_offset;
        info.tlut_format = stage.tlut_format;
        info.mip_filter = mipf != 0;
        info.levels = levels;
        info.addr = stage.base_addr; // HDTEX17: diagnostics only
        GLuint shared = 0;
        hook = s_replace_hook->on_new_texture(info, tex, &shared);
        if ((hook & TextureReplaceHook::kShared) && shared && shared != tex) { // HDTEX17: hook-owned custom texture
            glDeleteTextures(1, &tex);
            tex = shared;
            shared_.insert(tex);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    const bool upload = !(hook & (TextureReplaceHook::kReplaced | TextureReplaceHook::kShared)); // HDTEX17: + kShared
    const bool keep = (hook & TextureReplaceHook::kWantDecoded) != 0;
    if (upload || keep) {
        std::vector<Image> kept;
        gx::TexStage lvl = stage;
        for (uint32_t l = 0; l < levels; ++l) {
            lvl.width = std::max<uint32_t>(stage.width >> l, 1);
            lvl.height = std::max<uint32_t>(stage.height >> l, 1);
            Image img = decode_texture(ctx, lvl);
            if (upload)
                glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA, lvl.width, lvl.height, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, img.data);
            if (keep) kept.push_back(img);
            else UnloadImage(img);
            lvl.base_addr += texture_data_size(lvl.width, lvl.height, stage.format);
        }
        if (keep) {
            std::vector<TexDecodedLevel> lv;
            for (const Image& im : kept)
                lv.push_back({(const uint8_t*)im.data, (uint32_t)im.width, (uint32_t)im.height});
            s_replace_hook->on_decoded(info, lv.data(), (uint32_t)lv.size());
            for (Image& im : kept) UnloadImage(im);
            glBindTexture(GL_TEXTURE_2D, tex);
        }
    }
    if (upload) {
        // Complete with exactly the uploaded levels (the sampler picks the mip
        // filter only when levels > 1; a mip filter on an incomplete texture
        // samples black).
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)(levels - 1));
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    cache[key] = tex;
    return tex;
}

Image TextureCache::decode_texture(CPUContext& ctx, const gx::TexStage& stage) {
    uint32_t width = stage.width, height = stage.height;
    Image img = GenImageColor(width, height, BLANK);
    Color* pixels = (Color*)img.data;
    uint32_t addr = stage.base_addr;

    switch ((TextureFormat)stage.format) {
        case TextureFormat::I4:     decode_i4(ctx, addr, width, height, pixels); break;
        case TextureFormat::I8:     decode_i8(ctx, addr, width, height, pixels); break;
        case TextureFormat::IA4:    decode_ia4(ctx, addr, width, height, pixels); break;
        case TextureFormat::IA8:    decode_ia8(ctx, addr, width, height, pixels); break;
        case TextureFormat::RGB565: decode_rgb565(ctx, addr, width, height, pixels); break;
        case TextureFormat::RGB5A3: decode_rgb5a3(ctx, addr, width, height, pixels); break;
        case TextureFormat::RGBA8:  decode_rgba8(ctx, addr, width, height, pixels); break;
        case TextureFormat::C4:
        case TextureFormat::C8:
        case TextureFormat::C14X2:  decode_paletted(ctx, stage, pixels); break;
        case TextureFormat::CMPR:   decode_cmpr(ctx, addr, width, height, pixels); break;
        default:
            std::cout << "[TextureCache] Unknown texture format: "
                      << (int)stage.format << std::endl;
            for (uint32_t i = 0; i < width * height; i++) pixels[i] = MAGENTA;
            break;
    }

    return img;
}

static uint32_t get_tile_address(uint32_t x, uint32_t y, uint32_t bw, uint32_t bh, uint32_t bpp, uint32_t width) {
    uint32_t tiles_x = (width + bw - 1) / bw;
    uint32_t tx = x / bw;
    uint32_t ty = y / bh;
    uint32_t in_tile_x = x % bw;
    uint32_t in_tile_y = y % bh;

    uint32_t tile_idx = ty * tiles_x + tx;
    uint32_t tile_offset = tile_idx * bw * bh * bpp / 8;
    uint32_t in_tile_offset = (in_tile_y * bw + in_tile_x) * bpp / 8;

    return tile_offset + in_tile_offset;
}

void TextureCache::decode_i4(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x+=2) {
            uint32_t offset = get_tile_address(x, y, 8, 8, 4, width);
            uint8_t b = ctx.mmu.read8(addr + offset);
            uint8_t i1 = c4to8(b >> 4);
            uint8_t i2 = c4to8(b & 0x0F);
            out[y * width + x] = {i1, i1, i1, i1};
            if (x+1 < width) out[y * width + x + 1] = {i2, i2, i2, i2};
        }
    }
}

void TextureCache::decode_i8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t offset = get_tile_address(x, y, 8, 4, 8, width);
            uint8_t i = ctx.mmu.read8(addr + offset);
            out[y * width + x] = {i, i, i, i};
        }
    }
}

void TextureCache::decode_ia4(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t offset = get_tile_address(x, y, 8, 4, 8, width);
            uint8_t b = ctx.mmu.read8(addr + offset);
            uint8_t a = c4to8(b >> 4);
            uint8_t i = c4to8(b & 0x0F);
            out[y * width + x] = {i, i, i, a};
        }
    }
}

void TextureCache::decode_ia8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t offset = get_tile_address(x, y, 4, 4, 16, width);
            uint8_t a = ctx.mmu.read8(addr + offset);
            uint8_t i = ctx.mmu.read8(addr + offset + 1);
            out[y * width + x] = {i, i, i, a};
        }
    }
}

static inline Color unpack_rgb565(uint16_t c) {
    return { c5to8((c >> 11) & 0x1F), c6to8((c >> 5) & 0x3F), c5to8(c & 0x1F), 255 };
}

static inline Color unpack_rgb5a3(uint16_t c) {
    if (c & 0x8000) { 
        return { c5to8((c >> 10) & 0x1F), c5to8((c >> 5) & 0x1F), c5to8(c & 0x1F), 255 };
    }
    return { c4to8((c >> 8) & 0xF), c4to8((c >> 4) & 0xF), c4to8(c & 0xF),
             c3to8((c >> 12) & 0x7) };
}

void TextureCache::decode_rgb565(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t offset = get_tile_address(x, y, 4, 4, 16, width);
            out[y * width + x] = unpack_rgb565(ctx.mmu.read16(addr + offset));
        }
    }
}

void TextureCache::decode_rgb5a3(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t offset = get_tile_address(x, y, 4, 4, 16, width);
            out[y * width + x] = unpack_rgb5a3(ctx.mmu.read16(addr + offset));
        }
    }
}

void TextureCache::decode_rgba8(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {
    
    uint32_t tiles_x = (width + 3) / 4;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t tx = x / 4;
            uint32_t ty = y / 4;
            uint32_t tile_idx = ty * tiles_x + tx;
            uint32_t tile_offset = tile_idx * 64; 

            uint32_t in_tile_x = x % 4;
            uint32_t in_tile_y = y % 4;
            uint32_t in_tile_offset = (in_tile_y * 4 + in_tile_x) * 2;

            uint16_t ar = ctx.mmu.read16(addr + tile_offset + in_tile_offset);
            uint16_t gb = ctx.mmu.read16(addr + tile_offset + 32 + in_tile_offset);

            out[y * width + x] = {
                (uint8_t)(ar & 0xFF),
                (uint8_t)(gb >> 8),
                (uint8_t)(gb & 0xFF),
                (uint8_t)(ar >> 8)
            };
        }
    }
}

void TextureCache::decode_paletted(CPUContext& ctx, const gx::TexStage& stage, Color* out) {

    uint32_t width = stage.width, height = stage.height, addr = stage.base_addr;
    auto lookup = [&](uint32_t index) -> Color {
        uint32_t off = stage.tlut_offset + index * 2;
        if (off + 1 >= sizeof(g_state.tlutMem)) return MAGENTA;
        uint16_t entry = (g_state.tlutMem[off] << 8) | g_state.tlutMem[off + 1];
        switch (stage.tlut_format) {
            case 0: { 
                uint8_t a = entry >> 8, i = entry & 0xFF;
                return {i, i, i, a};
            }
            case 1: return unpack_rgb565(entry);
            default: return unpack_rgb5a3(entry);
        }
    };

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t index;
            switch ((TextureFormat)stage.format) {
                case TextureFormat::C4: {
                    uint32_t offset = get_tile_address(x & ~1u, y, 8, 8, 4, width);
                    uint8_t b = ctx.mmu.read8(addr + offset);
                    index = (x & 1) ? (b & 0x0F) : (b >> 4);
                    break;
                }
                case TextureFormat::C8: {
                    uint32_t offset = get_tile_address(x, y, 8, 4, 8, width);
                    index = ctx.mmu.read8(addr + offset);
                    break;
                }
                default: { 
                    uint32_t offset = get_tile_address(x, y, 4, 4, 16, width);
                    index = ctx.mmu.read16(addr + offset) & 0x3FFF;
                    break;
                }
            }
            out[y * width + x] = lookup(index);
        }
    }
}

void TextureCache::decode_cmpr(CPUContext& ctx, uint32_t addr, uint32_t width, uint32_t height, Color* out) {

    
    uint32_t tiles_x = (width + 7) / 8;
    for (uint32_t y = 0; y < height; y+=8) {
        for (uint32_t x = 0; x < width; x+=8) {
            uint32_t tx = x / 8;
            uint32_t ty = y / 8;
            uint32_t tile_offset = (ty * tiles_x + tx) * 32;

            for (int by = 0; by < 2; by++) {
                for (int bx = 0; bx < 2; bx++) {
                    uint32_t block_offset = tile_offset + (by * 2 + bx) * 8;
                    uint16_t c0 = ctx.mmu.read16(addr + block_offset);
                    uint16_t c1 = ctx.mmu.read16(addr + block_offset + 2);
                    uint32_t indices = ctx.mmu.read32(addr + block_offset + 4);

                    Color pal[4];
                    pal[0] = unpack_rgb565(c0);
                    pal[1] = unpack_rgb565(c1);
                    if (c0 > c1) {
                        // TEX7: GX blends 5/8 + 3/8, not DXT1's 2/3 + 1/3 (Dolphin
                        // TextureDecoder_Common DecodeDXTBlock / DXTBlend).
                        auto bl = [](uint32_t v1, uint32_t v2) { return (uint8_t)((v1 * 3 + v2 * 5) >> 3); };
                        pal[2] = { bl(pal[1].r, pal[0].r), bl(pal[1].g, pal[0].g), bl(pal[1].b, pal[0].b), 255 };
                        pal[3] = { bl(pal[0].r, pal[1].r), bl(pal[0].g, pal[1].g), bl(pal[0].b, pal[1].b), 255 };
                    } else {

                        pal[2] = { (uint8_t)((pal[0].r + pal[1].r)/2), (uint8_t)((pal[0].g + pal[1].g)/2), (uint8_t)((pal[0].b + pal[1].b)/2), 255 };
                        pal[3] = { pal[2].r, pal[2].g, pal[2].b, 0 };
                    }

                    for (int py = 0; py < 4; py++) {
                        for (int px = 0; px < 4; px++) {
                            int shift = 30 - 2 * (py * 4 + px); 
                            int idx = (indices >> shift) & 3;
                            uint32_t out_x = x + bx * 4 + px;
                            uint32_t out_y = y + by * 4 + py;
                            if (out_x < width && out_y < height) {
                                out[out_y * width + out_x] = pal[idx];
                            }
                        }
                    }
                }
            }
        }
    }
}

} 
