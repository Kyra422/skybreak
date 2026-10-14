// UI13: GX texture / U8 / TPL / THP / BRFNT readers (see ui_assets.h).
#include "ui_assets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace nwii::ui {

namespace {
inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void set_err(std::string* err, const std::string& m) {
    if (err) *err = m;
}
inline uint8_t x5(int v) { return uint8_t((v << 3) | (v >> 2)); }
inline uint8_t x6(int v) { return uint8_t((v << 2) | (v >> 4)); }
inline uint8_t x4(int v) { return uint8_t(v * 17); }
inline uint8_t x3(int v) { return uint8_t((v << 5) | (v << 2) | (v >> 1)); } // Dolphin Convert3To8

struct Tile {
    int w, h, bytes;
};
bool tile_of(int fmt, Tile& t) {
    switch (fmt) {
    case kGxI4: t = {8, 8, 32}; return true;
    case kGxI8: t = {8, 4, 32}; return true;
    case kGxIA4: t = {8, 4, 32}; return true;
    case kGxIA8: t = {4, 4, 32}; return true;
    case kGxRGB565: t = {4, 4, 32}; return true;
    case kGxRGB5A3: t = {4, 4, 32}; return true;
    case kGxRGBA8: t = {4, 4, 64}; return true;
    case kGxCMPR: t = {8, 8, 32}; return true;
    default: return false;
    }
}
void put(Image& im, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x >= im.w || y >= im.h) return;
    uint8_t* p = &im.rgba[(size_t(y) * im.w + x) * 4];
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
void rgb565(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = x5((v >> 11) & 31); g = x6((v >> 5) & 63); b = x5(v & 31);
}
} // namespace

size_t GxTextureSize(int w, int h, int fmt) {
    Tile t;
    if (w <= 0 || h <= 0 || !tile_of(fmt, t)) return 0;
    const size_t tx = size_t((w + t.w - 1) / t.w), ty = size_t((h + t.h - 1) / t.h);
    return tx * ty * size_t(t.bytes);
}

bool DecodeGxTexture(const uint8_t* data, size_t size, int w, int h, int fmt, Image& out, std::string* err) {
    Tile t;
    if (!tile_of(fmt, t)) { set_err(err, "unsupported GX texture format " + std::to_string(fmt)); return false; }
    const size_t need = GxTextureSize(w, h, fmt);
    if (!data || size < need) { set_err(err, "texture data too short"); return false; }
    out.w = w; out.h = h;
    out.rgba.assign(size_t(w) * h * 4, 0);
    const uint8_t* p = data;
    for (int ty = 0; ty < h; ty += t.h) {
        for (int tx = 0; tx < w; tx += t.w) {
            switch (fmt) {
            case kGxI4:
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; x += 2, ++p) {
                        const uint8_t a = x4(*p >> 4), b = x4(*p & 15);
                        put(out, tx + x, ty + y, a, a, a, a);
                        put(out, tx + x + 1, ty + y, b, b, b, b);
                    }
                break;
            case kGxI8:
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 8; ++x, ++p) put(out, tx + x, ty + y, *p, *p, *p, *p);
                break;
            case kGxIA4:
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 8; ++x, ++p) {
                        const uint8_t i = x4(*p & 15), a = x4(*p >> 4);
                        put(out, tx + x, ty + y, i, i, i, a);
                    }
                break;
            case kGxIA8:
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x, p += 2) put(out, tx + x, ty + y, p[1], p[1], p[1], p[0]);
                break;
            case kGxRGB565:
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x, p += 2) {
                        uint8_t r, g, b;
                        rgb565(be16(p), r, g, b);
                        put(out, tx + x, ty + y, r, g, b, 255);
                    }
                break;
            case kGxRGB5A3:
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x, p += 2) {
                        const uint16_t v = be16(p);
                        if (v & 0x8000)
                            put(out, tx + x, ty + y, x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), 255);
                        else
                            put(out, tx + x, ty + y, x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15),
                                x3((v >> 12) & 7));
                    }
                break;
            case kGxRGBA8:
                for (int i = 0; i < 16; ++i) {
                    const uint8_t* ar = p + i * 2;
                    const uint8_t* gb = p + 32 + i * 2;
                    put(out, tx + (i & 3), ty + (i >> 2), ar[1], gb[0], gb[1], ar[0]);
                }
                p += 64;
                break;
            case kGxCMPR:
                for (int sub = 0; sub < 4; ++sub, p += 8) {
                    const int sx = tx + (sub & 1) * 4, sy = ty + (sub >> 1) * 4;
                    const uint16_t c0 = be16(p), c1 = be16(p + 2);
                    uint8_t pal[4][4];
                    rgb565(c0, pal[0][0], pal[0][1], pal[0][2]);
                    rgb565(c1, pal[1][0], pal[1][1], pal[1][2]);
                    pal[0][3] = pal[1][3] = 255;
                    for (int c = 0; c < 3; ++c) {
                        if (c0 > c1) {
                            pal[2][c] = uint8_t((2 * pal[0][c] + pal[1][c]) / 3);
                            pal[3][c] = uint8_t((pal[0][c] + 2 * pal[1][c]) / 3);
                        } else {
                            pal[2][c] = uint8_t((pal[0][c] + pal[1][c]) / 2);
                            pal[3][c] = pal[2][c];
                        }
                    }
                    pal[2][3] = 255;
                    pal[3][3] = c0 > c1 ? 255 : 0;
                    for (int y = 0; y < 4; ++y) {
                        const uint8_t row = p[4 + y];
                        for (int x = 0; x < 4; ++x) {
                            const uint8_t* c = pal[(row >> (6 - 2 * x)) & 3];
                            put(out, sx + x, sy + y, c[0], c[1], c[2], c[3]);
                        }
                    }
                }
                break;
            }
        }
    }
    return true;
}

