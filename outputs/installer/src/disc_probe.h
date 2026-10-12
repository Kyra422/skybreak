#pragma once
// INSTALL19: disc validation for the Setup "Schijf" page.
//  - Image files: native header probe (reads at most a few KB; no key, no decompression) for ISO, WBFS, RVZ/WIA
//    (disc header at 0x58, measured on the user's own rvz files), CISO; GCZ and unknown -> DolphinTool needed.
//  - Extracted folder: nwii::ui::CheckDisc (boot.bin id/revision + SHA-1 of main.dol and rels.arc), the same check
//    as the launcher's "Schijf gereed".
// Messages match Bouw-SkywardSword.ps1 step 2 (NL) with English equivalents.
#include <string>

#include "common.h"

namespace sb {

enum class DiscKind { Missing, Folder, Iso, Wbfs, Rvz, Wia, Gcz, Ciso, Unknown };
const char* DiscKindName(DiscKind k);

enum class DiscVerdict {
    Ok,             // SOUE01 rev 0 (folder: hashes verified too)
    NeedsTool,      // format we cannot read natively (GCZ, ...): DolphinTool decides
    WrongRegion,    // SOUP01 / SOUJ01 / SOUK01
    WrongRevision,  // SOUE01 rev != 0
    WrongGame,      // other game id
    BadFolder,      // folder without sys/main.dol, or SHA-1 mismatch
    Unreadable,     // not recognised as a Wii disc / empty / too short
    Missing,        // path does not exist
};
const char* DiscVerdictName(DiscVerdict v);

struct DiscProbe {
    fs::path path;            // as given
    fs::path game_root;       // folder mode: the directory that contains sys/ and files/
    DiscKind kind = DiscKind::Missing;
    DiscVerdict verdict = DiscVerdict::Missing;
    std::string game_id;      // "SOUE01"
    int revision = -1;
    std::string internal_name;
    bool nkit = false;
    uint64_t size_bytes = 0;
    std::string msg_nl, msg_en; // one sentence for the status card
    std::string detail;         // e.g. "RVZ - SOUE01 - revisie 0"
    bool ok() const { return verdict == DiscVerdict::Ok; }
};

// Finds the game root inside an extracted folder (dir, dir/DATA, dir/P-SOUE, or one level down). Empty if none.
fs::path FindGameRoot(const fs::path& dir);

DiscProbe ProbeDisc(const fs::path& p);
// Fills verdict + messages from game_id/revision (used by ProbeDisc and for DolphinTool `header -j` output).
void ClassifyDisc(DiscProbe& d);

} // namespace sb
