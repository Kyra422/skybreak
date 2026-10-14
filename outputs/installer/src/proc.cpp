// INSTALL19: ProcessRunner (see proc.h).
#include "proc.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <thread>

namespace sb {

namespace {
// Tools a sandbox run may start directly. Children of the build script (cmake, ninja, cl, link, python,
// DolphinTool) are started by PowerShell inside the job, not by us.
const char* kSandboxAllow[] = {"powershell.exe", "vswhere.exe", "python.exe", "py.exe", "cmake.exe", "ninja.exe",
                               "dolphintool.exe", "tar.exe", "where.exe"};
// Never in a sandbox, and audited as installers in real mode.
const char* kInstallers[] = {"winget.exe", "vs_buildtools.exe", "vs_installer.exe", "setup.exe", "msiexec.exe"};
} // namespace

ProcessRunner::~ProcessRunner() { CloseJob(); }

void ProcessRunner::audit(const std::string& line) {
    std::string l = NowIso() + "|" + line;
    audit_.push_back(l);
    if (!audit_file_.empty()) AppendText(audit_file_, l + "\n");
    Log("[proc] %s", line.c_str());
}

bool ProcessRunner::Allowed(const fs::path& exe, std::string* why) const {
    std::string base = Lower(U(exe.filename()));
    bool installer = false;
    for (auto* n : kInstallers) if (base == n) installer = true;
    if (!sandbox_) return true;
    if (installer) {
        if (why) *why = "sandbox: installers and winget are never started";
        return false;
    }
    for (auto* n : kSandboxAllow) if (base == n) return true;
    if (why) *why = "sandbox: '" + base + "' is not on the allow-list";
    return false;
}

std::wstring ProcessRunner::QuoteArg(const std::wstring& a) {
    if (a.empty()) return L"\"\"";
    if (a.find_first_of(L" \t\"") == std::wstring::npos) return a;
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
    r += L'"';
    return r;
}

std::wstring ProcessRunner::CommandLine(const fs::path& exe, const std::vector<std::wstring>& args) {
    std::wstring cl = QuoteArg(exe.wstring());
    for (auto& a : args) { cl += L' '; cl += QuoteArg(a); }
    return cl;
}

fs::path ProcessRunner::FindOnPath(const std::wstring& name, bool allow_alias) {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"PATH", buf, DWORD(std::size(buf)));
    if (!n || n >= std::size(buf)) return {};
    std::wstring path(buf, n);
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(L';', start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring dir = path.substr(start, end - start);
        start = end + 1;
        if (dir.empty()) continue;
        if (dir.front() == L'"' && dir.back() == L'"' && dir.size() >= 2) dir = dir.substr(1, dir.size() - 2);
        fs::path cand = fs::path(dir) / name;
        std::error_code ec;
        if (fs::is_regular_file(cand, ec)) {
            if (!allow_alias && Lower(U(cand)).find("\\windowsapps\\") != std::string::npos) continue;
            return cand;
        }
    }
    return {};
}

ProcResult ProcessRunner::Run(const fs::path& exe, const std::vector<std::wstring>& args, uint32_t timeout_ms,
                              const fs::path& cwd) {
    ProcResult r;
    std::wstring cl = CommandLine(exe, args);
    std::string why;
    if (!Allowed(exe, &why)) {
        audit("REFUSED|" + U(cl) + "|" + why);
        r.error = why;
        return r;
    }
    audit("run|" + U(cl));
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { r.error = "CreatePipe failed"; return r; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCl = cl;
    BOOL ok = CreateProcessW(exe.c_str(), mutableCl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        r.error = Format("CreateProcess failed (%lu)", GetLastError());
        CloseHandle(rd);
        audit("failed|" + U(cl) + "|" + r.error);
        return r;
    }
    r.started = true;
    std::string out;
    std::thread reader([&] {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
    });
    DWORD w = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (w == WAIT_TIMEOUT) {
        r.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    reader.join();
    CloseHandle(rd);
    r.code = int(code);
    r.out = std::move(out);
    return r;
}

bool ProcessRunner::StartJob(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& cwd,
                             const fs::path& log, std::string* err) {
    CloseJob();
    std::wstring cl = CommandLine(exe, args);
    std::string why;
    if (!Allowed(exe, &why)) {
        audit("REFUSED|" + U(cl) + "|" + why);
        if (err) *err = why;
        return false;
    }
    audit("job|" + U(cl));
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) { if (err) *err = "CreateJobObject failed"; return false; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PRIORITY_CLASS;
    li.BasicLimitInformation.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);

    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE lf = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (lf == INVALID_HANDLE_VALUE) {
        CloseHandle(job);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (err) *err = "cannot open log " + U(log);
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = lf;
    si.hStdError = lf;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCl = cl;
    BOOL ok = CreateProcessW(exe.c_str(), mutableCl.data(), nullptr, nullptr, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(lf);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        DWORD e = GetLastError();
        CloseHandle(job);
        if (err) *err = Format("CreateProcess failed (%lu)", e);
        audit("failed|" + U(cl));
        return false;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        // Should not happen on Windows 8+ (nested jobs); without a job, cancel would leave children running.
        DWORD e = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        if (err) *err = Format("AssignProcessToJobObject failed (%lu)", e);
        return false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    job_ = job;
    proc_ = pi.hProcess;
    job_exit_ = -1;
    return true;
}

bool ProcessRunner::JobRunning() {
    if (!proc_) return false;
    if (WaitForSingleObject(HANDLE(proc_), 0) == WAIT_TIMEOUT) return true;
    DWORD code = 1;
    GetExitCodeProcess(HANDLE(proc_), &code);
    job_exit_ = int(code);
    CloseHandle(HANDLE(proc_));
    proc_ = nullptr;
    audit(Format("job-exit|%d", job_exit_));
    return false;
}

void ProcessRunner::CancelJob() {
    if (!job_) return;
    audit("cancel|TerminateJobObject");
    TerminateJobObject(HANDLE(job_), 1223 /* ERROR_CANCELLED */);
    if (proc_) WaitForSingleObject(HANDLE(proc_), 10000);
    // Wait until every process of the job is gone.
    for (int i = 0; i < 100 && ActiveJobProcesses() > 0; ++i) Sleep(100);
}

uint64_t ProcessRunner::PeakJobMemory() {
    if (!job_) return 0;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    if (!QueryInformationJobObject(HANDLE(job_), JobObjectExtendedLimitInformation, &li, sizeof li, nullptr)) return 0;
    return li.PeakJobMemoryUsed;
}

uint32_t ProcessRunner::ActiveJobProcesses() {
    if (!job_) return 0;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ai{};
    if (!QueryInformationJobObject(HANDLE(job_), JobObjectBasicAccountingInformation, &ai, sizeof ai, nullptr)) return 0;
    return ai.ActiveProcesses;
}

void ProcessRunner::CloseJob() {
    if (proc_) { CloseHandle(HANDLE(proc_)); proc_ = nullptr; }
    if (job_) { CloseHandle(HANDLE(job_)); job_ = nullptr; } // KILL_ON_JOB_CLOSE: nothing survives Setup
}

} // namespace sb
