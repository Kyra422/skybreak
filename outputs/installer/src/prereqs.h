#pragma once
// INSTALL19: prerequisites (Visual Studio 2022 Build Tools with C++/CMake/Ninja, Python 3.8+, DolphinTool).
// Detection is READ-ONLY (vswhere, --version runs, file and registry reads). Installing happens only after the
// player's consent on a separate page, and never in --sandbox. IPrereqEnv has a real and a mock implementation
// (--mock-prereqs FILE.json: simulated states, exit codes and verification failures; never launches anything).
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "download.h"
#include "json.h"

namespace sb {

class ProcessRunner;
struct IPlatform;

struct PrereqItem {
    std::string id;           // "vs", "python", "dolphintool"
    std::string name;         // display name
    bool present = false;
    bool required = true;     // dolphintool is only required for image files
    std::string path;         // found location
    std::string version;
    std::string note_nl, note_en;
};

struct SystemInfo {
    int cores = 1;
    double ram_gb = 0;
    std::string windows;
    bool winget = false;
    bool tar_lzma = false;
    int smart_app_control = -1; // -1 unknown, 0 off, 1 on, 2 evaluation
    int long_paths = -1;        // LongPathsEnabled (read only)
};

struct PrereqReport {
    std::vector<PrereqItem> items;
    SystemInfo sys;
    fs::path python, cmake, ninja, dolphintool, vs; // what the build gets passed
    const PrereqItem* get(const std::string& id) const;
    PrereqItem* get(const std::string& id);
    std::vector<std::string> MissingRequired() const;
};

struct DetectContext {
    fs::path install_dir;          // for <install>\hulpmiddelen\{python,dolphin}
    bool need_dolphintool = true;  // false for an extracted folder
};

struct InstallOutcome {
    bool ok = false;
    bool restart_needed = false; // 3010
    bool manual = false;         // route "zelf doen" (e.g. browser opened)
    int code = -1;
    std::string route;           // "winget", "bootstrapper", "embeddable", "7z", "browser", "mock"
    std::string msg_nl, msg_en;
};

struct ConsentInfo { // shown on the consent page
    std::string what, publisher, url, size, license, license_url, changes_nl, changes_en;
    bool admin = false;
};

class IPrereqEnv {
public:
    virtual ~IPrereqEnv() = default;
    virtual bool IsMock() const = 0;
    virtual PrereqReport Detect(const DetectContext& ctx) = 0;
    virtual ConsentInfo Consent(const std::string& id) const = 0;
    virtual InstallOutcome Install(const std::string& id, const DetectContext& ctx, const ProgressFn& progress,
                                   std::atomic<bool>* cancel) = 0;
};

Json LoadPrereqManifest(const fs::path& file); // prereqs.json (array or {"items":[...]})
std::unique_ptr<IPrereqEnv> MakeRealPrereqs(ProcessRunner& runner, IPlatform& platform, Json manifest);
std::unique_ptr<IPrereqEnv> MakeMockPrereqs(Json mock, Json manifest);

// After a download: verify the pinned SHA-256 (empty = refuse), extract `entries` with tar into `tmp`, copy them
// (files and folders) into `dir`; true when dir\DolphinTool.exe exists. Nothing is extracted on a hash mismatch.
bool ExtractPinnedArchive(ProcessRunner& run, const fs::path& tar, const fs::path& arc, const std::string& want_sha256,
                          const std::vector<std::string>& entries, const fs::path& tmp, const fs::path& dir, std::string* err);

// Jobs for the build: max(1, min(cores - 2, floor(ram_gb / 1.5))).
int SuggestedJobs(const SystemInfo& s);

} // namespace sb
