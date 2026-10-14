// INSTALL19: prerequisite detection (read-only) and consent-gated installation (see prereqs.h).
#include "prereqs.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <thread>

#include "platform.h"
#include "proc.h"

namespace sb {

const PrereqItem* PrereqReport::get(const std::string& id) const {
    for (auto& i : items) if (i.id == id) return &i;
    return nullptr;
}
PrereqItem* PrereqReport::get(const std::string& id) {
    for (auto& i : items) if (i.id == id) return &i;
    return nullptr;
}
std::vector<std::string> PrereqReport::MissingRequired() const {
    std::vector<std::string> r;
    for (auto& i : items) if (i.required && !i.present) r.push_back(i.id);
    return r;
}

int SuggestedJobs(const SystemInfo& s) {
    int by_cores = std::max(1, s.cores - 2);
    int by_ram = s.ram_gb > 0 ? std::max(1, int(std::floor(s.ram_gb / 1.5))) : by_cores;
    return std::max(1, std::min(by_cores, by_ram));
}

Json LoadPrereqManifest(const fs::path& file) {
    std::string t;
    if (!ReadText(file, t)) return Json::Array();
    Json j = Json::Parse(t);
    if (j.is_object() && j["items"].is_array()) return j["items"];
    return j.is_array() ? j : Json::Array();
}

// INSTALL19: the part of the DolphinTool route after the download, separate so the unit test can feed it the
// player's own archive. Nothing is extracted unless the SHA-256 matches the pinned value.
bool ExtractPinnedArchive(ProcessRunner& run, const fs::path& tar, const fs::path& arc, const std::string& want_sha256,
                          const std::vector<std::string>& entries, const fs::path& tmp, const fs::path& dir, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (want_sha256.empty()) return fail("geen vastgepinde SHA-256; niets uitgepakt");
    if (Sha256File(arc) != want_sha256) return fail("SHA-256 klopt niet; niets uitgepakt");
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    std::vector<std::wstring> args = {L"-xf", arc.wstring(), L"-C", tmp.wstring()};
    for (auto& e : entries) args.push_back(W(e));
    ProcResult r = run.Run(tar, args, 300000);
    bool ok = r.started && r.code == 0;
    if (ok) {
        fs::create_directories(dir, ec);
        for (auto& e : entries) {
            fs::path src = tmp / P(e);
            if (!e.empty() && (e.back() == '/' || e.back() == '\\')) src = src.parent_path(); // "LICENSES/" = a folder
            if (fs::is_directory(src, ec))
                fs::copy(src, dir / src.filename(), fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
            else
                fs::copy_file(src, dir / src.filename(), fs::copy_options::overwrite_existing, ec);
        }
        ok = fs::is_regular_file(dir / L"DolphinTool.exe", ec);
    }
    fs::remove_all(tmp, ec);
    return ok ? true : fail("uitpakken mislukt");
}

namespace {

const char* kVsComponents[] = {"Microsoft.VisualStudio.Workload.VCTools", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                               "Microsoft.VisualStudio.Component.Windows11SDK.22621",
                               "Microsoft.VisualStudio.Component.VC.CMake.Project"};

const Json& ManifestItem(const Json& manifest, const std::string& id) {
    for (auto& e : manifest.elements()) if (e.str("id") == id) return e;
    static const Json none;
    return none;
}

ConsentInfo ConsentFrom(const Json& m, const std::string& id) {
    ConsentInfo c;
    const Json& e = ManifestItem(m, id);
    c.what = e.str("name", id);
    c.publisher = e.str("publisher");
    c.url = e.str("url");
    double sz = e.num("size_bytes", 0);
    c.size = sz > 0 ? Format("%.0f MB", sz / 1048576.0) : e.str("size", "");
    c.license = e.str("license");
    c.license_url = e.str("license_url");
    c.changes_nl = e.str("changes_nl");
    c.changes_en = e.str("changes_en");
    c.admin = e.boolean("admin", false);
    return c;
}

SystemInfo ReadSystemInfo() {
    SystemInfo s;
    s.cores = int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) s.ram_gb = double(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
        RTL_OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof v;
        if (fn(&v) == 0) s.windows = Format("Windows %s (build %lu)", v.dwBuildNumber >= 22000 ? "11" : "10", v.dwBuildNumber);
    }
    DWORD val = 0, len = sizeof val;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy", L"VerifiedAndReputablePolicyState",
                     RRF_RT_REG_DWORD, nullptr, &val, &len) == ERROR_SUCCESS)
        s.smart_app_control = int(val);
    len = sizeof val;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"LongPathsEnabled", RRF_RT_REG_DWORD,
                     nullptr, &val, &len) == ERROR_SUCCESS)
        s.long_paths = int(val);
    return s;
}

