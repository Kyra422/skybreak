// INSTALL19: disc validation (see disc_probe.h).
#include "disc_probe.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#include "ui_disc.h" // nwii::ui::CheckDisc, compiled read-only from nWiiRuntime/src/ui (not edited)

namespace sb {

const char* DiscKindName(DiscKind k) {
    switch (k) {
    case DiscKind::Missing: return "missing";
    case DiscKind::Folder: return "folder";
    case DiscKind::Iso: return "iso";
    case DiscKind::Wbfs: return "wbfs";
    case DiscKind::Rvz: return "rvz";
    case DiscKind::Wia: return "wia";
    case DiscKind::Gcz: return "gcz";
    case DiscKind::Ciso: return "ciso";
    case DiscKind::Unknown: return "unknown";
    }
    return "?";
}

const char* DiscVerdictName(DiscVerdict v) {
    switch (v) {
    case DiscVerdict::Ok: return "ok";
    case DiscVerdict::NeedsTool: return "needs_tool";
    case DiscVerdict::WrongRegion: return "wrong_region";
    case DiscVerdict::WrongRevision: return "wrong_revision";
    case DiscVerdict::WrongGame: return "wrong_game";
    case DiscVerdict::BadFolder: return "bad_folder";
    case DiscVerdict::Unreadable: return "unreadable";
    case DiscVerdict::Missing: return "missing";
    }
    return "?";
}

namespace {

bool ReadAt(std::ifstream& f, uint64_t off, void* buf, size_t n) {
    f.clear();
    f.seekg(std::streamoff(off));
    if (!f) return false;
    f.read(static_cast<char*>(buf), std::streamsize(n));
    return size_t(f.gcount()) == n;
}

uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

bool PrintableId(const uint8_t* p) {
    for (int i = 0; i < 6; ++i)
        if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) return false;
    return true;
}

// Disc header (the first 0x60+ bytes of a Wii disc): id at 0, revision at 7, Wii magic at 0x18, name at 0x20.
bool ParseDiscHeader(const uint8_t* h, size_t n, DiscProbe& d, bool require_magic) {
    if (n < 0x60) return false;
    bool wii = BE32(h + 0x18) == 0x5D1C9EA3;
    bool gc = BE32(h + 0x1C) == 0xC2339F3D;
    if (require_magic && !wii && !gc) return false;
    if (!PrintableId(h)) return false;
    d.game_id.assign(reinterpret_cast<const char*>(h), 6);
    d.revision = h[7];
    size_t len = 0;
    while (len < 64 && 0x20 + len < n && h[0x20 + len]) ++len;
    d.internal_name.assign(reinterpret_cast<const char*>(h + 0x20), len);
    return true;
}

const char* RegionName(char r, bool en) {
    switch (r) {
    case 'P': return en ? "European (PAL)" : "Europese (PAL)";
    case 'J': return en ? "Japanese" : "Japanse";
    case 'K': return en ? "Korean" : "Koreaanse";
    default: return nullptr;
    }
}

} // namespace

