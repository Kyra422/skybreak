#pragma once
// UI13: the disc's BRFNT font as an ImGui font (custom glyph rectangles in the font atlas).
// CPU only (no GL), so outputs/pc-tests/ui13 can build an atlas and check it.
#include <vector>

#include "imgui.h"
#include "ui_assets.h"

namespace nwii::ui {

struct PendingGlyph {
    int rect = -1;
    Image img;
};

// Adds an ImGui font whose glyphs are the BRFNT glyphs scaled by k (Latin-1 plus curly quotes).
// Call before ImFontAtlas::Build(); then FinishBrfntGlyphs copies the pixels into the atlas.
ImFont* AddBrfntFont(ImFontAtlas* atlas, const BrfntFont& font, float k, const char* name,
                     std::vector<PendingGlyph>& pending);
// After Build(): writes the pending glyph images into the RGBA32 atlas texture data.
int FinishBrfntGlyphs(ImFontAtlas* atlas, std::vector<PendingGlyph>& pending);

} // namespace nwii::ui
