// INSTALL19: installer engine (see engine.h).
#include "engine.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <set>

namespace sb {

std::string DefaultLang() { return "en"; }

namespace {
// Top-level entries of the install folder that Setup or the build script create (uninstall removes exactly these).
const char* kCreated[] = {"SkyBreak.exe", "skybreak.ico", "bin", "bron", "bouw", "hulp", "hulpmiddelen", "logs", "setup",
                          "instellingen.toml", "instellingen.standaard.toml", "LEESMIJ.txt", "VERSIE.txt", "Speel.cmd",
                          "textures/glyphs", "README.txt", "Play.cmd"}; // REL22: button icons made by the build script (step 8) from the player's disc

bool CopyFileAlways(const fs::path& from, const fs::path& to, std::string* err) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
        if (err) *err = Format("copy failed: %s -> %s (%lu)", U(from).c_str(), U(to).c_str(), GetLastError());
        return false;
    }
    return true;
}

std::string Forward(const fs::path& p) { return Replace(U(p), "\\", "/"); }
} // namespace

// REL22: the UTF-8 activeCodePage manifest of nwiirecomp.exe / the game exe works from Windows 10 1903 (build 18362) on.
// Test mutants: refuse-non-ascii (the 0.19 rule: always refuse), old-windows (pretend an older build).
bool NonAsciiPathsWork(const std::string& mutate) {
    if (mutate == "refuse-non-ascii" || mutate == "old-windows") return false;
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static const DWORD build = [] {
        RTL_OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof v;
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        return fn && fn(&v) == 0 ? v.dwBuildNumber : DWORD(0);
    }();
    return build >= 18362;
}

Engine::Engine(const Options& o) : opt_(o) {
    plat_ = opt_.sandbox.empty() ? MakeRealPlatform() : MakeSandboxPlatform(opt_.sandbox);
    plat_->mutate = opt_.mutate;
    runner_.SetSandbox(!opt_.sandbox.empty());
    if (!opt_.sandbox.empty()) runner_.SetAuditFile(Normal(opt_.sandbox) / L"audit-proc.txt");
    else if (!opt_.log.empty()) runner_.SetAuditFile(opt_.log.parent_path() / L"audit-proc.txt");

    package_dir_ = opt_.package_dir.empty() ? ExeDir() : Normal(opt_.package_dir);
    std::string v;
    if (ReadText(package_dir_ / L"bron" / L"VERSION", v)) package_version_ = Trim(v);
    else if (ReadText(package_dir_ / L"VERSION", v)) package_version_ = Trim(v);
    // REVINSTALL19: the installed copy (<install>\setup\SkyBreak-Setup.exe, what the Apps entry starts) has no VERSION next to
    // it; it read "0.0.0", so a repair wrote DisplayVersion/VERSIE "0.0.0". The installed sources carry it in <install>\bron.
    else if (InstalledLayout() && ReadText(package_dir_.parent_path() / L"bron" / L"VERSION", v)) package_version_ = Trim(v);
    else package_version_ = "0.0.0";

    Json manifest = LoadPrereqManifest(package_dir_ / L"bron" / L"prereqs.json");
    if (manifest.size() == 0) manifest = LoadPrereqManifest(package_dir_ / L"prereqs.json");
    if (!opt_.mock_prereqs.empty()) {
        std::string t;
        ReadText(opt_.mock_prereqs, t);
        prereqs_ = MakeMockPrereqs(Json::Parse(t), manifest);
    } else {
        prereqs_ = MakeRealPrereqs(runner_, *plat_, manifest);
    }
    lang_ = opt_.lang.empty() ? DefaultLang() : opt_.lang;
}

// ------------------------------------------------------------------------------------------------ package
bool Engine::InstalledLayout() const {
    std::error_code ec;
    return fs::is_regular_file(package_dir_ / L"install.json", ec); // running as <install>\setup\SkyBreak-Setup.exe
}