bool ParseU8(const std::vector<uint8_t>& arc, std::vector<U8Entry>& out, std::string* err) {
    out.clear();
    if (arc.size() < 32 || be32(arc.data()) != 0x55AA382Du) { set_err(err, "not a U8 archive"); return false; }
    const uint32_t root = be32(&arc[4]);
    if (root + 12 > arc.size()) { set_err(err, "U8 root out of range"); return false; }
    const uint32_t count = be32(&arc[root + 8]);
    if (count == 0 || root + size_t(count) * 12 > arc.size()) { set_err(err, "U8 node table out of range"); return false; }
    const size_t strings = root + size_t(count) * 12;
    struct Dir { std::string path; uint32_t end; };
    std::vector<Dir> stack{{"", count}};
    for (uint32_t i = 1; i < count; ++i) {
        while (stack.size() > 1 && i >= stack.back().end) stack.pop_back();
        const uint8_t* n = &arc[root + size_t(i) * 12];
        const uint32_t name_off = (uint32_t(n[1]) << 16) | (uint32_t(n[2]) << 8) | n[3];
        std::string name;
        for (size_t k = strings + name_off; k < arc.size() && arc[k]; ++k) name.push_back(char(arc[k]));
        const std::string path = stack.back().path.empty() ? name : stack.back().path + "/" + name;
        if (n[0] == 1) {
            stack.push_back({(name == "." && stack.back().path.empty()) ? std::string() : path, be32(n + 8)});
        } else {
            U8Entry e;
            e.path = path;
            e.offset = be32(n + 4);
            e.size = be32(n + 8);
            if (size_t(e.offset) + e.size > arc.size()) { set_err(err, "U8 file out of range: " + path); return false; }
            out.push_back(std::move(e));
        }
    }
    return true;
}

const U8Entry* FindU8(const std::vector<U8Entry>& entries, const std::string& name) {
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    };
    const std::string want = lower(name);
    for (const U8Entry& e : entries) {
        const std::string p = lower(e.path);
        if (p.size() >= want.size() && p.compare(p.size() - want.size(), want.size(), want) == 0 &&
            (p.size() == want.size() || p[p.size() - want.size() - 1] == '/'))
            return &e;
    }
    return nullptr;
}

bool DecodeTpl(const uint8_t* data, size_t size, Image& out, std::string* err) {
    if (!data || size < 12 || be32(data) != 0x0020AF30u) { set_err(err, "not a TPL file"); return false; }
    const uint32_t n = be32(data + 4), table = be32(data + 8);
    if (n == 0 || size_t(table) + 8 > size) { set_err(err, "TPL has no image"); return false; }
    const uint32_t hdr = be32(data + table);
    if (size_t(hdr) + 12 > size) { set_err(err, "TPL image header out of range"); return false; }
    const int h = be16(data + hdr), w = be16(data + hdr + 2);
    const int fmt = int(be32(data + hdr + 4));
    const uint32_t off = be32(data + hdr + 8);
    if (off >= size) { set_err(err, "TPL image data out of range"); return false; }
    return DecodeGxTexture(data + off, size - off, w, h, fmt, out, err);
}

bool ReadFile(const std::string& path_utf8, std::vector<uint8_t>& out) {
    std::ifstream f(std::filesystem::path(std::u8string(path_utf8.begin(), path_utf8.end())), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0);
    out.resize(size_t(n));
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    return bool(f);
}

