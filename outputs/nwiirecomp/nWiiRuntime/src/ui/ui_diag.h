#pragma once
// LAUNCH19: "Diagnosepakket maken..." (GEAVANCEERD): a zip for a bug report with the newest SkyBreak logs (at most 5,
// the last 4 MB of each), the settings file and a systeeminfo.txt (Windows, GPU/driver, controllers, SkyBreak
// version, disc check result). Nothing of the game: no disc files, no save, no screenshots. Stored (uncompressed)
// zip written by our own code (CRC-32), so no library and no shell call. Pure C++ (unit-tested).
#include <string>
#include <utility>
#include <vector>

namespace nwii::ui::diag {

struct Request {
    std::string out_dir;       // where the zip goes (created when missing)
    std::string log_dir;       // *.log files (newest 5)
    std::string settings_path; // instellingen.toml
    std::string system_info;   // systeeminfo.txt text
    std::string stamp;         // "" = now
};
// Returns the zip path ("" on failure, *err = why). Name: SkyBreak-diagnose-<yyyy-mm-dd_hhmmss>.zip
std::string MakePackage(const Request& r, std::string* err);

// Writes a stored zip of (name, bytes) entries. Names use '/' and are ASCII/UTF-8 (flag bit 11 set).
bool WriteStoredZip(const std::string& path_utf8, const std::vector<std::pair<std::string, std::string>>& entries,
                    std::string* err);
uint32_t Crc32(const std::string& bytes);

} // namespace nwii::ui::diag
