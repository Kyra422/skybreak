#pragma once
// INSTALL19: every side effect of Setup outside its own install folder goes through IPlatform:
// known folders (desktop, Start menu, AppData), the per-user uninstall entry (HKCU), shortcuts, opening URLs.
// RealPlatform touches the real system; SandboxPlatform (--sandbox DIR) redirects all of it into DIR
// (registry -> DIR\registry.json, known folders -> DIR\{Desktop,StartMenu,AppData\...}, URLs only logged).
// Every action is written to an audit list (tests check it).
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "json.h"

namespace sb {

enum class Folder { Desktop, StartMenuPrograms, RoamingAppData, LocalAppData, Temp, UserProfile };

struct IPlatform {
    virtual ~IPlatform() = default;
    virtual bool IsSandbox() const = 0;
    virtual fs::path Root() const { return {}; } // sandbox dir (empty for real)
    virtual fs::path KnownFolder(Folder f) = 0;

    // HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\SkyBreak (values: strings and DWORDs).
    virtual bool WriteUninstallEntry(const Json& values) = 0;
    virtual bool ReadUninstallEntry(Json& out) = 0;
    virtual bool DeleteUninstallEntry() = 0;

    virtual bool CreateShortcut(const fs::path& lnk, const fs::path& target, const fs::path& workdir,
                                const fs::path& icon, const std::string& description, std::string* err);
    virtual void OpenUrl(const std::string& url) = 0;
    virtual void OpenFolder(const fs::path& dir) = 0;

    // Audit trail: "kind|target" per side effect (tests read it via WriteAudit).
    void Audit(const std::string& kind, const std::string& target);
    const std::vector<std::string>& AuditLines() const { return audit_; }
    bool WriteAudit(const fs::path& file) const;

    // Test hook (--mutate leak-desktop): the sandbox reports its desktop as REAL in the audit.
    std::string mutate;

protected:
    std::vector<std::string> audit_;
};

std::unique_ptr<IPlatform> MakeRealPlatform();
std::unique_ptr<IPlatform> MakeSandboxPlatform(const fs::path& dir);

// Read-only system queries (safe in every mode).
uint64_t FreeBytes(const fs::path& anyPathOnVolume); // walks up to an existing parent
std::string FileSystemName(const fs::path& anyPathOnVolume);
bool LooksLikeOneDrive(const fs::path& p);
bool HasNonAscii(const std::string& utf8);
// REL22: programs started from THIS installation: <install>/SkyBreak.exe and every exe in <install>/bin (the game exe), matched
// by full path (or the same file under another spelling), never by name alone. Read-only: the processes are never touched.
struct RunningProgram {
    uint32_t pid = 0;
    fs::path image;
};
std::vector<RunningProgram> RunningFromInstall(const fs::path& install_dir);
fs::path RealKnownFolder(Folder f);

} // namespace sb
