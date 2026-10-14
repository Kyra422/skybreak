#pragma once
#include "runtime/progress_snapshot.h"
#include <array>
#include <filesystem>
#include <string>
#include <vector>
namespace nwii::ui::achievements {
// Local player-wide collection, like Dusklight. Independent of the Wii save.
class Store {
    std::filesystem::path path_;
    std::array<int64_t, runtime::progress::milestones.size()> times_{};
    uint32_t unlocked_ = 0;
    bool live_ = false, dirty_ = false, writable_ = true;
    std::string error_;
public:
    void Load(const std::filesystem::path& path);
    std::vector<size_t> Observe(const runtime::progress::Snapshot& s, int64_t now);
    bool Save();
    uint32_t mask() const { return unlocked_; }
    bool dirty() const { return dirty_; }
    int count() const;
    const std::string& error() const { return error_; }
};
}