bool Engine::VerifyPackage(std::vector<std::string>* bad, std::string* err) {
    if (InstalledLayout()) return true; // repair/modify from the installed copy: the sources are already in place
    std::string text;
    if (!ReadText(package_dir_ / L"MANIFEST.sha256", text)) {
        if (err) *err = tr("MANIFEST.sha256 ontbreekt: pak de hele zip uit en start Setup vanuit die map.",
                           "MANIFEST.sha256 is missing: extract the whole zip and start Setup from that folder.");
        return false;
    }
    int n = 0, wrong = 0;
    for (auto& line : SplitLines(text)) {
        if (line.size() < 66 || line[0] == '#') continue;
        std::string want = Lower(line.substr(0, 64));
        std::string rel = line.substr(64);
        while (!rel.empty() && (rel[0] == ' ' || rel[0] == '*')) rel.erase(0, 1);
        fs::path f = package_dir_ / P(Replace(rel, "/", "\\"));
        ++n;
        std::string got = Sha256File(f);
        if (got != want) {
            ++wrong;
            if (bad) bad->push_back(rel + (got.empty() ? " (missing)" : " (changed)"));
        }
    }
    if (n == 0) {
        if (err) *err = "MANIFEST.sha256 is empty";
        return false;
    }
    if (wrong) {
        if (err) *err = Format(lang_ == "en" ? "Package incomplete: %d of %d files missing or changed. Download the zip again."
                                             : "Pakket onvolledig: %d van %d bestanden ontbreken of zijn gewijzigd. Download de zip opnieuw.",
                               wrong, n);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ existing install
fs::path Engine::ExistingDir() const { return existing_dir_; }

void Engine::DetectExisting(const fs::path& preferred) {
    existing_ = Json();
    existing_dir_.clear();
    mode_ = Mode::Install;
    std::vector<fs::path> cands;
    // INSTALL19 10 Oct: a cancelled install in a non-default folder has no Apps entry yet (written at the end), so the
    // folder the player chose must be looked at first (found by E2E E2: it resumed but reported mode 'install').
    if (!preferred.empty()) cands.push_back(preferred);
    if (!opt_.install_dir.empty()) cands.push_back(opt_.install_dir);
    Json reg;
    if (plat_->ReadUninstallEntry(reg) && !reg.str("InstallLocation").empty()) cands.push_back(P(reg.str("InstallLocation")));
    cands.push_back(DefaultInstallDir());
    for (auto& c : cands) {
        std::string t;
        if (!ReadText(StatePath(c), t)) continue;
        Json j = Json::Parse(t);
        if (!j.is_object()) continue;
        existing_ = j;
        existing_dir_ = Normal(c);
        std::string phase = j.str("phase");
        if (phase != "klaar") mode_ = Mode::Resume;
        else if (CompareVersions(package_version_, j.str("version")) > 0) mode_ = Mode::Update;
        else mode_ = Mode::Repair;
        return;
    }
}

// ------------------------------------------------------------------------------------------------ defaults + checks
fs::path Engine::DefaultInstallDir() { return plat_->KnownFolder(Folder::LocalAppData) / L"Programs" / L"SkyBreak"; }
// INSTALL19 10 Oct (user decision): the save folder stays %APPDATA%\SkywardSwordPC
// (the nand subfolder) for now: no move, no copy.
fs::path Engine::DefaultDataDir() { return plat_->KnownFolder(Folder::RoamingAppData) / L"SkywardSwordPC"; }
fs::path Engine::OldSavesDir() { return plat_->KnownFolder(Folder::RoamingAppData) / L"SkywardSwordPC" / L"nand"; }

bool Engine::OldSavesAvailable() {
    std::error_code ec;
    fs::path d = OldSavesDir();
    return fs::is_directory(d, ec) && !fs::is_empty(d, ec);
}

// INSTALL19 10 Oct: the copy/keep/fresh choice only matters when the chosen data folder is not the old save folder itself.
bool Engine::OldSavesElsewhere(const Choices& c) {
    return OldSavesAvailable() && Lower(U(Normal(c.data_dir / L"nand"))) != Lower(U(Normal(OldSavesDir())));
}

std::vector<PathIssue> Engine::CheckInstallDir(const fs::path& d) {
    std::vector<PathIssue> out;
    auto add = [&](bool error, const std::string& nl, const std::string& en) { out.push_back({error, nl, en}); };
    if (d.empty() || !d.is_absolute()) {
        add(true, "Kies een volledig pad (bijvoorbeeld C:\\SkyBreak).", "Choose a full path (for example C:\\SkyBreak).");
        return out;
    }
    std::string s = U(Normal(d));
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
        if (n && n < MAX_PATH && IsInside(d, fs::path(std::wstring(buf, n))))
            add(true, "Niet in Program Files: daarvoor zijn beheerdersrechten nodig.", "Not inside Program Files: that needs administrator rights.");
    }
    // REVINSTALL19: Setup started from the installed copy (<install>\setup) repairs/updates its own folder; that is not "inside
    // the zip folder" (before: Herstellen from Windows Settings > Apps dead-ended on this error).
    bool own_copy = opt_.mutate != "old-zip-check" && InstalledLayout() && Lower(U(Normal(package_dir_))) == Lower(U(Normal(d / L"setup")));
    if (!own_copy && (IsInside(d, package_dir_) || IsInside(package_dir_, d)))
        add(true, "Niet in (of boven) de uitgepakte zip-map: kies een eigen map.", "Not inside (or above) the extracted zip folder: choose a separate folder.");
    if (d.parent_path() == d.root_path() && d.filename().empty())
        add(true, "Kies een map, niet de hoofdmap van een schijf.", "Choose a folder, not the root of a drive.");
    // REVINSTALL19: an existing folder with someone else's content is refused. Uninstall removes what Setup created BY NAME
    // (bin, logs, setup, game, ...) and the build script empties its own <install>\game, so a typed "D:\Games" would later
    // delete the player's own D:\Games\bin or D:\Games\game. Allowed: empty, a SkyBreak install (setup\install.json, also
    // cancelled/resumed ones) and only Setup's own pre-state leftovers (setup\downloads|logs, hulpmiddelen\python|dolphin).
    if (opt_.mutate != "allow-foreign-dir" && !ForeignContentFree(d))
        add(true, "Deze map is niet leeg en is geen SkyBreak-installatie. Kies een lege of nieuwe map (bijvoorbeeld ...\\SkyBreak): "
                  "verwijderen haalt later mappen als bin, logs en game weg.",
            "This folder is not empty and is not a SkyBreak installation. Choose an empty or new folder (for example ...\\SkyBreak): "
            "uninstalling later removes folders such as bin, logs and game.");
    // REVINSTALL19 (continuation): count characters like Windows does (UTF-16 units, MAX_PATH); s.size() counted UTF-8 bytes,
    // so an accent counted 2 and a CJK character 3 ("Het pad is te lang" for a path that is not). Mutant old-space-rules: bytes.
    size_t len = opt_.mutate == "old-space-rules" ? s.size() : Normal(d).wstring().size();
    if (len > 150)
        add(true, Format("Het pad is te lang (%zu tekens, max. 150). Kies een korter pad, bijvoorbeeld C:\\SkyBreak.", len),
            Format("The path is too long (%zu characters, max. 150). Choose a shorter one, for example C:\\SkyBreak.", len));
    else if (len > 80)
        add(false, Format("Lang pad (%zu tekens). Dat werkt meestal, maar C:\\SkyBreak is veiliger voor de compiler.", len),
            Format("Long path (%zu characters). It usually works, but C:\\SkyBreak is safer for the compiler.", len));
    // REVINSTALL19 (continuation): was a warning. A real build in 'Spellen (x86), Zoë' and in '... Zoë 日' stops at step 6:
    // nWiiRecomp gets its config path from char** argv (ANSI code page) and toml++ reads that path as UTF-8 ("Config file
    // ...\recomp_config.toml not found"). The default folder contains the Windows user name, so this hits e.g. a user 'Zoë'.
    // REL22: nwiirecomp.exe and the game exe now carry a UTF-8 activeCodePage manifest (nWiiRuntime/src/platform/
    // skybreak-utf8.manifest), skybreak.rc reads UTF-8 paths and cl/ninja already run under chcp 65001, so a non-ASCII folder
    // works end to end. Windows before 10 1903 ignores that manifest: refused there only. Mutant refuse-non-ascii = the 0.19 rule.
    if (HasNonAscii(s) && !NonAsciiPathsWork(opt_.mutate))
        add(true, "Het pad bevat letters met accenten of andere bijzondere tekens. Op deze Windows-versie (ouder dan 10 1903) stopt "
                  "de bouw daarop. Kies een pad met gewone letters, bijvoorbeeld C:\\SkyBreak.",
            "The path contains accented or other special characters. On this Windows version (older than 10 1903) the build stops "
            "on them. Choose a path with plain letters, for example C:\\SkyBreak.");
    if (LooksLikeOneDrive(d))
        add(false, "Deze map wordt door OneDrive gesynchroniseerd. Dat maakt het bouwen traag en vult je OneDrive; kies liever een map buiten OneDrive.",
            "This folder is synchronised by OneDrive. That slows the build and fills your OneDrive; better choose a folder outside OneDrive.");
    // writable: probe the nearest existing parent with a temporary delete-on-close file
    fs::path probe = Normal(d);
    std::error_code ec;
    while (!probe.empty() && !fs::exists(probe, ec) && probe != probe.parent_path()) probe = probe.parent_path();
    fs::path tmp = probe / (L"skybreak-schrijftest-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        add(true, "In deze map kan Setup niet schrijven. Kies een andere map.", "Setup cannot write to this folder. Choose another folder.");
    else
        CloseHandle(h);
    return out;
}

// REVINSTALL19 (continuation): in pointer mode the build reads the game from the player's own folder; a non-ASCII character in
// that path stops build step 6 like a non-ASCII install folder does. Copying (the recommended default) avoids it.
std::vector<PathIssue> Engine::CheckDiscChoice(const Choices& c, const DiscProbe& d) {
    std::vector<PathIssue> out;
    bool pointer = d.kind == DiscKind::Folder && !c.copy_folder;
    fs::path folder = d.game_root.empty() ? d.path : d.game_root;
    if (pointer && !NonAsciiPathsWork(opt_.mutate) && HasNonAscii(U(Normal(folder))))  // REL22: only before Windows 10 1903
        out.push_back({true,
                       "Het pad van deze map bevat letters met accenten of andere bijzondere tekens. Verwijzen werkt dan niet (de bouw "
                       "stopt bij stap 6): kies \"Kopiëren naar de SkyBreak-map\".",
                       "This folder's path contains accented or other special characters. Pointing to it does not work then (the build "
                       "stops at step 6): choose \"Copy into the SkyBreak folder\"."});
    return out;
}

bool Engine::ForeignContentFree(const fs::path& d) const {
    std::error_code ec;
    if (!fs::is_directory(d, ec) || fs::is_empty(d, ec)) return true;
    if (fs::is_regular_file(StatePath(d), ec)) return true;
    // prerequisites are downloaded before install.json exists (step 4): a retry after such a stop must stay possible
    auto only = [&](const fs::path& dir, std::initializer_list<const wchar_t*> names) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::wstring n = it->path().filename().wstring();
            std::transform(n.begin(), n.end(), n.begin(), ::towlower);
            if (std::none_of(names.begin(), names.end(), [&](const wchar_t* x) { return n == x; })) return false;
        }
        return !ec;
    };
    return only(d, {L"setup", L"hulpmiddelen"}) && (!fs::is_directory(d / L"setup", ec) || only(d / L"setup", {L"downloads", L"logs"})) &&
           (!fs::is_directory(d / L"hulpmiddelen", ec) || only(d / L"hulpmiddelen", {L"python", L"dolphin"}));
}

