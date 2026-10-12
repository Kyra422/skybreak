#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Asset-only mods. Selection is editable in the launcher/F1; mounts are frozen
// before texture indexing and guest boot, so open archives cannot change midway.
namespace nwii::runtime::mods {
struct Info {
    std::string id, name, version, author, description, error;
    bool enabled = false;
    size_t files = 0, textures = 0, conflicts = 0;
};
void Initialize(const std::filesystem::path& root = {});
std::filesystem::path Root();
std::vector<Info> List();
std::string Error();
bool Rescan();
bool Enable(const std::string& id, bool enabled);
bool Move(const std::string& id, int direction);
bool RestartRequired();
void Mount();
std::filesystem::path Resolve(const std::filesystem::path& disc_relative);
std::vector<std::filesystem::path> TextureDirs();
void ReportUnusedFiles();
}
