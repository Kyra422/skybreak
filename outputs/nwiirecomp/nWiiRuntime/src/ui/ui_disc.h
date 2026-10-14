#pragma once
// UI13: "Schijf gereed" check for the launcher: the extracted disc (sys/boot.bin game id and
// revision, sys/main.dol SHA-1, files/rels.arc SHA-1). Same expectations as the release tool
// (outputs/release/tools/pkg10_release.py verify-game). Pure C++.
#include <cstddef>
#include <cstdint>
#include <string>

namespace nwii::ui {

struct Sha1 {
    Sha1();
    void Update(const void* data, size_t n);
    std::string HexDigest(); // finalises
private:
    void Block(const uint8_t* p);
    uint32_t h_[5];
    uint64_t len_ = 0;
    uint8_t buf_[64];
    size_t used_ = 0;
};
// Lower-case hex SHA-1 of a file; empty when it cannot be read.
std::string Sha1File(const std::string& path_utf8);

inline constexpr const char* kMainDolSha1 = "450a6806f46d59dcf8278db08e06f94865a4b18a";
inline constexpr const char* kRelsArcSha1 = "a9d58dfea7071fad1c710bf3040529ac274e77f9";

struct DiscStatus {
    bool ready = false;        // everything matches
    std::string game_id;       // "SOUE01"
    int revision = -1;
    bool dol_ok = false, rels_ok = false;
    std::string message;       // Dutch, one line ("Schijf gereed." or the problem)
    std::string detail;        // Dutch, e.g. "Wii - SOUE01 (NTSC-U) - revisie 0"
};
// expected_* empty = use the SOUE01 rev 0 constants above.
DiscStatus CheckDisc(const std::string& game_dir_utf8, const char* expected_dol = nullptr,
                     const char* expected_rels = nullptr);

} // namespace nwii::ui