// REVINSTALL19 (continuation): Space() runs every frame on the folders page; measure a leftover folder at most every 2 s per path.
static uint64_t LeftoverBytes(const fs::path& p) {
    static std::wstring last;
    static uint64_t size = 0;
    static ULONGLONG at = 0;
    ULONGLONG now = GetTickCount64();
    if (p.native() == last && now - at < 2000) return size;
    std::error_code ec;
    size = fs::is_directory(p, ec) ? DirSize(p) : 0;
    last = p.native();
    at = now;
    return size;
}

std::vector<SpaceRow> Engine::Space(const Choices& c, const DiscProbe& d, const PrereqReport& r) {
    const uint64_t GB = 1024ull * 1024 * 1024;
    uint64_t need = uint64_t(0.15 * GB) + 2 * GB + uint64_t(0.25 * GB); // sources + build peak + exe/pdb
    bool copy = d.kind != DiscKind::Folder || c.copy_folder;
    // REVINSTALL19: an update/repair/resume whose game copy is already there needs no second 3.8 GB (an update on a nearly
    // full drive was refused before). Note: work/pc-game measures 3.9 GiB; the 2 GiB build headroom covers the difference.
    std::error_code ec;
    bool game_there = fs::is_regular_file(c.install_dir / L"game" / L"sys" / L"main.dol", ec);
    if (copy && !game_there) need += uint64_t(3.8 * GB);
    // REVINSTALL19 (continuation): a cancelled step 3 leaves <install>\game.uitpakken (up to 3.9 GB). Bouw-SkywardSword.ps1 deletes
    // it before it extracts again, so a resume counts it as free space (before: a resume on a nearly full drive was refused although
    // the drive had room once the leftover was gone). At most the 3.8 GB of the copy itself. Mutant old-space-rules: not counted.
    if (copy && !game_there && opt_.mutate != "old-space-rules")
        need -= std::min<uint64_t>(LeftoverBytes(c.install_dir / L"game.uitpakken"), uint64_t(3.8 * GB));
    // REVINSTALL19: --mock-free-gb (tests) replaces the measured free space, only in a sandbox run
    auto free_of = [&](const fs::path& p) {
        return opt_.mock_free_gb >= 0 && plat_->IsSandbox() ? uint64_t(opt_.mock_free_gb * double(GB)) : FreeBytes(p);
    };
    std::vector<SpaceRow> rows;
    std::string idrive = U(Normal(c.install_dir).root_name());
    rows.push_back({idrive, need, free_of(c.install_dir)});
    const PrereqItem* vs = r.get("vs");
    if (vs && !vs->present) {
        wchar_t sys[MAX_PATH];
        GetSystemWindowsDirectoryW(sys, MAX_PATH);
        std::string sdrive = U(fs::path(sys).root_name());
        if (Lower(sdrive) == Lower(idrive)) rows[0].need += 7 * GB;
        else rows.push_back({sdrive, 7 * GB, free_of(fs::path(sys))});
    }
    return rows;
}

