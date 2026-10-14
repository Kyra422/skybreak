#pragma once
// LAUNCH19: the player's Skyward Sword (USA, SOUE / title 00010000/534f5545) save: automatic backups at start and
// import/export with Dolphin. Pure C++ std::filesystem (no SDL/ImGui): unit-tested on fake NAND folders
// (outputs/pc-tests/launch19/saves-test.cpp).
//
// The NAND folder has Dolphin's layout (<nand>/title/00010000/534f5545/data/wiiking2.sav, banner.bin, skip.dat;
// metadata in <nand>/fst.bin, which Dolphin also fills in for files without an entry), so a save is a plain copy of
// the data folder. Rules:
//   - backups go to <nand>/../reservekopie/<yyyy-mm-dd_hhmmss>/title/00010000/534f5545/data; only folders with that
//     exact name pattern are ever removed (the oldest beyond `keep`); a save identical to the newest backup is not
//     copied again;
//   - import/export: only this game's save (another title id / region is refused with a reason); the save that
//     would be replaced is first copied to reservekopie/<stamp>-voor-import or <stamp>-dolphin-voor-export;
//     files are copied over, nothing is deleted.
#include <string>
#include <vector>

namespace nwii::ui::saves {

inline constexpr const char* kTitleHigh = "00010000";
inline constexpr const char* kTitleLow = "534f5545"; // "SOUE" = The Legend of Zelda: Skyward Sword (USA)

struct Info {
    bool found = false;          // the data folder has files
    int files = 0;
    std::string data_dir;        // <nand>/title/00010000/534f5545/data
    std::string backup_root;     // <nand>/../reservekopie
    std::string last_backup;     // newest backup folder name ("" = none)
    int backups = 0;
};
Info Inspect(const std::string& nand_root_utf8);

struct Outcome {
    bool ok = false;
    bool skipped = false;        // nothing to do (no save, keep == 0, identical to the newest backup)
    std::string message;         // one line for the player (NL or EN)
    std::string backup;          // the backup folder that was written ("" = none)
    int files = 0;               // files copied
};
// Before the game starts. keep = [nand] backups (0 = off). `stamp` "" = now (tests pass a fixed one).
Outcome BackupAtStart(const std::string& nand_root_utf8, int keep, bool en, const std::string& stamp = "");
// The save folder (.../534f5545/data) inside a folder the player picked: a Dolphin user folder, its Wii folder, a
// title folder or the data folder itself. "" = not found; *why = the reason (NL or EN, e.g. another region).
std::string FindDolphinSave(const std::string& picked_utf8, bool en, std::string* why);
// Dolphin -> SkyBreak and SkyBreak -> Dolphin (the folder the player picked: the Dolphin user folder or its Wii folder).
Outcome ImportFromDolphin(const std::string& picked_utf8, const std::string& nand_root_utf8, bool en,
                          const std::string& stamp = "");
Outcome ExportToDolphin(const std::string& nand_root_utf8, const std::string& picked_utf8, bool en,
                        const std::string& stamp = "");
// "2026-10-10_224012" (local time)
std::string Stamp();

} // namespace nwii::ui::saves
