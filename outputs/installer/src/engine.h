#pragma once
// INSTALL19: the installer engine shared by the GUI wizard and --headless: package check, disc probe, folders and
// space, prerequisites, source sync, build pipeline, finishing (start program, shortcuts, uninstall entry,
// install.json), update/repair/resume detection and uninstall. All system side effects go through IPlatform and
// ProcessRunner, so --sandbox keeps everything inside the sandbox folder.
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "disc_probe.h"
#include "json.h"
#include "pipeline.h"
#include "platform.h"
#include "prereqs.h"
#include "proc.h"

namespace sb {

// Headless exit codes (design §3).
enum ExitCode { kExitOk = 0, kExitError = 1, kExitDisc = 2, kExitPrereq = 3, kExitCancelled = 4, kExitSpace = 5, kExitPackage = 6,
                kExitRunning = 7 /* REL22: SkyBreak of this installation still runs; nothing changed */,
                kExitBusy = 8 /* Another real Setup process owns the installer session. */ };

struct Options {
    bool headless = false;
    fs::path answers;
    fs::path sandbox;          // --sandbox DIR
    fs::path mock_prereqs;     // --mock-prereqs FILE
    fs::path package_dir;      // --package DIR (default: folder of the exe)
    fs::path log;              // --log FILE
    std::string lang;          // --lang nl|en
    bool uninstall = false, modify = false, verify_package = false, delete_saves = false, from_temp = false;
    fs::path install_dir;      // --install-dir (uninstall/modify)
    bool warp = false;         // --warp
    fs::path ui_snapshot;      // --ui-snapshot DIR
    std::string mutate;        // test mutants: leak-desktop, skip-consent, skip-legal (REVINSTALL19 continuation: + old-space-rules, allow-non-ascii)
    fs::path build_script;     // --build-script FILE (tests: a stand-in for Bouw-SkywardSword.ps1)
    int cancel_at_step = 0, cancel_at_a = 0; // --cancel-at STEP:A (tests)
    bool no_dialog = false;
    double mock_free_gb = -1;  // REVINSTALL19: --mock-free-gb N (tests; honoured only together with --sandbox)
    int wait_closed_s = 0;     // REL22: --wait-closed N: headless waits up to N s for SkyBreak of this installation to close
};

struct Choices {
    std::string lang = "en";
    bool accepted = false;      // "own copy" checkbox
    fs::path disc;
    fs::path install_dir, data_dir;
    bool copy_folder = true;    // extracted folder: copy (recommended) or point to it
    bool keep_build = true;
    bool startmenu = true, desktop = true;
    std::string old_saves = "copy"; // copy | use_old | fresh
    int jobs = 0;
    Json consent = Json::Object(); // id -> bool
};

struct PathIssue {
    bool error = false; // true blocks Next
    std::string nl, en;
};

struct SpaceRow {
    std::string drive;
    uint64_t need = 0, free = 0;
    bool red() const { return free < need; }
    bool amber() const { return !red() && free < need + need / 10; }
};

enum class Mode { Install, Update, Repair, Resume };

class Engine {
public:
    explicit Engine(const Options& o);

    const Options& opt() const { return opt_; }
    IPlatform& plat() { return *plat_; }
    ProcessRunner& runner() { return runner_; }
    IPrereqEnv& prereqs() { return *prereqs_; }
    Pipeline& pipeline() { return pipeline_; }

    // ---- package
    fs::path PackageDir() const { return package_dir_; }
    std::string PackageVersion() const { return package_version_; }
    bool InstalledLayout() const;
    // MANIFEST.sha256 of the package; `bad` lists missing/changed files.
    bool VerifyPackage(std::vector<std::string>* bad, std::string* err);

    // ---- existing installation
    Mode mode() const { return mode_; }
    const Json& Existing() const { return existing_; }
    fs::path ExistingDir() const;
    void DetectExisting(const fs::path& preferred = {}); // INSTALL19 10 Oct: the chosen install folder is checked first

    // ---- defaults and checks
    fs::path DefaultInstallDir();
    fs::path DefaultDataDir();
    fs::path OldSavesDir();          // %APPDATA%\SkywardSwordPC\nand (sandboxed in --sandbox)
    bool OldSavesAvailable();
    bool OldSavesElsewhere(const Choices& c); // INSTALL19 10 Oct: old saves exist AND the chosen data folder is another folder
    std::vector<PathIssue> CheckInstallDir(const fs::path& d);
    std::vector<PathIssue> CheckDiscChoice(const Choices& c, const DiscProbe& d); // REVINSTALL19 continuation: pointer mode + non-ASCII
    bool ForeignContentFree(const fs::path& d) const; // REVINSTALL19: missing/empty, a SkyBreak install, or only Setup's own leftovers
    std::vector<SpaceRow> Space(const Choices& c, const DiscProbe& d, const PrereqReport& r);
    std::string NandRootToml(const Choices& c);
    std::string TimeEstimate(const PrereqReport& r, const Choices& c, const DiscProbe& d, bool en);

    // ---- steps
    DiscProbe Probe(const fs::path& p) { return ProbeDisc(p); }
    PrereqReport Detect(const Choices& c, const DiscProbe& d);
    // Copy <package>\bron into <install>\bron, only files whose content changed (keeps mtimes -> incremental ninja).
    bool SyncSources(const fs::path& install_dir, std::string* err, int* copied, int* unchanged, int* removed);
    bool MigrateOldSaves(const Choices& c, std::string* msg);
    BuildOptions MakeBuildOptions(const Choices& c, const DiscProbe& d, const PrereqReport& r);
    bool StartBuild(const Choices& c, const DiscProbe& d, const PrereqReport& r, std::string* err);
    bool Finish(const Choices& c, const DiscProbe& d, std::string* err);

    // ---- install.json
    fs::path StatePath(const fs::path& install_dir) const { return install_dir / L"setup" / L"install.json"; }
    Json& state() { return state_; }
    void SaveState(const fs::path& install_dir);
    void SetPhase(const fs::path& install_dir, const std::string& phase);

    // ---- uninstall
    struct UninstallResult {
        bool ok = false;
        std::vector<std::string> removed, kept_unknown, errors;
        bool saves_removed = false;
        bool running = false; // REL22: SkyBreak of this installation was running -> nothing removed (errors[0] = RunningText)
    };
    UninstallResult Uninstall(const fs::path& install_dir, bool delete_saves, bool delete_screenshots);

    // ---- REL22: SkyBreak programs of this installation that still run (start program, game exe), by full path; never killed
    std::vector<RunningProgram> Running(const fs::path& install_dir) const;
    std::string RunningText(const std::vector<RunningProgram>& run) const;

    std::string tr(const char* nl, const char* en) const { return lang_ == "en" ? en : nl; }
    void SetLang(const std::string& l) { lang_ = l; }
    const std::string& Lang() const { return lang_; }

private:
    Options opt_;
    std::unique_ptr<IPlatform> plat_;
    ProcessRunner runner_;
    std::unique_ptr<IPrereqEnv> prereqs_;
    Pipeline pipeline_;
    fs::path package_dir_;
    std::string package_version_;
    Json existing_;
    fs::path existing_dir_;
    Mode mode_ = Mode::Install;
    Json state_ = Json::Object();
    std::string lang_ = "nl";
};

// UI language default: Dutch UI language -> nl, else en.
std::string DefaultLang();

// REL22: non-ASCII install/disc folders work (UTF-8 activeCodePage manifests) on Windows 10 1903+; false on older Windows and
// for the test mutants refuse-non-ascii / old-windows.
bool NonAsciiPathsWork(const std::string& mutate);

} // namespace sb
