// INSTALL19: real and sandbox platform (see platform.h).
#include "platform.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>  // REL22: RunningFromInstall

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace sb {

void IPlatform::Audit(const std::string& kind, const std::string& target) {
    std::string mode = IsSandbox() ? "sandbox" : "real";
    if (mutate == "leak-desktop" && kind == "shortcut" && target.find("Desktop") != std::string::npos) mode = "real";
    audit_.push_back(kind + "|" + target + "|" + mode);
    Log("[audit] %s %s (%s)", kind.c_str(), target.c_str(), mode.c_str());
}

bool IPlatform::WriteAudit(const fs::path& file) const {
    std::string s;
    for (auto& l : audit_) s += l + "\n";
    return WriteText(file, s);
}

bool IPlatform::CreateShortcut(const fs::path& lnk, const fs::path& target, const fs::path& workdir,
                               const fs::path& icon, const std::string& description, std::string* err) {
    Audit("shortcut", U(lnk));
    std::error_code ec;
    fs::create_directories(lnk.parent_path(), ec);
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* sl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl));
    bool ok = false;
    if (SUCCEEDED(hr)) {
        sl->SetPath(target.c_str());
        sl->SetWorkingDirectory(workdir.c_str());
        if (!icon.empty()) sl->SetIconLocation(icon.c_str(), 0);
        sl->SetDescription(W(description).c_str());
        IPersistFile* pf = nullptr;
        hr = sl->QueryInterface(IID_PPV_ARGS(&pf));
        if (SUCCEEDED(hr)) {
            hr = pf->Save(lnk.c_str(), TRUE);
            ok = SUCCEEDED(hr);
            pf->Release();
        }
        sl->Release();
    }
    if (!ok && err) *err = Format("snelkoppeling maken mislukt (0x%08lX)", (unsigned long)hr);
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