void ClassifyDisc(DiscProbe& d) {
    const std::string kind = Lower(DiscKindName(d.kind));
    if (d.game_id.empty()) {
        if (d.verdict == DiscVerdict::Missing) return;
        d.verdict = DiscVerdict::Unreadable;
        d.msg_nl = "Niet herkend als Wii-schijf. Kies een .iso, .wbfs, .rvz, .wia of .gcz van je eigen Skyward Sword (VS, versie 1.00), of een uitgepakte map.";
        d.msg_en = "Not recognised as a Wii disc. Choose an .iso, .wbfs, .rvz, .wia or .gcz of your own Skyward Sword (USA, version 1.00), or an extracted folder.";
        return;
    }
    d.detail = Format("%s - %s - revisie %d%s", Lower(DiscKindName(d.kind)).c_str(), d.game_id.c_str(), d.revision,
                      d.internal_name.empty() ? "" : (" - '" + d.internal_name + "'").c_str());
    if (d.game_id != "SOUE01") {
        const char* nl = d.game_id.size() >= 4 && d.game_id.compare(0, 3, "SOU") == 0 ? RegionName(d.game_id[3], false) : nullptr;
        const char* en = d.game_id.size() >= 4 && d.game_id.compare(0, 3, "SOU") == 0 ? RegionName(d.game_id[3], true) : nullptr;
        if (nl) {
            d.verdict = DiscVerdict::WrongRegion;
            d.msg_nl = Format("Dit is de %s versie van Skyward Sword (%s). Alleen de Amerikaanse versie 1.00 (SOUE01, revisie 0) wordt ondersteund.", nl, d.game_id.c_str());
            d.msg_en = Format("This is the %s version of Skyward Sword (%s). Only the American version 1.00 (SOUE01, revision 0) is supported.", en, d.game_id.c_str());
        } else {
            d.verdict = DiscVerdict::WrongGame;
            d.msg_nl = Format("Dit is geen Skyward Sword-schijf voor dit project (spelcode %s, '%s'). Nodig: SOUE01, revisie 0.", d.game_id.c_str(), d.internal_name.c_str());
            d.msg_en = Format("This is not a Skyward Sword disc for this project (game id %s, '%s'). Needed: SOUE01, revision 0.", d.game_id.c_str(), d.internal_name.c_str());
        }
        return;
    }
    if (d.revision != 0) {
        d.verdict = DiscVerdict::WrongRevision;
        d.msg_nl = Format("Dit is SOUE01 revisie %d. Alleen revisie 0 (versie 1.00) wordt ondersteund.", d.revision);
        d.msg_en = Format("This is SOUE01 revision %d. Only revision 0 (version 1.00) is supported.", d.revision);
        return;
    }
    d.verdict = DiscVerdict::Ok;
    d.msg_nl = "Schijf herkend: The Legend of Zelda: Skyward Sword, VS versie 1.00 (SOUE01, revisie 0).";
    d.msg_en = "Disc recognised: The Legend of Zelda: Skyward Sword, USA version 1.00 (SOUE01, revision 0).";
    if (d.nkit) {
        d.msg_nl += " Let op: dit is een NKit-bestand. Dat kan werken; faalt de controle tijdens het uitpakken, maak dan een gewone dump.";
        d.msg_en += " Note: this is an NKit image. It may work; if the check fails during extraction, make a normal dump.";
    }
    (void)kind;
}

fs::path FindGameRoot(const fs::path& dir) {
    std::error_code ec;
    for (const fs::path& c : {dir, dir / L"DATA", dir / L"P-SOUE"})
        if (fs::is_regular_file(c / L"sys" / L"main.dol", ec)) return c;
    // one level down (e.g. a folder that contains the DATA partition under another name)
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && fs::is_regular_file(it->path() / L"sys" / L"main.dol", ec)) return it->path();
    }
    return {};
}

