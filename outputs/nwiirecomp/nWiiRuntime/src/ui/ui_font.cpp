// UI13: BRFNT -> ImGui font atlas (see ui_font.h).
#include "ui_font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nwii::ui {

ImFont* AddBrfntFont(ImFontAtlas* atlas, const BrfntFont& f, float k, const char* name,
                     std::vector<PendingGlyph>& pending) {
    // The container font only supplies metrics; every glyph comes from the BRFNT. Glyph tops are
    // at the line top (offset y 0): the line is one BRFNT cell high, baseline at cell row f.baseline.
    static const ImWchar kSpaceOnly[] = {0x20, 0x20, 0};
    ImFontConfig cfg;
    cfg.SizePixels = std::max(8.0f, std::round(f.cell_h * k));
    cfg.GlyphRanges = kSpaceOnly;
    cfg.OversampleH = cfg.OversampleV = 1;
    std::snprintf(cfg.Name, sizeof cfg.Name, "%s", name);
    ImFont* font = atlas->AddFontDefault(&cfg);
    for (const BrfntGlyph& gl : f.glyphs) {
        const bool latin = (gl.code >= 0x20 && gl.code <= 0x7E) || (gl.code >= 0xA0 && gl.code <= 0xFF);
        if (!latin && gl.code != 0x2019 && gl.code != 0x201C && gl.code != 0x201D) continue;
        Image src;
        if (!BrfntGlyphImage(f, gl, src)) continue;
        const int w = std::max(1, int(std::lround(src.w * k))), h = std::max(1, int(std::lround(src.h * k)));
        PendingGlyph p;
        p.rect = atlas->AddCustomRectFontGlyph(font, ImWchar(gl.code), w, h, gl.char_width * k,
                                               ImVec2(std::round(gl.left * k), 0.0f));
        p.img = Resample(src, w, h);
        pending.push_back(std::move(p));
    }
    return font;
}

int FinishBrfntGlyphs(ImFontAtlas* atlas, std::vector<PendingGlyph>& pending) {
    unsigned char* px = nullptr;
    int tw = 0, th = 0, n = 0;
    atlas->GetTexDataAsRGBA32(&px, &tw, &th);
    if (!px) return 0;
    for (const PendingGlyph& p : pending) {
        const ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(p.rect);
        if (!r || !r->IsPacked()) continue;
        const int w = std::min<int>(p.img.w, tw - r->X);
        for (int y = 0; y < p.img.h && r->Y + y < th; ++y)
            std::memcpy(px + (size_t(r->Y + y) * tw + r->X) * 4, p.img.rgba.data() + size_t(y) * p.img.w * 4,
                        size_t(std::max(0, w)) * 4);
        ++n;
    }
    pending.clear();
    return n;
}

} // namespace nwii::ui