std::string Engine::NandRootToml(const Choices& c) {
    if (c.old_saves == "use_old" && OldSavesElsewhere(c)) return Forward(OldSavesDir());
    fs::path nand = c.data_dir / L"nand";
    if (!plat_->IsSandbox() && Lower(U(Normal(c.data_dir))) == Lower(U(Normal(DefaultDataDir()))))
        return "%APPDATA%/SkywardSwordPC/nand"; // INSTALL19 10 Oct: = the script's own default (user decision: no move)
    return Forward(Normal(nand));
}

std::string Engine::TimeEstimate(const PrereqReport& r, const Choices& c, const DiscProbe& d, bool en) {
    int jobs = c.jobs > 0 ? c.jobs : SuggestedJobs(r.sys);
    bool pointer = d.kind == DiscKind::Folder && !c.copy_folder;
    double total = 0;
    for (auto& s : PlanSteps(jobs, pointer, d.kind != DiscKind::Folder, false)) total += s.expected_s;
    int lo = std::max(1, int(total * 0.75 / 60.0 + 0.5)), hi = std::max(lo + 1, int(total * 1.5 / 60.0 + 0.5));
    return en ? Format("about %d-%d minutes on this PC (%d jobs)", lo, hi, jobs)
              : Format("ca. %d-%d minuten op deze pc (%d taken)", lo, hi, jobs);
}

