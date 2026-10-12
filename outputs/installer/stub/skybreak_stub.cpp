// INSTALL19: SkyBreak.exe - the start program in the install folder (our own code; shortcuts point here).
// Starts bin\<name>.exe "<game>" "<install>\instellingen.toml" without a console window, writes its output to
// logs\speel-YYYYMMDD-HHMMSS.log (newest 20 kept, like Speel.ps1), waits, and shows a short message after a crash or
// a freeze report ([FREEZE8] =====). The game folder is <install>\game or the path in hulp\spelmap.txt; the exe name
// comes from hulp\exenaam.txt (default SkyBreak-game). Environment variables pass through unchanged.
//   --diagnose      sets NWII_SAMPLE=1 (like Speel.ps1 -Diagnose)
//   --no-dialog     never show a message box (tests); also SKYBREAK_NO_DIALOG=1
// Exit code = the game's exit code (2 when the game could not be started).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path ExeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

std::wstring FirstLine(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s;
    std::getline(f, s);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring Quote(const std::wstring& a) {
    if (a.find_first_of(L" \t\"") == std::wstring::npos && !a.empty()) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; bs = 0; continue; }
        r.append(bs, L'\\');
        bs = 0;
        r += c;
    }
    r.append(bs * 2, L'\\');
    return r + L"\"";
}

bool g_no_dialog = false;

void Message(const std::wstring& text, const fs::path& open_dir) {
    if (g_no_dialog) return;
    if (open_dir.empty()) {
        MessageBoxW(nullptr, text.c_str(), L"SkyBreak", MB_ICONWARNING | MB_OK);
        return;
    }
    if (MessageBoxW(nullptr, (text + L"\n\nLogmap openen?").c_str(), L"SkyBreak", MB_ICONWARNING | MB_YESNO) == IDYES)
        ShellExecuteW(nullptr, L"open", open_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf(b, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

bool LogHas(const fs::path& log, const char* needle) {
    std::ifstream f(log, std::ios::binary);
    std::string line;
    while (std::getline(f, line))
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool diagnose = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--diagnose")) diagnose = true;
        else if (!wcscmp(argv[i], L"--no-dialog")) g_no_dialog = true;
    }
    LocalFree(argv);
    wchar_t env[8] = {};
    if (GetEnvironmentVariableW(L"SKYBREAK_NO_DIALOG", env, 8) && env[0] == L'1') g_no_dialog = true;

    const fs::path dir = ExeDir();
    std::wstring name = FirstLine(dir / L"hulp" / L"exenaam.txt");
    bool name_ok = !name.empty() && std::all_of(name.begin(), name.end(), [](wchar_t c) {
        return iswalnum(c) || c == L'-' || c == L'_' || c == L'.';
    });
    if (!name_ok) name = L"SkyBreak-game";
    const fs::path exe = dir / L"bin" / (name + L".exe");
    fs::path game = dir / L"game";
    std::wstring pointer = FirstLine(dir / L"hulp" / L"spelmap.txt");
    if (!pointer.empty()) {
        fs::path p(pointer);
        game = p.is_absolute() ? p : dir / p;
    }
    const fs::path settings = dir / L"instellingen.toml";
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec) || !fs::is_regular_file(game / L"sys" / L"main.dol", ec)) {
        Message(L"SkyBreak is niet compleet (" + (fs::is_regular_file(exe, ec) ? game.wstring() + L" ontbreekt" : exe.filename().wstring() + L" ontbreekt") +
                    L").\nStart SkyBreak-Setup en kies Herstellen.",
                {});
        return 2;
    }
    if (!fs::is_regular_file(settings, ec) && fs::is_regular_file(dir / L"instellingen.standaard.toml", ec))
        fs::copy_file(dir / L"instellingen.standaard.toml", settings, ec);

    // log of this session; keep the newest 20
    const fs::path logs = dir / L"logs";
    fs::create_directories(logs, ec);
    std::vector<fs::path> old;
    for (fs::directory_iterator it(logs, ec), end; !ec && it != end; it.increment(ec)) {
        std::wstring f = it->path().filename().wstring();
        if (f.rfind(L"speel-", 0) == 0 && it->path().extension() == L".log") old.push_back(it->path());
    }
    std::sort(old.begin(), old.end());
    while (old.size() >= 20) { fs::remove(old.front(), ec); old.erase(old.begin()); }
    const fs::path log = logs / (L"speel-" + Stamp() + L".log");

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE lf = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (diagnose) SetEnvironmentVariableW(L"NWII_SAMPLE", L"1");
    std::wstring cl = Quote(exe.wstring()) + L" " + Quote(game.wstring()) + L" " + Quote(settings.wstring());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = lf;
    si.hStdError = lf;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        DWORD e = GetLastError();
        wchar_t b[64];
        swprintf(b, 64, L" (fout %lu)", e);
        Message(L"Het spel kon niet starten" + std::wstring(b) + L". Mogelijk blokkeert Slim app-beheer of een virusscanner het zelf gebouwde spel.", logs);
        return 2;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    bool freeze = LogHas(log, "[FREEZE8] =====");
    if (code != 0) {
        wchar_t b[96];
        swprintf(b, 96, L"Het spel is gestopt met foutcode %lu (0x%08lX).", code, code);
        std::wstring t = b;
        if (LogHas(log, "[GL] fatal")) t += L"\nOpenGL werkt niet: controleer het stuurprogramma van je videokaart.";
        Message(t + L"\nHet logboek staat in " + log.wstring(), logs);
    } else if (freeze) {
        Message(L"De vastloopdetector heeft iets gemeld. Het verslag staat in " + log.wstring(), logs);
    }
    return int(code);
}
