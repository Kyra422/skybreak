// REL22 test double: a harmless stand-in for SkyBreak.exe / bin\SkyBreak-game.exe. The tests copy it to a SANDBOX path and start
// it, to check that Setup and Bouw-SkywardSword.ps1 notice a running SkyBreak of THAT installation (by full path) and ask the
// player to close it. It only sleeps: "dummy-skybreak.exe [seconds]" (default 10) exits after that time, or earlier when a file
// "<exe>.stop" appears next to it. No window, no console, no files written.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwchar>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    double seconds = 10;
    if (cmd && *cmd) seconds = _wtof(cmd);
    wchar_t exe[32768];
    DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(sizeof exe / sizeof exe[0]));
    std::wstring stop = std::wstring(exe, n) + L".stop";
    ULONGLONG end = GetTickCount64() + ULONGLONG(seconds * 1000);
    while (GetTickCount64() < end) {
        if (GetFileAttributesW(stop.c_str()) != INVALID_FILE_ATTRIBUTES) break;
        Sleep(100);
    }
    return 0;
}