PrereqReport Engine::Detect(const Choices& c, const DiscProbe& d) {
    DetectContext ctx;
    ctx.install_dir = c.install_dir;
    ctx.need_dolphintool = d.kind != DiscKind::Folder;
    return prereqs_->Detect(ctx);
}

// ------------------------------------------------------------------------------------------------ sources
bool Engine::SyncSources(const fs::path& install_dir, std::string* err, int* copied, int* unchanged, int* removed) {
    int c = 0, u = 0, r = 0;
    fs::path src = package_dir_ / L"bron", dst = install_dir / L"bron";
    std::error_code ec;
    if (InstalledLayout()) return true; // running from the installed copy: nothing to sync
    if (!fs::is_directory(src, ec)) {
        if (err) *err = tr("De map bron\\ ontbreekt in het pakket.", "The bron\\ folder is missing from the package.");
        return false;
    }
    std::set<std::wstring> seen;
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        fs::path rel = fs::relative(it->path(), src, ec);
        fs::path to = dst / rel;
        std::wstring key = rel.wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        seen.insert(key);
        std::error_code e2;
        if (fs::is_regular_file(to, e2) && fs::file_size(to, e2) == it->file_size(e2) && Sha256File(to) == Sha256File(it->path())) {
            ++u;
            continue;
        }
        if (!CopyFileAlways(it->path(), to, err)) return false;
        ++c;
    }
    if (ec) {
        if (err) *err = "Could not read source folder: " + ec.message();
        return false;
    }
    // remove files that the new package no longer has (stale sources)
    std::vector<fs::path> stale;
    for (fs::recursive_directory_iterator it(dst, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::wstring key = fs::relative(it->path(), dst, ec).wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        if (!seen.count(key)) stale.push_back(it->path());
    }
    for (auto& s : stale) { std::error_code e2; if (fs::remove(s, e2)) ++r; }
    if (copied) *copied = c;
    if (unchanged) *unchanged = u;
    if (removed) *removed = r;
    Log("[sync] bron: %d copied, %d unchanged, %d removed", c, u, r);
    return true;
}