fs::path RealKnownFolder(Folder f) {
    KNOWNFOLDERID id{};
    switch (f) {
    case Folder::Desktop: id = FOLDERID_Desktop; break;
    case Folder::StartMenuPrograms: id = FOLDERID_Programs; break;
    case Folder::RoamingAppData: id = FOLDERID_RoamingAppData; break;
    case Folder::LocalAppData: id = FOLDERID_LocalAppData; break;
    case Folder::UserProfile: id = FOLDERID_Profile; break;
    case Folder::Temp: {
        wchar_t buf[MAX_PATH + 2];
        DWORD n = GetTempPathW(MAX_PATH + 1, buf);
        return fs::path(std::wstring(buf, n));
    }
    }
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

namespace {

// ------------------------------------------------------------------------------------------- real
class RealPlatform final : public IPlatform {
public:
    bool IsSandbox() const override { return false; }
    fs::path KnownFolder(Folder f) override { return RealKnownFolder(f); }

    bool WriteUninstallEntry(const Json& values) override {
        Audit("registry-write", "HKCU\\" + U(std::wstring(kUninstallKey)));
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
            return false;
        bool ok = true;
        for (auto& kv : values.items()) {
            std::wstring name = W(kv.first);
            if (kv.second.is_number() || kv.second.is_bool()) {
                DWORD v = kv.second.is_bool() ? DWORD(kv.second.as_bool()) : DWORD(kv.second.as_number());
                ok &= RegSetValueExW(k, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v) == ERROR_SUCCESS;
            } else {
                std::wstring v = W(kv.second.as_string());
                ok &= RegSetValueExW(k, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                                     DWORD((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
            }
        }
        RegCloseKey(k);
        return ok;
    }

    bool ReadUninstallEntry(Json& out) override {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
        out = Json::Object();
        for (DWORD i = 0;; ++i) {
            wchar_t name[256];
            DWORD nlen = 256, type = 0, dlen = 0;
            if (RegEnumValueW(k, i, name, &nlen, nullptr, &type, nullptr, &dlen) != ERROR_SUCCESS) break;
            std::vector<BYTE> data(dlen + 2);
            nlen = 256;
            if (RegEnumValueW(k, i, name, &nlen, nullptr, &type, data.data(), &dlen) != ERROR_SUCCESS) break;
            if (type == REG_DWORD && dlen >= 4) out.set(U(std::wstring(name)), Json(int64_t(*reinterpret_cast<DWORD*>(data.data()))));
            else if (type == REG_SZ || type == REG_EXPAND_SZ)
                out.set(U(std::wstring(name)), Json(U(std::wstring(reinterpret_cast<wchar_t*>(data.data())))));
        }
        RegCloseKey(k);
        return true;
    }

    bool DeleteUninstallEntry() override {
        Audit("registry-delete", "HKCU\\" + U(std::wstring(kUninstallKey)));
        LSTATUS s = RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
        return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
    }

    void OpenUrl(const std::string& url) override {
        Audit("open-url", url);
        ShellExecuteW(nullptr, L"open", W(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    void OpenFolder(const fs::path& dir) override {
        Audit("open-folder", U(dir));
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
};

// ------------------------------------------------------------------------------------------- sandbox
class SandboxPlatform final : public IPlatform {
public:
    explicit SandboxPlatform(fs::path dir) : dir_(Normal(dir)) {
        std::error_code ec;
        fs::create_directories(dir_, ec);
    }
    bool IsSandbox() const override { return true; }
    fs::path Root() const override { return dir_; }

    fs::path KnownFolder(Folder f) override {
        fs::path p;
        switch (f) {
        case Folder::Desktop: p = dir_ / L"Desktop"; break;
        case Folder::StartMenuPrograms: p = dir_ / L"StartMenu" / L"Programs"; break;
        case Folder::RoamingAppData: p = dir_ / L"AppData" / L"Roaming"; break;
        case Folder::LocalAppData: p = dir_ / L"AppData" / L"Local"; break;
        case Folder::Temp: p = dir_ / L"Temp"; break;
        case Folder::UserProfile: p = dir_; break;
        }
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }

    fs::path RegFile() const { return dir_ / L"registry.json"; }

    bool WriteUninstallEntry(const Json& values) override {
        Audit("registry-write", U(RegFile()));
        Json all = LoadReg();
        Json& key = all.set("HKCU\\" + U(std::wstring(kUninstallKey)), Json::Object());
        for (auto& kv : values.items()) key.set(kv.first, kv.second);
        return WriteText(RegFile(), all.Dump(2) + "\n");
    }
    bool ReadUninstallEntry(Json& out) override {
        Json all = LoadReg();
        const Json& k = all["HKCU\\" + U(std::wstring(kUninstallKey))];
        if (!k.is_object()) return false;
        out = k;
        return true;
    }
    bool DeleteUninstallEntry() override {
        Audit("registry-delete", U(RegFile()));
        Json all = LoadReg();
        all.erase("HKCU\\" + U(std::wstring(kUninstallKey)));
        return WriteText(RegFile(), all.Dump(2) + "\n");
    }
    void OpenUrl(const std::string& url) override { Audit("open-url(logged only)", url); }
    void OpenFolder(const fs::path& d) override { Audit("open-folder(logged only)", U(d)); }

private:
    Json LoadReg() const {
        std::string t;
        if (!ReadText(RegFile(), t)) return Json::Object();
        Json j = Json::Parse(t);
        return j.is_object() ? j : Json::Object();
    }
    fs::path dir_;
};

} // namespace

std::unique_ptr<IPlatform> MakeRealPlatform() { return std::make_unique<RealPlatform>(); }
std::unique_ptr<IPlatform> MakeSandboxPlatform(const fs::path& dir) { return std::make_unique<SandboxPlatform>(dir); }

uint64_t FreeBytes(const fs::path& any) {
    fs::path p = Normal(any);
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec)) {
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExW(p.c_str(), &avail, nullptr, nullptr)) return 0;
    return avail.QuadPart;
}

std::string FileSystemName(const fs::path& any) {
    fs::path root = Normal(any).root_path();
    wchar_t fsname[64] = {};
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsname, 64)) return "";
    return U(std::wstring(fsname));
}

bool LooksLikeOneDrive(const fs::path& p) {
    std::string s = Lower(U(Normal(p)));
    if (s.find("\\onedrive") != std::string::npos) return true;
    for (const wchar_t* var : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(var, buf, DWORD(std::size(buf)));
        if (n && n < std::size(buf) && IsInside(p, fs::path(std::wstring(buf, n)))) return true;
    }
    return false;
}

bool HasNonAscii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return true;
    return false;
}

// REL22: is a SkyBreak program of THIS installation running? Every process image is compared by full path with
// <install>\SkyBreak.exe and with <install>\bin\*.exe; when the text differs (8.3 short names, a junction, subst) the
// same-file check (volume serial + file index) decides. A program with the same NAME elsewhere (another installation, a
// developer build) never matches. Read-only: the processes are only queried (PROCESS_QUERY_LIMITED_INFORMATION).
namespace {
bool FileId(const fs::path& p, BY_HANDLE_FILE_INFORMATION& fi) {
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL ok = GetFileInformationByHandle(h, &fi);
    CloseHandle(h);
    return ok != FALSE;
}
bool SameFile(const fs::path& a, const fs::path& b) {
    BY_HANDLE_FILE_INFORMATION fa{}, fb{};
    if (!FileId(a, fa) || !FileId(b, fb)) return false;
    return fa.dwVolumeSerialNumber == fb.dwVolumeSerialNumber && fa.nFileIndexHigh == fb.nFileIndexHigh &&
           fa.nFileIndexLow == fb.nFileIndexLow;
}
bool EqualPath(const fs::path& a, const fs::path& b) {
    std::wstring x = Normal(a).wstring(), y = Normal(b).wstring();
    return x.size() == y.size() && CompareStringOrdinal(x.c_str(), int(x.size()), y.c_str(), int(y.size()), TRUE) == CSTR_EQUAL;
}
} // namespace

std::vector<RunningProgram> RunningFromInstall(const fs::path& install_dir) {
    std::vector<RunningProgram> out;
    if (install_dir.empty()) return out;
    const fs::path start = install_dir / L"SkyBreak.exe", bin = install_dir / L"bin";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    const DWORD self = GetCurrentProcessId();
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == 0 || pe.th32ProcessID == self) continue;
        std::wstring exe = pe.szExeFile;
        // cheap name filter first: the start program, or any .exe (a game exe in bin\ may have another name)
        if (exe.size() < 4 || CompareStringOrdinal(exe.c_str() + exe.size() - 4, 4, L".exe", 4, TRUE) != CSTR_EQUAL) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t buf[32768];
        DWORD n = DWORD(std::size(buf));
        BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &n);
        CloseHandle(h);
        if (!ok || n == 0) continue;
        fs::path image(std::wstring(buf, n));
        bool match = EqualPath(image, start) || EqualPath(image.parent_path(), bin);
        if (!match) {
            // same file under another spelling (short name, junction, subst): compare identities of the candidates only
            fs::path name = image.filename();
            if (CompareStringOrdinal(name.c_str(), -1, L"SkyBreak.exe", -1, TRUE) == CSTR_EQUAL) match = SameFile(image, start);
            if (!match) {
                std::error_code ec;
                fs::path twin = bin / name;
                if (fs::exists(twin, ec)) match = SameFile(image, twin);
            }
        }
        if (match) out.push_back({uint32_t(pe.th32ProcessID), image});
    }
    CloseHandle(snap);
    return out;
}

} // namespace sb
