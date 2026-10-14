#pragma once
// UIT14: NWII_CRASH_REPORT=1 - diagnostics for rare crashes such as the 0xC0000005 seen once at process exit
// (UI13 open point). On an unhandled exception it writes, straight to the stdout handle (the CRT may already be
// torn down): the exception, the faulting module + offset, the thread (main or other) and how far the exit had
// got (running / main's teardown / exit() running static destructors), then the stack as module+offset frames
// (RtlVirtualUnwind; no dbghelp, so it is safe under the loader lock). work/uit14/symbolize.py turns the frames
// into function/file/line with the PDB. Then the exception continues as before (Windows ends the process).
// Installed from nwii::ui::OnConfigLoaded, so probe runs (no UI) get it too.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <intrin.h>

namespace nwii::ui::crashreport {

// 0 running, 1 main's teardown started (ui::Shutdown), 2 exit() is running (atexit handlers / static destructors)
inline std::atomic<int> g_phase{0};
inline DWORD g_main_tid = 0;
inline LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;

inline void Out(const char* s) {
    DWORD w = 0;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, s, DWORD(std::strlen(s)), &w, nullptr);
}

inline void ModuleOf(DWORD64 addr, char* name, size_t n, DWORD64& base) {
    HMODULE m = nullptr;
    name[0] = 0;
    base = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &m) && m) {
        char full[MAX_PATH] = {};
        GetModuleFileNameA(m, full, MAX_PATH);
        const char* b = std::strrchr(full, '\\');
        std::snprintf(name, n, "%s", b ? b + 1 : full);
        base = reinterpret_cast<DWORD64>(m);
    } else {
        std::snprintf(name, n, "?");
    }
}