bool Engine::MigrateOldSaves(const Choices& c, std::string* msg) {
    if (c.old_saves != "copy" || !OldSavesElsewhere(c)) return true; // INSTALL19 10 Oct: same folder -> nothing to copy
    fs::path to = c.data_dir / L"nand";
    std::error_code ec;
    if (fs::is_directory(to, ec) && !fs::is_empty(to, ec)) {
        if (msg) *msg = tr("De nieuwe datamap is niet leeg; oude saves niet gekopieerd.", "The new data folder is not empty; old saves not copied.");
        return true;
    }
    fs::create_directories(to, ec);
    fs::copy(OldSavesDir(), to, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec) {
        if (msg) *msg = "saves copy failed: " + ec.message();
        return false;
    }
    if (msg) *msg = tr("Oude saves gekopieerd; de oude map blijft als reservekopie staan.", "Old saves copied; the old folder stays as a backup.");
    plat_->Audit("copy-saves", U(OldSavesDir()) + " -> " + U(to));
    return true;
}

// ------------------------------------------------------------------------------------------------ build
BuildOptions Engine::MakeBuildOptions(const Choices& c, const DiscProbe& d, const PrereqReport& r) {
    BuildOptions b;
    b.script = !opt_.build_script.empty() ? opt_.build_script
                                          : c.install_dir / L"bron" / L"outputs" / L"release" / L"Bouw-SkywardSword.ps1";
    b.disc = d.kind == DiscKind::Folder ? d.game_root : d.path;
    b.install_dir = c.install_dir;
    b.work_dir = c.install_dir / L"bouw";
    b.pointer_mode = d.kind == DiscKind::Folder && !c.copy_folder;
    if (!b.pointer_mode) b.game_dir = c.install_dir / L"game";
    b.nand_root = NandRootToml(c);
    b.product = "SkyBreak " + package_version_;
    b.exe_name = kGameExeName;
    b.jobs = c.jobs > 0 ? c.jobs : SuggestedJobs(r.sys);
    b.python = r.python;
    b.cmake = r.cmake;
    b.ninja = r.ninja;
    if (d.kind != DiscKind::Folder) b.dolphintool = r.dolphintool;
    b.bronnen = c.install_dir / L"bron" / L"work" / L"nwii-build" / L"_deps";
    b.progress_json = c.install_dir / L"setup" / L"voortgang.jsonl";
    b.log = c.install_dir / L"setup" / L"logs" / (L"bouw-" + W(NowStamp()) + L".log");
    b.keep_build = c.keep_build;
    return b;
}

bool Engine::StartBuild(const Choices& c, const DiscProbe& d, const PrereqReport& r, std::string* err) {
    BuildOptions b = MakeBuildOptions(c, d, r);
    std::error_code ec;
    bool incremental = fs::exists(b.work_dir / L"build" / L"build.ninja", ec);
    state_.set("phase", "bouwen");
    state_.set("build_log", U(b.log));
    state_.set("jobs", b.jobs);
    state_.set("incremental", incremental);
    SaveState(c.install_dir);
    return pipeline_.Start(runner_, b, incremental, err);
}

// ------------------------------------------------------------------------------------------------ state
void Engine::SaveState(const fs::path& install_dir) {
    state_.set("product", kProduct);
    state_.set("version", package_version_);
    state_.set("updated", NowIso());
    WriteText(StatePath(install_dir), state_.Dump(2) + "\n");
}

void Engine::SetPhase(const fs::path& install_dir, const std::string& phase) {
    state_.set("phase", phase);
    const PipelineState& ps = pipeline_.State();
    if (ps.step) {
        state_.set("step", ps.step);
        state_.set("a", ps.a);
        state_.set("b", ps.b);
    }
    uint64_t peak = runner_.PeakJobMemory();
    if (peak) state_.set("peak_job_memory_mb", double(peak / (1024 * 1024)));
    SaveState(install_dir);
}

