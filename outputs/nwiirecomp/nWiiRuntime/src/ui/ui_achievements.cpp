#include "ui_achievements.h"
#include <fstream>
#include <toml++/toml.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace nwii::ui::achievements {
void Store::Load(const std::filesystem::path& path) {
    path_ = path; times_ = {}; unlocked_ = 0; live_ = dirty_ = false; writable_ = true; error_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) { writable_ = false; error_ = ec.message(); }
        return;
    }
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("Achievementbestand is niet leesbaar.");
        auto data = toml::parse(f);
        if (data["version"].value<int>() != 1) throw std::runtime_error("Onbekende achievementversie.");
        for (size_t i = 0; i < times_.size(); ++i) {
            times_[i] = data["unlocked"][runtime::progress::milestones[i].id].value_or<int64_t>(0);
            if (times_[i] > 0) unlocked_ |= 1u << i;
        }
    } catch (const std::exception& e) {
        writable_ = false; error_ = e.what(); // Preserve an unreadable/unknown file, never overwrite it.
    }
}
std::vector<size_t> Store::Observe(const runtime::progress::Snapshot& s, int64_t now) {
    std::vector<size_t> notify;
    if (!s.playing) { live_ = false; return notify; }
    for (size_t i = 0; i < times_.size(); ++i) {
        if ((s.mask & (1u << i)) && !(unlocked_ & (1u << i))) {
            unlocked_ |= 1u << i; times_[i] = std::max<int64_t>(1, now); dirty_ = true;
            if (live_) notify.push_back(i); // Quietly import existing milestones on entering a save.
        }
    }
    live_ = true;
    return notify;
}
int Store::count() const {
    int n = 0; for (size_t i = 0; i < times_.size(); ++i) if (unlocked_ & (1u << i)) ++n; return n;
}
bool Store::Save() {
    if (!dirty_) return true;
    if (!writable_ || path_.empty()) return false;
    try {
        std::filesystem::create_directories(path_.parent_path());
        auto temp = path_; temp += ".tmp";
        {
            std::ofstream f(temp, std::ios::binary | std::ios::trunc);
            f << "version = 1\n\n[unlocked]\n";
            for (size_t i = 0; i < times_.size(); ++i)
                if (times_[i] > 0) f << runtime::progress::milestones[i].id << " = " << times_[i] << '\n';
            f.flush(); if (!f) throw std::runtime_error("Achievementbestand kon niet worden geschreven.");
        }
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Achievementbestand kon niet worden vervangen.");
#else
        std::filesystem::rename(temp, path_);
#endif
        dirty_ = false; error_.clear(); return true;
    } catch (const std::exception& e) { error_ = e.what(); return false; }
}
}
