#pragma once
// UI13: host-side readers for the launcher's look, built at run time from the player's own
// disc files (nothing from the game is shipped): GX texture decoders (Dolphin TextureDecoder
// semantics), U8 archives, TPL images, THP video frames (as standard JPEG) and BRFNT fonts.
// Pure C++ (no SDL/GL/ImGui) so outputs/pc-tests/ui13 can test it directly.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nwii::ui {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba; // w*h*4, row-major, top row first, straight alpha
};

// GX texture formats (GXTexFmt).
enum GxFormat : int { kGxI4 = 0, kGxI8 = 1, kGxIA4 = 2, kGxIA8 = 3, kGxRGB565 = 4, kGxRGB5A3 = 5,
                      kGxRGBA8 = 6, kGxCMPR = 14 };

// Encoded size of a w x h texture in `fmt` (tiles padded); 0 = unsupported format.
size_t GxTextureSize(int w, int h, int fmt);
// Decodes a tiled GX texture. False (and *err) on an unsupported format or short data.
bool DecodeGxTexture(const uint8_t* data, size_t size, int w, int h, int fmt, Image& out,
                     std::string* err = nullptr);

struct U8Entry {
    std::string path; // "dir/sub/name" (no leading '/' or '.')
    uint32_t offset = 0, size = 0;
};
bool ParseU8(const std::vector<uint8_t>& arc, std::vector<U8Entry>& out, std::string* err = nullptr);
// Entry whose path ends with `name` (case-insensitive); nullptr if none.
const U8Entry* FindU8(const std::vector<U8Entry>& entries, const std::string& name);

// First image of a TPL file.
bool DecodeTpl(const uint8_t* data, size_t size, Image& out, std::string* err = nullptr);

struct ThpInfo {
    int frames = 0, width = 0, height = 0;
    float fps = 0.0f;
};
// Video frame `frame` of a THP movie as a standard JPEG: THP leaves 0xFF bytes in the entropy
// coded data unstuffed, so every 0xFF of the scan gets its 0x00 back (THP-codec test, compare.py).
bool ThpFrameJpeg(const std::string& path, int frame, std::vector<uint8_t>& jpeg, ThpInfo* info = nullptr,
                  std::string* err = nullptr);
bool RestoreJpegStuffing(const std::vector<uint8_t>& thp_jpeg, std::vector<uint8_t>& jpeg,
                         std::string* err = nullptr);

// BRFNT (NW4R RFNT 1.4) glyph font.
struct BrfntGlyph {
    uint32_t code = 0;   // Unicode code point (UTF-16 fonts)
    int index = 0;       // glyph index
    int left = 0, glyph_width = 0, char_width = 0;
};
struct BrfntFont {
    int font_type = 0, leading = 0, ascent = 0, encoding = 0;
    int cell_w = 0, cell_h = 0, baseline = 0, max_char_w = 0;
    int sheet_w = 0, sheet_h = 0, sheet_count = 0, sheet_fmt = 0, sheet_row = 0, sheet_line = 0;
    int default_index = 0;
    std::vector<std::vector<uint8_t>> sheets; // per sheet: RGBA (sheet_w * sheet_h * 4)
    std::vector<BrfntGlyph> glyphs;           // sorted by code
    const BrfntGlyph* Find(uint32_t code) const;
};
bool ParseBrfnt(const std::vector<uint8_t>& file, BrfntFont& out, std::string* err = nullptr);
// The glyph's cell (glyph_width x cell_h) as RGBA. Intensity sheets (I4/I8: white fill, grey
// outline in SS's fonts) become white text with a black outline; IA sheets keep their alpha.
bool BrfntGlyphImage(const BrfntFont& font, const BrfntGlyph& glyph, Image& out);

// Resample with premultiplied 4x4 supersampling (good for both up- and downscaling).
Image Resample(const Image& src, int w, int h);

bool ReadFile(const std::string& path_utf8, std::vector<uint8_t>& out);

} // namespace nwii::ui