// ------------------------------------------------------------------------------------------------ finish
bool Engine::Finish(const Choices& c, const DiscProbe& d, std::string* err) {
    const fs::path& inst = c.install_dir;
    std::error_code ec;
    // REL22: SkyBreak.exe of this installation is replaced below; a running copy would make that fail halfway. The callers ask
    // the player to close it first (GUI: retry/cancel, headless: exit 7); this is the last line of defence. Nothing is killed.
    if (auto run = Running(inst); !run.empty()) {
        if (err) *err = RunningText(run);
        return false;
    }
    // start program + icon (our code, from the package)
    fs::path stub = package_dir_ / L"app" / L"SkyBreak.exe", ico = package_dir_ / L"app" / L"skybreak.ico";
    if (fs::is_regular_file(stub, ec) || !fs::is_regular_file(inst / L"SkyBreak.exe", ec)) {
        if (!CopyFileAlways(stub, inst / L"SkyBreak.exe", err)) return false;
        if (!CopyFileAlways(ico, inst / L"skybreak.ico", err)) return false;
    }
    // own copy of Setup for uninstall/modify/repair
    fs::path self = ExePath(), setup = inst / L"setup" / L"SkyBreak-Setup.exe";
    if (Normal(self) != Normal(setup)) CopyFileAlways(self, setup, nullptr);
    std::string vtxt;
    if (ReadText(package_dir_ / L"bron" / L"prereqs.json", vtxt)) WriteText(inst / L"setup" / L"prereqs.json", vtxt);

    // shortcuts (remove ones that were switched off)
    Json lnks = Json::Array();
    std::vector<std::pair<bool, fs::path>> want = {
        {c.startmenu, plat_->KnownFolder(Folder::StartMenuPrograms) / L"SkyBreak.lnk"},
        {c.desktop, plat_->KnownFolder(Folder::Desktop) / L"SkyBreak.lnk"},
    };
    for (auto& [on, lnk] : want) {
        if (on) {
            std::string e;
            if (!plat_->CreateShortcut(lnk, inst / L"SkyBreak.exe", inst, inst / L"skybreak.ico",
                                       tr("SkyBreak - pc-port van The Legend of Zelda: Skyward Sword, gebouwd van je eigen schijf",
                                          "SkyBreak - PC port of The Legend of Zelda: Skyward Sword, built from your own disc"),
                                       &e)) {
                if (err) *err = e;
                return false;
            }
            lnks.push(Json(U(lnk)));
        } else if (fs::exists(lnk, ec)) {
            plat_->Audit("shortcut-delete", U(lnk));
            fs::remove(lnk, ec);
        }
    }

    // Apps entry (HKCU, no admin)
    uint64_t size_kb = DirSize(inst) / 1024;
    Json reg = Json::Object();
    reg.set("DisplayName", "SkyBreak");
    reg.set("DisplayVersion", package_version_);
    reg.set("DisplayIcon", U(inst / L"skybreak.ico"));
    reg.set("Publisher", "SkyBreak (fanproject)");
    reg.set("InstallLocation", U(inst));
    reg.set("EstimatedSize", Json(double(std::min<uint64_t>(size_kb, 0xFFFFFFFFull))));
    reg.set("UninstallString", "\"" + U(setup) + "\" --uninstall");
    reg.set("ModifyPath", "\"" + U(setup) + "\" --modify");
    reg.set("NoRepair", 0);
    reg.set("URLInfoAbout", "");
    reg.set("Comments", "PC-port van The Legend of Zelda: Skyward Sword, gebouwd van je eigen schijf. Geen officieel Nintendo-product.");
    if (!plat_->WriteUninstallEntry(reg)) {
        if (err) *err = tr("De ingang in Windows-instellingen > Apps kon niet worden geschreven.", "The entry in Windows Settings > Apps could not be written.");
        return false;
    }

    // install.json
    Json created = Json::Array();
    for (auto* n : kCreated) created.push(Json(n));
    bool pointer = d.kind == DiscKind::Folder && !c.copy_folder;
    if (!pointer) created.push(Json("game"));
    state_.set("install_dir", U(inst));
    state_.set("data_dir", U(c.data_dir));
    state_.set("nand_root", NandRootToml(c));
    state_.set("nand_dir", c.old_saves == "use_old" && OldSavesElsewhere(c) ? U(OldSavesDir()) : U(c.data_dir / L"nand"));
    state_.set("disc", U(d.path));
    state_.set("disc_kind", DiscKindName(d.kind));
    state_.set("pointer_mode", pointer);
    state_.set("game_dir", pointer ? U(d.game_root) : U(inst / L"game"));
    state_.set("keep_build", c.keep_build);
    state_.set("startmenu", c.startmenu);
    state_.set("desktop", c.desktop);
    state_.set("lang", lang_);
    state_.set("created", created);
    state_.set("shortcuts", lnks);
    state_.set("sandbox", plat_->IsSandbox());
    state_.set("installed", NowIso());
    SetPhase(inst, "klaar");
    return true;
}

// ------------------------------------------------------------------------------------------------ running programs (REL22)
std::vector<RunningProgram> Engine::Running(const fs::path& install_dir) const {
    if (install_dir.empty() || opt_.mutate == "ignore-running") return {}; // mutant ignore-running = the 0.19 behaviour (tests)
    return RunningFromInstall(Normal(install_dir));
}