bool RestoreJpegStuffing(const std::vector<uint8_t>& d, std::vector<uint8_t>& jpeg, std::string* err) {
    jpeg.clear();
    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8) { set_err(err, "THP frame is not a JPEG"); return false; }
    size_t p = 2;
    while (p + 4 <= d.size()) {
        if (d[p] != 0xFF) { set_err(err, "invalid JPEG segment marker"); return false; }
        const uint8_t marker = d[p + 1];
        const size_t len = be16(&d[p + 2]);
        if (len < 2 || p + 2 + len > d.size()) { set_err(err, "invalid JPEG segment length"); return false; }
        if (marker == 0xDA) {
            const size_t start = p + 2 + len;
            size_t end = d.size() - 2;
            while (end > start && !(d[end] == 0xFF && d[end + 1] == 0xD9)) --end;
            if (end <= start) { set_err(err, "missing JPEG end marker"); return false; }
            jpeg.assign(d.begin(), d.begin() + std::ptrdiff_t(start));
            for (size_t k = start; k < end; ++k) {
                jpeg.push_back(d[k]);
                if (d[k] == 0xFF) jpeg.push_back(0x00);
            }
            jpeg.insert(jpeg.end(), d.begin() + std::ptrdiff_t(end), d.end());
            return true;
        }
        p += 2 + len;
    }
    set_err(err, "missing JPEG scan");
    return false;
}

bool ThpFrameJpeg(const std::string& path, int frame, std::vector<uint8_t>& jpeg, ThpInfo* info, std::string* err) {
    std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!f) { set_err(err, "cannot open " + path); return false; }
    uint8_t h[48];
    if (!f.read(reinterpret_cast<char*>(h), 48) || std::memcmp(h, "THP\0", 4) != 0) {
        set_err(err, "not a THP movie"); return false;
    }
    const uint32_t bufsize = be32(h + 8), frames = be32(h + 20), first_size = be32(h + 24);
    const uint32_t comp_off = be32(h + 32), first = be32(h + 40);
    float fps;
    { uint32_t v = be32(h + 16); std::memcpy(&fps, &v, 4); }
    uint8_t c[64];
    f.seekg(comp_off);
    if (!f.read(reinterpret_cast<char*>(c), 28)) { set_err(err, "THP component table missing"); return false; }
    const uint32_t ncomp = be32(c);
    if (ncomp == 0 || ncomp > 16 || c[4] != 0) { set_err(err, "THP has no leading video component"); return false; }
    // Component info follows the 16 type bytes: the video info (width, height) comes first.
    const int width = int(be32(c + 20)), height = int(be32(c + 24));
    if (info) { info->frames = int(frames); info->width = width; info->height = height; info->fps = fps; }
    if (frame < 0 || uint32_t(frame) >= frames) { set_err(err, "THP frame out of range"); return false; }
    uint64_t pos = first;
    uint32_t size = first_size;
    for (int i = 0;; ++i) {
        if (size < 8 + 4 * ncomp || (bufsize && size > bufsize)) { set_err(err, "invalid THP frame size"); return false; }
        f.seekg(std::streamoff(pos));
        uint8_t fh[8 + 4 * 16];
        if (!f.read(reinterpret_cast<char*>(fh), std::streamsize(8 + 4 * ncomp))) { set_err(err, "truncated THP frame"); return false; }
        if (i == frame) {
            const uint32_t vsize = be32(fh + 8);
            if (vsize == 0 || vsize > size) { set_err(err, "invalid THP video size"); return false; }
            std::vector<uint8_t> raw(vsize);
            if (!f.read(reinterpret_cast<char*>(raw.data()), vsize)) { set_err(err, "truncated THP video data"); return false; }
            return RestoreJpegStuffing(raw, jpeg, err);
        }
        pos += size;
        size = be32(fh);
    }
}

const BrfntGlyph* BrfntFont::Find(uint32_t code) const {
    auto it = std::lower_bound(glyphs.begin(), glyphs.end(), code,
                               [](const BrfntGlyph& g, uint32_t c) { return g.code < c; });
    return it != glyphs.end() && it->code == code ? &*it : nullptr;
}

