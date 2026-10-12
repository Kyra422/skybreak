#pragma once
// INSTALL19: ProcessRunner. Every program Setup starts goes through here:
//  - Run(): short synchronous call with captured output (vswhere, --version, DolphinTool header ...);
//  - StartJob(): the long build in a Job object (KILL_ON_JOB_CLOSE, below-normal priority), output to a log file,
//    cancel = TerminateJobObject (kills powershell + cmake + ninja + cl together).
// Every launch is written to an audit log. In sandbox mode only an allow-list of tools may run; installers,
// winget and anything else are REFUSED (and audited as such), so a sandbox test can never install software.
#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace sb {

struct ProcResult {
    bool started = false;
    int code = -1;          // process exit code (DWORD as int)
    bool timed_out = false;
    std::string out;        // stdout + stderr
    std::string error;      // why it did not start
};

class ProcessRunner {
public:
    ProcessRunner() = default;
    ~ProcessRunner();
    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    void SetSandbox(bool on) { sandbox_ = on; }
    bool Sandbox() const { return sandbox_; }
    void SetAuditFile(const fs::path& p) { audit_file_ = p; }
    const std::vector<std::string>& AuditLines() const { return audit_; }

    // true when `exe` may be started in the current mode (`why` explains a refusal).
    bool Allowed(const fs::path& exe, std::string* why) const;

    ProcResult Run(const fs::path& exe, const std::vector<std::wstring>& args, uint32_t timeout_ms = 60000,
                   const fs::path& cwd = {});

    // Long-running job. Output (stdout+stderr) is appended to `log`.
    bool StartJob(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& cwd, const fs::path& log,
                  std::string* err);
    bool JobRunning();
    int JobExitCode() const { return job_exit_; }
    void CancelJob();           // TerminateJobObject
    uint64_t PeakJobMemory();   // bytes (whole job)
    uint32_t ActiveJobProcesses();
    void CloseJob();

    static std::wstring QuoteArg(const std::wstring& a);
    static std::wstring CommandLine(const fs::path& exe, const std::vector<std::wstring>& args);
    // Search PATH (like where.exe); skips the WindowsApps aliases unless allow_alias.
    static fs::path FindOnPath(const std::wstring& name, bool allow_alias = false);

private:
    void audit(const std::string& line);
    bool sandbox_ = false;
    fs::path audit_file_;
    std::vector<std::string> audit_;
    void* job_ = nullptr;
    void* proc_ = nullptr;
    int job_exit_ = -1;
};

} // namespace sb
