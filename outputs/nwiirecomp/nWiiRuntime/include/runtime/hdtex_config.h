#pragma once
// HDTEX16: the [graphics] keys of the custom ("HD") texture loader and the texture dumper. Parsed by
// Config::load (src/core/config.cpp, load_hdtex16) like [ui]; kept out of config.h on purpose (every
// generated output_*.cpp includes config.h). Used by src/hle/gx/hd_textures.cpp and the settings UI.
//
// [graphics]
//   hd_textures = true          load Dolphin-named custom textures (tex1_*.png / .dds) when a pack exists.
//                               Default (key absent): on when a pack folder with tex1_ files exists.
//                               Env NWII_HD_TEXTURES=0/1 overrides.
//   hd_textures_preload = false decode the pack into RAM in the background from the start (less pop-in; RAM
//                               cache = 1/4 of the physical RAM, 1.5..8 GB, 2..6 below-normal decode threads).
//                               Env NWII_HDTEX_PRELOAD=0/1 overrides.
//   texture_dump = false        write every texture the game loads as PNG (Dolphin names) to
//                               <game root>/dump/textures/<game id>/. Env NWII_TEX_DUMP=0/1 overrides.
//   texture_pack_dir = ""       an extra pack folder (searched first); relative = next to the settings
//                               file. The default folders are <game root>/textures/<game id> (+ /SOU).
//   glyph_set = "auto"          CTRL18-UI: controller button pictures from <game root>/textures/glyphs/<game id>/
//                               <set>/, searched before every other folder: "auto" (last used device + its preset)
//                               | "ps" | "xbox" | "switch" | "kbm" | "wii" (original) | a set folder name.
//                               Env NWII_GLYPH_SET overrides (runtime/gx/glyph_sets.h, hd_textures.h).
#include <string>

namespace nwii::runtime {

struct HdTexConfig {
    int hd_textures = -1; // -1 = auto (on when a pack exists), 0 off, 1 on
    bool preload = false;
    bool dump = false;
    std::string pack_dir; // resolved (Config::resolve_path); empty = defaults only
    std::string glyph_set = "auto"; // CTRL18-UI: [graphics] glyph_set
};
HdTexConfig& hdtex_config(); // config.cpp

} // namespace nwii::runtime