bool ParseBrfnt(const std::vector<uint8_t>& d, BrfntFont& font, std::string* err) {
    font = BrfntFont{};
    if (d.size() < 16 || std::memcmp(d.data(), "RFNT", 4) != 0 || be16(&d[4]) != 0xFEFF) {
        set_err(err, "not an RFNT font"); return false;
    }
    size_t off = be16(&d[12]);
    uint32_t ptr_glyph = 0, ptr_width = 0, ptr_map = 0;
    bool have_finf = false;
    while (off + 8 <= d.size()) {
        const uint32_t sz = be32(&d[off + 4]);
        if (sz < 8 || off + sz > d.size()) break;
        if (std::memcmp(&d[off], "FINF", 4) == 0 && sz >= 8 + 23) {
            const uint8_t* p = &d[off + 8];
            font.font_type = p[0];
            font.leading = int8_t(p[1]);
            font.default_index = be16(p + 2);
            font.encoding = p[7];
            ptr_glyph = be32(p + 8); ptr_width = be32(p + 12); ptr_map = be32(p + 16);
            font.ascent = p[22];
            have_finf = true;
        }
        off += sz;
    }
    if (!have_finf || ptr_glyph + 24 > d.size()) { set_err(err, "RFNT without FINF/TGLP"); return false; }
    {
        const uint8_t* t = &d[ptr_glyph];
        font.cell_w = t[0]; font.cell_h = t[1]; font.baseline = int8_t(t[2]); font.max_char_w = t[3];
        const uint32_t sheet_size = be32(t + 4);
        font.sheet_count = be16(t + 8);
        font.sheet_fmt = be16(t + 10) & 0x7FFF;
        font.sheet_row = be16(t + 12); font.sheet_line = be16(t + 14);
        font.sheet_w = be16(t + 16); font.sheet_h = be16(t + 18);
        const uint32_t image = be32(t + 20);
        if ((be16(t + 10) & 0x8000) != 0) { set_err(err, "compressed font sheets are not supported"); return false; }
        if (font.sheet_row <= 0 || font.sheet_line <= 0 || font.cell_w <= 0 || font.cell_h <= 0 ||
            size_t(image) + size_t(sheet_size) * font.sheet_count > d.size() ||
            GxTextureSize(font.sheet_w, font.sheet_h, font.sheet_fmt) > sheet_size) {
            set_err(err, "font sheets out of range"); return false;
        }
        for (int s = 0; s < font.sheet_count; ++s) {
            Image im;
            if (!DecodeGxTexture(&d[image + size_t(sheet_size) * s], sheet_size, font.sheet_w, font.sheet_h,
                                 font.sheet_fmt, im, err))
                return false;
            font.sheets.push_back(std::move(im.rgba));
        }
    }
    // Widths: chained CWDH blocks.
    struct W { int left, gw, cw; };
    std::vector<W> widths;
    for (uint32_t p = ptr_width, guard = 0; p && p + 8 <= d.size() && guard < 64; ++guard) {
        const int begin = be16(&d[p]), end = be16(&d[p + 2]);
        const uint32_t next = be32(&d[p + 4]);
        if (end < begin || p + 8 + size_t(end - begin + 1) * 3 > d.size()) break;
        if (int(widths.size()) <= end) widths.resize(size_t(end) + 1, {0, font.cell_w, font.cell_w});
        for (int i = begin; i <= end; ++i) {
            const uint8_t* e = &d[p + 8 + size_t(i - begin) * 3];
            widths[size_t(i)] = {int8_t(e[0]), e[1], int8_t(e[2])};
        }
        p = next;
    }
    // Code map: chained CMAP blocks (direct / table / scan).
    std::vector<std::pair<uint32_t, int>> map;
    for (uint32_t p = ptr_map, guard = 0; p && p + 12 <= d.size() && guard < 256; ++guard) {
        const uint32_t begin = be16(&d[p]), end = be16(&d[p + 2]);
        const int method = be16(&d[p + 4]);
        const uint32_t next = be32(&d[p + 8]);
        const size_t data = p + 12;
        if (method == 0 && data + 2 <= d.size()) {
            const int base = be16(&d[data]);
            for (uint32_t c = begin; c <= end; ++c) map.emplace_back(c, base + int(c - begin));
        } else if (method == 1 && data + size_t(end - begin + 1) * 2 <= d.size()) {
            for (uint32_t c = begin; c <= end; ++c) {
                const int idx = be16(&d[data + size_t(c - begin) * 2]);
                if (idx != 0xFFFF) map.emplace_back(c, idx);
            }
        } else if (method == 2 && data + 2 <= d.size()) {
            const int n = be16(&d[data]);
            for (int k = 0; k < n && data + 2 + size_t(k) * 4 + 4 <= d.size(); ++k) {
                const int idx = be16(&d[data + 2 + size_t(k) * 4 + 2]);
                if (idx != 0xFFFF) map.emplace_back(be16(&d[data + 2 + size_t(k) * 4]), idx);
            }
        }
        p = next;
    }
    const int cells = font.sheet_row * font.sheet_line * font.sheet_count;
    for (auto& [code, idx] : map) {
        if (idx < 0 || idx >= cells) continue;
        BrfntGlyph g;
        g.code = code;
        g.index = idx;
        const W w = size_t(idx) < widths.size() ? widths[size_t(idx)] : W{0, font.cell_w, font.cell_w};
        g.left = w.left; g.glyph_width = std::min(w.gw, font.cell_w); g.char_width = w.cw;
        font.glyphs.push_back(g);
    }
    std::sort(font.glyphs.begin(), font.glyphs.end(), [](const BrfntGlyph& a, const BrfntGlyph& b) {
        return a.code < b.code || (a.code == b.code && a.index < b.index);
    });
    font.glyphs.erase(std::unique(font.glyphs.begin(), font.glyphs.end(),
                                  [](const BrfntGlyph& a, const BrfntGlyph& b) { return a.code == b.code; }),
                      font.glyphs.end());
    if (font.glyphs.empty()) { set_err(err, "font has no mapped glyphs"); return false; }
    return true;
}