class RealPrereqs final : public IPrereqEnv {
public:
    RealPrereqs(ProcessRunner& r, IPlatform& p, Json m) : run_(r), plat_(p), manifest_(std::move(m)) {}
    bool IsMock() const override { return false; }
    ConsentInfo Consent(const std::string& id) const override { return ConsentFrom(manifest_, id); }

    PrereqReport Detect(const DetectContext& ctx) override {
        PrereqReport rep;
        rep.sys = ReadSystemInfo();
        fs::path winget = ProcessRunner::FindOnPath(L"winget.exe", true);
        rep.sys.winget = !winget.empty();
        fs::path tar = SystemTar();
        if (!tar.empty()) {
            ProcResult r = run_.Run(tar, {L"--version"}, 10000);
            rep.sys.tar_lzma = r.started && r.out.find("liblzma") != std::string::npos;
        }
        DetectVs(rep);
        DetectPython(rep, ctx);
        DetectCMakeNinja(rep);
        DetectDolphin(rep, ctx);
        return rep;
    }

    InstallOutcome Install(const std::string& id, const DetectContext& ctx, const ProgressFn& progress,
                           std::atomic<bool>* cancel) override {
        InstallOutcome o;
        if (plat_.IsSandbox() || run_.Sandbox()) {
            o.msg_nl = "Zandbakstand: er wordt niets geinstalleerd.";
            o.msg_en = "Sandbox mode: nothing is installed.";
            o.route = "refused";
            return o;
        }
        if (id == "vs") return InstallVs(ctx, progress, cancel);
        if (id == "python") return InstallPython(ctx, progress, cancel);
        if (id == "dolphintool") return InstallDolphin(ctx, progress, cancel);
        o.msg_nl = o.msg_en = "unknown item " + id;
        return o;
    }

private:
    static fs::path SystemTar() {
        wchar_t sys[MAX_PATH];
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        fs::path t = fs::path(std::wstring(sys, n)) / L"tar.exe";
        std::error_code ec;
        return fs::is_regular_file(t, ec) ? t : fs::path();
    }
    static fs::path Vswhere() {
        wchar_t pf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH);
        if (!n || n >= MAX_PATH) return {};
        fs::path p = fs::path(std::wstring(pf, n)) / L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";
        std::error_code ec;
        return fs::is_regular_file(p, ec) ? p : fs::path();
    }

    void DetectVs(PrereqReport& rep) {
        PrereqItem it;
        it.id = "vs";
        it.name = "Visual Studio 2022 Build Tools (C++ x64, Windows SDK, CMake)";
        fs::path vw = Vswhere();
        if (!vw.empty()) {
            ProcResult r = run_.Run(vw, {L"-latest", L"-products", L"*", L"-version", L"[17.0,18.0)", L"-requires",
                                         L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64", L"-property", L"installationPath"});
            std::string path = r.started ? Trim(r.out) : "";
            if (!path.empty()) path = SplitLines(path).front();
            bool other = false;
            if (path.empty()) { // VS 2026+ with C++: the build script accepts it too (untested with MSVC 14.5x)
                r = run_.Run(vw, {L"-latest", L"-products", L"*", L"-requires", L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                  L"-property", L"installationPath"});
                path = r.started ? Trim(r.out) : "";
                if (!path.empty()) { path = SplitLines(path).front(); other = true; }
            }
            if (!path.empty()) {
                it.present = true;
                it.path = path;
                rep.vs = P(path);
                ProcResult v = run_.Run(vw, {L"-path", W(path), L"-property", L"catalog_productDisplayVersion"});
                it.version = v.started ? Trim(v.out) : "";
                if (other) {
                    it.note_nl = "Geen Visual Studio 2022 gevonden; een nieuwere versie met C++ wordt gebruikt (niet getest).";
                    it.note_en = "No Visual Studio 2022 found; a newer version with C++ is used (untested).";
                }
            }
        }
        if (!it.present) {
            it.note_nl = vw.empty() ? "Visual Studio is niet gevonden." : "Visual Studio gevonden, maar zonder de C++-compiler (MSVC x64).";
            it.note_en = vw.empty() ? "Visual Studio was not found." : "Visual Studio found, but without the C++ compiler (MSVC x64).";
        } else if (!HasWindowsSdk()) {
            it.present = false;
            it.note_nl = "Visual Studio gevonden, maar de Windows SDK ontbreekt.";
            it.note_en = "Visual Studio found, but the Windows SDK is missing.";
        }
        rep.items.push_back(it);
    }

    static bool HasWindowsSdk() {
        wchar_t buf[MAX_PATH] = {};
        DWORD len = sizeof buf;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots", L"KitsRoot10", RRF_RT_REG_SZ |
                         RRF_SUBKEY_WOW6432KEY, nullptr, buf, &len) != ERROR_SUCCESS) {
            len = sizeof buf;
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots", L"KitsRoot10", RRF_RT_REG_SZ,
                             nullptr, buf, &len) != ERROR_SUCCESS)
                return false;
        }
        std::error_code ec;
        return fs::is_directory(fs::path(buf) / L"Include", ec);
    }

    bool PythonOk(const fs::path& exe, std::string& version) {
        ProcResult r = run_.Run(exe, {L"-c", L"import sys;print('%d.%d.%d' % sys.version_info[:3]);sys.exit(0 if sys.version_info >= (3,8) else 1)"}, 20000);
        version = r.started ? Trim(r.out) : "";
        return r.started && r.code == 0;
    }

    void DetectPython(PrereqReport& rep, const DetectContext& ctx) {
        PrereqItem it;
        it.id = "python";
        it.name = "Python 3.8+";
        std::vector<fs::path> cands;
        if (!ctx.install_dir.empty()) cands.push_back(ctx.install_dir / L"hulpmiddelen" / L"python" / L"python.exe");
        fs::path onpath = ProcessRunner::FindOnPath(L"python.exe");
        if (!onpath.empty()) cands.push_back(onpath);
        fs::path py = ProcessRunner::FindOnPath(L"py.exe");
        if (!py.empty()) {
            ProcResult r = run_.Run(py, {L"-3", L"-c", L"import sys;print(sys.executable)"}, 20000);
            if (r.started && r.code == 0 && !Trim(r.out).empty()) cands.push_back(P(Trim(r.out)));
        }
        std::error_code ec;
        for (auto& c : cands) {
            if (!fs::is_regular_file(c, ec)) continue;
            std::string v;
            if (PythonOk(c, v)) {
                it.present = true;
                it.path = U(c);
                it.version = v;
                rep.python = c;
                break;
            }
            it.note_nl = "Python " + v + " is te oud (" + U(c) + "); nodig: 3.8 of nieuwer.";
            it.note_en = "Python " + v + " is too old (" + U(c) + "); needed: 3.8 or newer.";
        }
        if (!it.present && it.note_nl.empty()) {
            it.note_nl = "Python 3 is niet gevonden.";
            it.note_en = "Python 3 was not found.";
        }
        rep.items.push_back(it);
    }

    void DetectCMakeNinja(PrereqReport& rep) {
        std::error_code ec;
        std::vector<fs::path> cm, nj;
        if (!rep.vs.empty()) {
            fs::path base = rep.vs / L"Common7" / L"IDE" / L"CommonExtensions" / L"Microsoft" / L"CMake";
            cm.push_back(base / L"CMake" / L"bin" / L"cmake.exe");
            nj.push_back(base / L"Ninja" / L"ninja.exe");
        }
        cm.push_back(ProcessRunner::FindOnPath(L"cmake.exe"));
        nj.push_back(ProcessRunner::FindOnPath(L"ninja.exe"));
        if (!rep.python.empty()) {
            cm.push_back(rep.python.parent_path() / L"Scripts" / L"cmake.exe");
            nj.push_back(rep.python.parent_path() / L"Scripts" / L"ninja.exe");
        }
        for (auto& c : cm) if (!c.empty() && fs::is_regular_file(c, ec)) { rep.cmake = c; break; }
        for (auto& c : nj) if (!c.empty() && fs::is_regular_file(c, ec)) { rep.ninja = c; break; }
        PrereqItem* vs = rep.get("vs");
        if (vs && vs->present && (rep.cmake.empty() || rep.ninja.empty())) {
            vs->present = false;
            vs->note_nl = "Visual Studio gevonden, maar zonder 'C++ CMake tools' (CMake en Ninja).";
            vs->note_en = "Visual Studio found, but without 'C++ CMake tools' (CMake and Ninja).";
        }
    }

    void DetectDolphin(PrereqReport& rep, const DetectContext& ctx) {
        PrereqItem it;
        it.id = "dolphintool";
        it.name = "DolphinTool (Dolphin-emulator)";
        it.required = ctx.need_dolphintool;
        std::vector<fs::path> cands;
        wchar_t env[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"DOLPHINTOOL", env, DWORD(std::size(env)));
        if (n && n < std::size(env)) cands.push_back(fs::path(std::wstring(env, n)));
        if (!ctx.install_dir.empty()) cands.push_back(ctx.install_dir / L"hulpmiddelen" / L"dolphin" / L"DolphinTool.exe");
        std::error_code ec;
        for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
            n = GetEnvironmentVariableW(var, env, DWORD(std::size(env)));
            if (!n || n >= std::size(env)) continue;
            fs::path base(std::wstring(env, n));
            for (fs::directory_iterator d(base, ec), e; !ec && d != e; d.increment(ec)) {
                if (!d->is_directory(ec) || Lower(U(d->path().filename())).rfind("dolphin", 0) != 0) continue;
                for (fs::recursive_directory_iterator f(d->path(), fs::directory_options::skip_permission_denied, ec), fe;
                     !ec && f != fe; f.increment(ec)) {
                    if (f.depth() > 2) { f.disable_recursion_pending(); continue; }
                    if (Lower(U(f->path().filename())) == "dolphintool.exe") cands.push_back(f->path());
                }
                ec.clear();
            }
            ec.clear();
        }
        fs::path onpath = ProcessRunner::FindOnPath(L"DolphinTool.exe");
        if (!onpath.empty()) cands.push_back(onpath);
        fs::path dl = RealKnownFolder(Folder::UserProfile) / L"Downloads";
        if (fs::is_directory(dl, ec)) {
            for (fs::recursive_directory_iterator f(dl, fs::directory_options::skip_permission_denied, ec), fe; !ec && f != fe;
                 f.increment(ec)) {
                if (f.depth() > 3) { f.disable_recursion_pending(); continue; }
                if (Lower(U(f->path().filename())) == "dolphintool.exe") cands.push_back(f->path());
            }
        }
        for (auto& c : cands) {
            if (!fs::is_regular_file(c, ec)) continue;
            ProcResult r = run_.Run(c, {L"header", L"--help"}, 15000);
            if (r.started && uint32_t(r.code) == 0xC0000135u) {
                it.note_nl = "DolphinTool gevonden (" + U(c) + "), maar de Visual C++-runtime ontbreekt (komt mee met Visual Studio).";
                it.note_en = "DolphinTool found (" + U(c) + "), but the Visual C++ runtime is missing (comes with Visual Studio).";
                continue;
            }
            if (r.started) {
                it.present = true;
                it.path = U(c);
                rep.dolphintool = c;
                break;
            }
        }
        if (!it.present && it.note_nl.empty()) {
            it.note_nl = ctx.need_dolphintool ? "DolphinTool is niet gevonden (nodig om een schijfbestand uit te pakken)."
                                              : "Niet nodig: je gebruikt een uitgepakte map.";
            it.note_en = ctx.need_dolphintool ? "DolphinTool was not found (needed to extract a disc image)."
                                              : "Not needed: you use an extracted folder.";
        }
        rep.items.push_back(it);
    }

    // ---------------------------------------------------------------------------------- installs (consent given)
    InstallOutcome InstallVs(const DetectContext& ctx, const ProgressFn& progress, std::atomic<bool>* cancel) {
        InstallOutcome o;
        std::wstring adds;
        for (auto* c : kVsComponents) { adds += L" --add "; adds += W(c); }
        std::wstring override_ = L"--wait --passive --norestart" + adds + L" --includeRecommended";
        fs::path winget = ProcessRunner::FindOnPath(L"winget.exe", true);
        ProcResult r;
        if (!winget.empty()) {
            o.route = "winget";
            if (progress) progress(0.05, "winget: Visual Studio Build Tools");
            r = run_.Run(winget, {L"install", L"--id", L"Microsoft.VisualStudio.2022.BuildTools", L"-e", L"--source", L"winget",
                                  L"--accept-package-agreements", L"--accept-source-agreements", L"--override", override_},
                         4u * 3600u * 1000u);
        } else {
            o.route = "bootstrapper";
            const Json& m = ManifestItem(manifest_, "vs");
            fs::path dest = ctx.install_dir / L"setup" / L"downloads" / L"vs_BuildTools.exe";
            std::string err;
            if (!HttpDownload(m.str("bootstrapper_url", "https://aka.ms/vs/17/release/vs_BuildTools.exe"), dest, progress, cancel, &err) ||
                !VerifyAuthenticode(dest, "Microsoft Corporation", &err)) {
                o.msg_nl = "Visual Studio-installatieprogramma niet bruikbaar: " + err;
                o.msg_en = "Visual Studio installer not usable: " + err;
                return o;
            }
            std::vector<std::wstring> args = {L"--wait", L"--passive", L"--norestart"};
            for (auto* c : kVsComponents) { args.push_back(L"--add"); args.push_back(W(c)); }
            args.push_back(L"--includeRecommended");
            r = run_.Run(dest, args, 4u * 3600u * 1000u);
        }
        o.code = r.code;
        o.restart_needed = r.code == 3010;
        o.ok = r.started && (r.code == 0 || r.code == 3010 || r.code == -1978335189 /* already installed */);
        o.msg_nl = o.ok ? (o.restart_needed ? "Geinstalleerd; Windows vraagt om een herstart. Start Setup daarna opnieuw." : "Geinstalleerd.")
                        : Format("Installatie mislukt (code %d). Installeer 'Desktopontwikkeling met C++' zelf via de Visual Studio Installer.", r.code);
        o.msg_en = o.ok ? (o.restart_needed ? "Installed; Windows asks for a restart. Start Setup again afterwards." : "Installed.")
                        : Format("Installation failed (code %d). Install 'Desktop development with C++' yourself with the Visual Studio Installer.", r.code);
        return o;
    }

    InstallOutcome InstallPython(const DetectContext& ctx, const ProgressFn& progress, std::atomic<bool>* cancel) {
        InstallOutcome o;
        const Json& m = ManifestItem(manifest_, "python");
        std::string url = m.str("url");
        fs::path tar = SystemTar();
        if (!url.empty() && !tar.empty()) {
            o.route = "embeddable";
            fs::path zip = ctx.install_dir / L"setup" / L"downloads" / L"python-embed.zip";
            fs::path dir = ctx.install_dir / L"hulpmiddelen" / L"python";
            std::string err;
            bool ok = HttpDownload(url, zip, progress, cancel, &err);
            std::string want = m.str("sha256");
            if (ok && !want.empty() && Sha256File(zip) != want) { ok = false; err = "SHA-256 klopt niet"; }
            std::string md5 = m.str("md5"); // INSTALL19 10 Oct: the checksum python.org publishes for this file
            if (ok && !md5.empty() && Md5File(zip) != md5) { ok = false; err = "controlesom (MD5 van python.org) klopt niet"; }
            if (ok) {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ProcResult r = run_.Run(tar, {L"-xf", zip.wstring(), L"-C", dir.wstring()}, 300000);
                ok = r.started && r.code == 0;
                if (!ok) err = "uitpakken mislukt";
            }
            if (ok && !VerifyAuthenticode(dir / L"python.exe", "Python Software Foundation", &err)) ok = false;
            if (ok) {
                o.ok = true;
                o.msg_nl = "Eigen Python staat in " + U(dir) + " (niets aan Windows veranderd).";
                o.msg_en = "Private Python is in " + U(dir) + " (nothing changed in Windows).";
                return o;
            }
            o.msg_nl = "Eigen Python mislukt: " + err + ". ";
            o.msg_en = "Private Python failed: " + err + ". ";
        }
        fs::path winget = ProcessRunner::FindOnPath(L"winget.exe", true);
        if (winget.empty()) {
            o.manual = true;
            o.msg_nl += "Installeer Python 3.12 zelf van python.org en klik op Opnieuw zoeken.";
            o.msg_en += "Install Python 3.12 yourself from python.org and click Search again.";
            plat_.OpenUrl("https://www.python.org/downloads/windows/");
            return o;
        }
        o.route = "winget";
        ProcResult r = run_.Run(winget, {L"install", L"--id", L"Python.Python.3.12", L"-e", L"--source", L"winget", L"--scope", L"user",
                                         L"--accept-package-agreements", L"--accept-source-agreements"},
                                3600u * 1000u);
        o.code = r.code;
        o.ok = r.started && (r.code == 0 || r.code == -1978335189);
        o.msg_nl += o.ok ? "Python geinstalleerd (alleen voor jou)." : Format("winget mislukt (code %d).", r.code);
        o.msg_en += o.ok ? "Python installed (for you only)." : Format("winget failed (code %d).", r.code);
        return o;
    }

    InstallOutcome InstallDolphin(const DetectContext& ctx, const ProgressFn& progress, std::atomic<bool>* cancel) {
        InstallOutcome o;
        const Json& m = ManifestItem(manifest_, "dolphintool");
        std::string url = m.str("url"), want = m.str("sha256");
        fs::path tar = SystemTar();
        bool lzma = false;
        if (!tar.empty()) {
            ProcResult r = run_.Run(tar, {L"--version"}, 10000);
            lzma = r.started && r.out.find("liblzma") != std::string::npos;
        }
        if (url.empty() || want.empty() || !lzma) {
            o.route = "browser";
            o.manual = true;
            plat_.OpenUrl(m.str("page_url", "https://dolphin-emu.org/download/"));
            o.msg_nl = "Download Dolphin zelf van dolphin-emu.org, pak het uit en kies daarna DolphinTool.exe met Bladeren. "
                       "Of pak de schijf uit met Dolphin (Eigenschappen > Bestandssysteem > Hele schijf uitpakken) en kies die map.";
            o.msg_en = "Download Dolphin yourself from dolphin-emu.org, extract it and then pick DolphinTool.exe with Browse. "
                       "Or extract the disc with Dolphin (Properties > Filesystem > Extract Entire Disc) and choose that folder.";
            return o;
        }
        o.route = "7z";
        fs::path arc = ctx.install_dir / L"setup" / L"downloads" / L"dolphin.7z";
        fs::path dir = ctx.install_dir / L"hulpmiddelen" / L"dolphin";
        fs::path tmp = ctx.install_dir / L"setup" / L"downloads" / L"dolphin-uitpakken";
        std::string err;
        bool ok = HttpDownload(url, arc, progress, cancel, &err);
        if (ok) ok = ExtractPinnedArchive(run_, tar, arc, want, m["extract"].strings(), tmp, dir, &err); // INSTALL19: unit-tested (A1/A2)
        o.ok = ok;
        o.msg_nl = ok ? "DolphinTool staat in " + U(dir) + "." : "DolphinTool ophalen mislukt: " + err + ".";
        o.msg_en = ok ? "DolphinTool is in " + U(dir) + "." : "Getting DolphinTool failed: " + err + ".";
        return o;
    }

    ProcessRunner& run_;
    IPlatform& plat_;
    Json manifest_;
};