// Stack as module+offset frames; a broken stack ends the walk (SEH) instead of crashing the report.
inline void Unwind(CONTEXT& c) {
    char line[256], mod[128];
    DWORD64 base = 0;
    __try {
        for (int i = 0; i < 48 && c.Rip; ++i) {
            ModuleOf(c.Rip, mod, sizeof mod, base);
            std::snprintf(line, sizeof line, "[CRASH] frame %2d %s+0x%llX\n", i, mod, (unsigned long long)(c.Rip - base));
            Out(line);
            DWORD64 image = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
            if (!f) { // leaf function: return address on top of the stack
                if (!c.Rsp) break;
                c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                c.Rsp += 8;
            } else {
                void* handler_data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &handler_data, &frame, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Out("[CRASH] (stack walk stopped: unreadable frame)\n");
    }
}

inline LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    static std::atomic<int> once{0};
    if (once.exchange(1) == 0 && ep && ep->ExceptionRecord && ep->ContextRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        char line[512], mod[128];
        DWORD64 base = 0;
        const DWORD64 pc = reinterpret_cast<DWORD64>(er->ExceptionAddress);
        ModuleOf(pc, mod, sizeof mod, base);
        const DWORD tid = GetCurrentThreadId();
        static const char* kPhase[] = {"running", "main teardown (after the host loop)", "exit() / static destructors"};
        const int ph = g_phase.load();
        std::snprintf(line, sizeof line,
                      "\n[CRASH] ===== unhandled exception 0x%08lX at %s+0x%llX (pc 0x%llX) thread %lu%s, phase: %s =====\n",
                      er->ExceptionCode, mod, (unsigned long long)(pc - base), (unsigned long long)pc, tid,
                      tid == g_main_tid ? " (main)" : " (not main)", kPhase[ph < 0 || ph > 2 ? 0 : ph]);
        Out(line);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            std::snprintf(line, sizeof line, "[CRASH] access violation: %s address 0x%llX\n",
                          er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
                          (unsigned long long)er->ExceptionInformation[1]);
            Out(line);
        }
#if defined(_M_X64)
        CONTEXT c = *ep->ContextRecord;
        std::snprintf(line, sizeof line, "[CRASH] rip=%llX rsp=%llX rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX\n",
                      (unsigned long long)c.Rip, (unsigned long long)c.Rsp, (unsigned long long)c.Rax,
                      (unsigned long long)c.Rbx, (unsigned long long)c.Rcx, (unsigned long long)c.Rdx,
                      (unsigned long long)c.Rsi, (unsigned long long)c.Rdi);
        Out(line);
        Unwind(c);
#endif
        Out("[CRASH] ===== end (symbolize: work/uit14/symbolize.py <log>) =====\n");
    }
    return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// STAB20: abort() / std::terminate (an uncaught C++ exception, a noexcept violation), a CRT invalid parameter and a
// pure virtual call all end in __fastfail = exit 0xC0000409, which never reaches the SEH filter above (and Windows
// writes nothing to the log). ALWAYS installed (it only acts when the process is already dying): the reason, the
// uncaught exception's what(), thread + exit phase and the stack as module+offset frames for symbolize.py, straight
// to the stdout handle; then the CRT's own behaviour continues unchanged (same exit code).
inline void ReportFastFailPath(const char* reason) {
    static std::atomic<int> once{0};
    if (once.exchange(1) != 0) return;
    char line[640], mod[128];
    const DWORD tid = GetCurrentThreadId();
    static const char* kPhase[] = {"running", "main teardown (after the host loop)", "exit() / static destructors"};
    const int ph = g_phase.load();
    std::snprintf(line, sizeof line, "\n[CRASH] ===== %s on thread %lu%s, phase: %s (exit code will be 0xC0000409) =====\n",
                  reason, tid, tid == g_main_tid ? " (main)" : " (not main)", kPhase[ph < 0 || ph > 2 ? 0 : ph]);
    Out(line);
    if (std::exception_ptr ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::snprintf(line, sizeof line, "[CRASH] uncaught C++ exception: %s\n", e.what());
            Out(line);
        } catch (...) {
            Out("[CRASH] uncaught C++ exception (not a std::exception)\n");
        }
    }
    void* frames[48] = {};
    const USHORT n = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        DWORD64 base = 0;
        ModuleOf(reinterpret_cast<DWORD64>(frames[i]), mod, sizeof mod, base);
        std::snprintf(line, sizeof line, "[CRASH] frame %2u %s+0x%llX\n", unsigned(i), mod,
                      (unsigned long long)(reinterpret_cast<DWORD64>(frames[i]) - base));
        Out(line);
    }
    Out("[CRASH] ===== end (symbolize: work/uit14/symbolize.py <log>) =====\n");
}
inline void OnAbortSignal(int) { ReportFastFailPath("abort() / std::terminate"); } // returns: abort() goes on
inline void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    ReportFastFailPath("CRT invalid parameter");
    __fastfail(5); // FAST_FAIL_INVALID_ARG: what the CRT does without a handler
}
inline void __cdecl OnPurecall() {
    ReportFastFailPath("pure virtual function call");
    std::abort();
}
inline void InstallFastFailReport() {
    std::signal(SIGABRT, &OnAbortSignal);
    _set_invalid_parameter_handler(&OnInvalidParameter);
    _set_purecall_handler(&OnPurecall);
}

inline void Install() {
    static bool done = false;
    if (done) return;
    done = true;                                    // STAB20: the fast-fail report and the phase are always on
    g_main_tid = GetCurrentThreadId();
    InstallFastFailReport();                        // STAB20
    std::atexit([] { g_phase.store(2); });
    const char* e = std::getenv("NWII_CRASH_REPORT");
    if (!e || *e != '1') return;
    g_prev = SetUnhandledExceptionFilter(&Filter);
    Out("[CRASH] crash report armed (NWII_CRASH_REPORT=1)\n");
}

// LAUNCH19: [diag] crash_report = true reaches the process as NWII_CRASH_REPORT=1 only after the config load (the
// settings UI sets it when the player did not), i.e. after Install(): arm the report then. Once only.
inline void ArmIfRequested() {
    static bool armed = false;
    const char* e = std::getenv("NWII_CRASH_REPORT");
    if (armed || !e || *e != '1') return;
    armed = true;
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(&Filter);
    if (cur == &Filter) return; // Install() armed it already
    g_prev = cur;
    Out("[CRASH] crash report armed ([diag] crash_report)\n");
}

inline void MarkTeardown() {
    int expect = 0;
    g_phase.compare_exchange_strong(expect, 1);
}

} // namespace nwii::ui::crashreport
#else
namespace nwii::ui::crashreport {
inline void Install() {}
inline void ArmIfRequested() {} // LAUNCH19
inline void MarkTeardown() {}
} // namespace nwii::ui::crashreport
#endif