bool BrfntGlyphImage(const BrfntFont& font, const BrfntGlyph& g, Image& out) {
    const int per_sheet = font.sheet_row * font.sheet_line;
    if (per_sheet <= 0 || g.index < 0) return false;
    const int sheet = g.index / per_sheet, cell = g.index % per_sheet;
    if (sheet >= int(font.sheets.size())) return false;
    // NW4R ResFontBase: cells are (cell + 1) pixels apart.
    const int cx = (cell % font.sheet_row) * (font.cell_w + 1);
    const int cy = (cell / font.sheet_row) * (font.cell_h + 1);
    const int w = std::max(1, g.glyph_width), h = font.cell_h;
    out.w = w; out.h = h;
    out.rgba.assign(size_t(w) * h * 4, 0);
    const bool intensity = font.sheet_fmt == kGxI4 || font.sheet_fmt == kGxI8;
    const std::vector<uint8_t>& s = font.sheets[size_t(sheet)];
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = cx + x, sy = cy + y;
            if (sx >= font.sheet_w || sy >= font.sheet_h) continue;
            const uint8_t* p = &s[(size_t(sy) * font.sheet_w + sx) * 4];
            uint8_t* q = &out.rgba[(size_t(y) * w + x) * 4];
            if (intensity) {
                const int v = p[0];
                const uint8_t c = uint8_t(std::clamp(2 * v - 255, 0, 255));
                q[0] = q[1] = q[2] = c;
                q[3] = uint8_t(std::min(255, 2 * v));
            } else {
                q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3];
            }
        }
    return true;
}

Image Resample(const Image& src, int w, int h) {
    Image out;
    out.w = std::max(1, w); out.h = std::max(1, h);
    out.rgba.assign(size_t(out.w) * out.h * 4, 0);
    if (src.w <= 0 || src.h <= 0) return out;
    const float sx = float(src.w) / out.w, sy = float(src.h) / out.h;
    auto fetch = [&](int x, int y, float* acc, float wgt) {
        x = std::clamp(x, 0, src.w - 1); y = std::clamp(y, 0, src.h - 1);
        const uint8_t* p = &src.rgba[(size_t(y) * src.w + x) * 4];
        const float a = p[3] / 255.0f * wgt;
        acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += a;
    };
    constexpr int kSub = 4;
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < kSub; ++j)
                for (int i = 0; i < kSub; ++i) {
                    // bilinear sample at the sub-pixel centre
                    const float fx = (x + (i + 0.5f) / kSub) * sx - 0.5f, fy = (y + (j + 0.5f) / kSub) * sy - 0.5f;
                    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
                    const float ax = fx - x0, ay = fy - y0;
                    fetch(x0, y0, acc, (1 - ax) * (1 - ay));
                    fetch(x0 + 1, y0, acc, ax * (1 - ay));
                    fetch(x0, y0 + 1, acc, (1 - ax) * ay);
                    fetch(x0 + 1, y0 + 1, acc, ax * ay);
                }
            uint8_t* q = &out.rgba[(size_t(y) * out.w + x) * 4];
            const float a = acc[3] / (kSub * kSub);
            if (acc[3] > 0) {
                q[0] = uint8_t(std::clamp(acc[0] / acc[3], 0.0f, 255.0f) + 0.5f);
                q[1] = uint8_t(std::clamp(acc[1] / acc[3], 0.0f, 255.0f) + 0.5f);
                q[2] = uint8_t(std::clamp(acc[2] / acc[3], 0.0f, 255.0f) + 0.5f);
            }
            q[3] = uint8_t(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    return out;
}

} // namespace nwii::ui