std::string Engine::RunningText(const std::vector<RunningProgram>& run) const {
    std::string s = tr("SkyBreak draait nog. Sluit SkyBreak (het spel en de launcher) en kies dan Opnieuw proberen. Setup sluit het "
                       "niet zelf af, zodat je niets kwijtraakt. Nog open:",
                       "SkyBreak is still running. Close SkyBreak (the game and the launcher), then choose Retry. Setup does not close "
                       "it for you, so nothing is lost. Still open:");
    for (auto& r : run) s += "\n  " + U(r.image) + Format(" (pid %u)", unsigned(r.pid));
    return s;
}

// ------------------------------------------------------------------------------------------------ uninstall
Engine::UninstallResult Engine::Uninstall(const fs::path& install_dir, bool delete_saves, bool delete_screenshots) {
    UninstallResult res;
    fs::path inst = Normal(install_dir);
    std::string t;
    if (!ReadText(StatePath(inst), t)) {
        res.errors.push_back(tr("Geen SkyBreak-installatie gevonden in ", "No SkyBreak installation found in ") + U(inst));
        return res;
    }
    Json st = Json::Parse(t);
    std::error_code ec;
    // REL22: never start removing while SkyBreak (start program or game exe) of THIS installation runs: its files are locked
    // and the result was a half-deleted folder. Nothing is removed; the caller asks the player to close it (retry/cancel).
    if (auto run = Running(inst); !run.empty()) {
        res.running = true;
        res.errors.push_back(RunningText(run));
        return res;
    }
    for (auto& l : st["shortcuts"].strings()) {
        fs::path lnk = P(l);
        if (Lower(U(lnk.extension())) != ".lnk") continue;
        plat_->Audit("shortcut-delete", l);
        if (fs::remove(lnk, ec)) res.removed.push_back(l);
    }
    Json reg;
    if (plat_->ReadUninstallEntry(reg) && Lower(U(Normal(P(reg.str("InstallLocation"))))) == Lower(U(inst)))
        plat_->DeleteUninstallEntry();

    fs::path self = Normal(ExePath());
    std::vector<std::string> names = st["created"].strings();
    if (delete_screenshots) names.push_back("screenshots");
    for (auto& n : names) {
        fs::path p = inst / P(n);
        if (!IsInside(p, inst) || Normal(p) == inst) continue;
        if (n == "game" && st.boolean("pointer_mode", false)) continue; // never the player's own folder
        if (!fs::exists(p, ec)) continue;
        if (IsInside(self, p)) {
            // our own running copy: remove everything else in that folder, the exe itself is deleted after exit
            for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
                if (Normal(it->path()) != self) { std::error_code e2; fs::remove_all(it->path(), e2); }
            res.removed.push_back(n + " (behalve Setup zelf)");
            continue;
        }
        fs::remove_all(p, ec);
        if (ec) res.errors.push_back(n + ": " + ec.message());
        else res.removed.push_back(n);
        ec.clear();
    }
    if (delete_saves) {
        fs::path nand = P(st.str("nand_dir"));
        // only the recorded nand folder (and its SkyBreak parent when empty); never a drive root or the install dir
        if (!nand.empty() && nand.is_absolute() && std::distance(nand.begin(), nand.end()) >= 3 && !IsInside(inst, nand)) {
            fs::remove_all(nand, ec);
            res.saves_removed = !ec;
            if (ec) res.errors.push_back("saves: " + ec.message());
            fs::path parent = nand.parent_path();
            if ((Lower(U(parent.filename())) == "skybreak" || Lower(U(parent.filename())) == "skywardswordpc") && fs::is_empty(parent, ec)) fs::remove(parent, ec); // INSTALL19 10 Oct
        }
    }
    if (fs::is_empty(inst / L"textures", ec)) fs::remove(inst / L"textures", ec); // REL22: only textures\glyphs was ours
    ec.clear();
    for (fs::directory_iterator it(inst, ec), end; !ec && it != end; it.increment(ec)) {
        if (IsInside(self, it->path())) continue;
        res.kept_unknown.push_back(U(it->path().filename()));
    }
    if (res.kept_unknown.empty() && !IsInside(self, inst)) fs::remove(inst, ec);
    res.ok = res.errors.empty();
    Log("[uninstall] removed %zu, kept %zu unknown, errors %zu, saves_removed %d", res.removed.size(), res.kept_unknown.size(),
        res.errors.size(), int(res.saves_removed));
    return res;
}

} // namespace sb