// ------------------------------------------------------------------------------------------------ mock
class MockPrereqs final : public IPrereqEnv {
public:
    MockPrereqs(Json mock, Json manifest) : mock_(std::move(mock)), manifest_(std::move(manifest)) {
        for (auto& s : mock_["present"].strings()) installed_.push_back(s);
    }
    bool IsMock() const override { return true; }
    ConsentInfo Consent(const std::string& id) const override { return ConsentFrom(manifest_, id); }

    PrereqReport Detect(const DetectContext& ctx) override {
        PrereqReport rep;
        rep.sys.cores = mock_.integer("cores", 8);
        rep.sys.ram_gb = mock_.num("ram_gb", 16);
        rep.sys.windows = "Windows (mock)";
        rep.sys.winget = mock_.boolean("winget", true);
        rep.sys.tar_lzma = mock_.boolean("tar_lzma", true);
        for (const char* id : {"vs", "python", "dolphintool"}) {
            PrereqItem it;
            it.id = id;
            it.name = std::string(id) + " (mock)";
            it.required = std::string(id) != "dolphintool" || ctx.need_dolphintool;
            it.present = std::find(installed_.begin(), installed_.end(), id) != installed_.end();
            if (it.present) it.path = std::string("mock:\\") + id;
            else { it.note_nl = "ontbreekt (mock)"; it.note_en = "missing (mock)"; }
            rep.items.push_back(it);
        }
        return rep;
    }

