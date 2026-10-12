#pragma once
// INSTALL19: shared helpers for SkyBreak-Setup and its tests (UTF-8/UTF-16, files, SHA-256, log, time).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sb {

namespace fs = std::filesystem;

inline constexpr const char* kProduct = "SkyBreak";
inline constexpr const char* kGameExeName = "SkyBreak-game";     // bin\SkyBreak-game.exe (built locally)
inline constexpr const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SkyBreak";

std::wstring W(const std::string& utf8);
std::string U(const std::wstring& w);
std::string U(const fs::path& p);
fs::path P(const std::string& utf8);

std::string Lower(std::string s);
bool StartsWith(const std::string& s, const std::string& prefix);
bool EndsWith(const std::string& s, const std::string& suffix);
std::string Trim(const std::string& s);
std::vector<std::string> SplitLines(const std::string& s);
std::string Format(const char* fmt, ...);
std::string Replace(std::string s, const std::string& from, const std::string& to);

bool ReadText(const fs::path& p, std::string& out);
bool WriteText(const fs::path& p, const std::string& text); // atomic: write .tmp then rename
bool AppendText(const fs::path& p, const std::string& text);

std::string Sha256Bytes(const void* data, size_t n);
std::string Sha256File(const fs::path& p); // lower-case hex, empty on error
std::string Md5File(const fs::path& p);    // INSTALL19 10 Oct: only to match python.org's published MD5 (integrity; authenticity = Authenticode)

std::string NowStamp(); // yyyyMMdd-HHmmss (local time)
std::string NowIso();   // yyyy-MM-dd HH:mm:ss

// Is `p` equal to or inside `dir` (both made absolute + normalised, case-insensitive on Windows)?
bool IsInside(const fs::path& p, const fs::path& dir);
fs::path Normal(const fs::path& p);

// Directory of the running exe.
fs::path ExeDir();
fs::path ExePath();

// Log to a file (and OutputDebugString). Thread-safe.
void SetLogFile(const fs::path& p);
fs::path LogFile();
void Log(const char* fmt, ...);

// Total size of all files under dir (0 when missing).
uint64_t DirSize(const fs::path& dir);

// Version compare "0.19.0" vs "0.18.2": <0, 0, >0.
int CompareVersions(const std::string& a, const std::string& b);

} // namespace sb