DiscProbe ProbeDisc(const fs::path& p) {
    DiscProbe d;
    d.path = p;
    std::error_code ec;
    if (p.empty() || !fs::exists(p, ec)) {
        d.kind = DiscKind::Missing;
        d.verdict = DiscVerdict::Missing;
        d.msg_nl = "Bestand of map niet gevonden.";
        d.msg_en = "File or folder not found.";
        return d;
    }
    if (fs::is_directory(p, ec)) {
        d.kind = DiscKind::Folder;
        fs::path root = FindGameRoot(p);
        if (root.empty()) {
            d.verdict = DiscVerdict::BadFolder;
            d.msg_nl = "In deze map staat geen uitgepakte Wii-schijf (sys\\main.dol ontbreekt). Kies de map met sys\\ en files\\.";
            d.msg_en = "This folder does not contain an extracted Wii disc (sys\\main.dol is missing). Choose the folder with sys\\ and files\\.";
            return d;
        }
        d.game_root = root;
        uint8_t h[0x440] = {};
        std::ifstream bf(root / L"sys" / L"boot.bin", std::ios::binary);
        if (bf) bf.read(reinterpret_cast<char*>(h), sizeof h);
        size_t got = bf ? sizeof h : size_t(bf.gcount());
        if (!ParseDiscHeader(h, got, d, false)) {
            d.verdict = DiscVerdict::BadFolder;
            d.msg_nl = "sys\\boot.bin ontbreekt of is onleesbaar in deze map.";
            d.msg_en = "sys\\boot.bin is missing or unreadable in this folder.";
            return d;
        }
        ClassifyDisc(d);
        if (!d.ok()) return d;
        // Same check as the launcher: SHA-1 of main.dol and rels.arc.
        nwii::ui::DiscStatus st = nwii::ui::CheckDisc(U(root));
        if (!st.ready) {
            d.verdict = DiscVerdict::BadFolder;
            d.msg_nl = "De map is SOUE01, maar de bestanden kloppen niet: " + st.message +
                       " Pak de schijf opnieuw uit (Dolphin > Eigenschappen > Bestandssysteem > Hele schijf uitpakken).";
            d.msg_en = std::string("The folder is SOUE01, but the files do not match (") + (st.dol_ok ? "rels.arc" : "main.dol") +
                       " SHA-1). Extract the disc again (Dolphin > Properties > Filesystem > Extract Entire Disc).";
            return d;
        }
        d.size_bytes = DirSize(root);
        d.msg_nl = "Uitgepakte map herkend: SOUE01 revisie 0; main.dol en rels.arc kloppen (SHA-1).";
        d.msg_en = "Extracted folder recognised: SOUE01 revision 0; main.dol and rels.arc match (SHA-1).";
        return d;
    }

    d.size_bytes = fs::file_size(p, ec);
    std::ifstream f(p, std::ios::binary);
    uint8_t h[0x200] = {};
    if (!f || d.size_bytes < 0x60 || !ReadAt(f, 0, h, std::min<uint64_t>(sizeof h, d.size_bytes))) {
        d.kind = DiscKind::Unknown;
        d.verdict = DiscVerdict::Unreadable;
        ClassifyDisc(d);
        return d;
    }
    uint8_t hdr[0x100] = {};
    if (!memcmp(h, "RVZ\x01", 4) || !memcmp(h, "WIA\x01", 4)) {
        d.kind = h[0] == 'R' ? DiscKind::Rvz : DiscKind::Wia;
        // header_1 (0x48 bytes) then header_2: disc_type, compression, level, chunk_size, then the disc header copy.
        if (d.size_bytes >= 0x58 + 0x80 && ReadAt(f, 0x58, hdr, 0x80)) ParseDiscHeader(hdr, 0x80, d, true);
    } else if (!memcmp(h, "WBFS", 4)) {
        d.kind = DiscKind::Wbfs;
        uint8_t shift = h[8];
        if (shift >= 9 && shift <= 16) {
            uint64_t off = uint64_t(1) << shift; // disc info of slot 0 starts with the disc header copy
            if (ReadAt(f, off, hdr, sizeof hdr)) ParseDiscHeader(hdr, sizeof hdr, d, true);
        }
    } else if (!memcmp(h, "CISO", 4)) {
        d.kind = DiscKind::Ciso;
        if (ReadAt(f, 0x8000, hdr, sizeof hdr)) ParseDiscHeader(hdr, sizeof hdr, d, true);
    } else if (BE32(h) == 0xB10BC001 || (h[0] == 0x01 && h[1] == 0xC0 && h[2] == 0x0B && h[3] == 0xB1)) {
        d.kind = DiscKind::Gcz;
    } else if (ParseDiscHeader(h, sizeof h, d, true)) {
        d.kind = DiscKind::Iso;
        uint8_t nk[4] = {};
        if (ReadAt(f, 0x200, nk, 4) && !memcmp(nk, "NKIT", 4)) d.nkit = true;
    } else {
        d.kind = DiscKind::Unknown;
    }
    if (d.kind == DiscKind::Gcz || (d.game_id.empty() && d.kind != DiscKind::Unknown && d.kind != DiscKind::Iso)) {
        if (d.kind == DiscKind::Gcz) {
            d.verdict = DiscVerdict::NeedsTool;
            d.msg_nl = "GCZ-bestand: dit formaat controleert DolphinTool tijdens de installatie.";
            d.msg_en = "GCZ image: DolphinTool checks this format during installation.";
            return d;
        }
    }
    if (d.game_id.empty()) d.verdict = DiscVerdict::Unreadable;
    ClassifyDisc(d);
    return d;
}

} // namespace sb