    InstallOutcome Install(const std::string& id, const DetectContext&, const ProgressFn& progress, std::atomic<bool>* cancel) override {
        InstallOutcome o;
        const Json& a = mock_["actions"][id];
        o.route = "mock";
        if (id == "vs") o.route = mock_.boolean("winget", true) ? "winget" : "bootstrapper";
        if (id == "dolphintool" && !mock_.boolean("tar_lzma", true)) {
            o.route = "browser";
            o.manual = true;
            o.msg_nl = "tar zonder liblzma: download Dolphin zelf (browser).";
            o.msg_en = "tar without liblzma: download Dolphin yourself (browser).";
            return o;
        }
        double secs = a.num("seconds", 0.2);
        int steps = std::max(1, int(secs * 10));
        for (int i = 1; i <= steps; ++i) {
            if (cancel && cancel->load()) { o.msg_nl = o.msg_en = "geannuleerd"; return o; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (progress) progress(double(i) / steps, "mock " + id);
        }
        if (!a.boolean("sha256_ok", true)) {
            o.msg_nl = "SHA-256 klopt niet; niets geinstalleerd.";
            o.msg_en = "SHA-256 mismatch; nothing installed.";
            return o;
        }
        if (!a.boolean("authenticode_ok", true)) {
            o.msg_nl = "Digitale handtekening klopt niet; niets geinstalleerd.";
            o.msg_en = "Digital signature mismatch; nothing installed.";
            return o;
        }
        o.code = a.integer("exit", 0);
        o.restart_needed = o.code == 3010;
        o.ok = o.code == 0 || o.code == 3010;
        if (o.ok && !o.restart_needed) installed_.push_back(id);
        o.msg_nl = o.ok ? (o.restart_needed ? "Herstart nodig (mock 3010)." : "Geinstalleerd (mock).") : Format("Mislukt (mock code %d).", o.code);
        o.msg_en = o.ok ? (o.restart_needed ? "Restart needed (mock 3010)." : "Installed (mock).") : Format("Failed (mock code %d).", o.code);
        return o;
    }

private:
    Json mock_, manifest_;
    std::vector<std::string> installed_;
};

} // namespace

std::unique_ptr<IPrereqEnv> MakeRealPrereqs(ProcessRunner& runner, IPlatform& platform, Json manifest) {
    return std::make_unique<RealPrereqs>(runner, platform, std::move(manifest));
}
std::unique_ptr<IPrereqEnv> MakeMockPrereqs(Json mock, Json manifest) {
    return std::make_unique<MockPrereqs>(std::move(mock), std::move(manifest));
}

} // namespace sb
