// INSTALL19: build orchestration and progress model (see pipeline.h).
#include "pipeline.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <fstream>

#include "proc.h"

namespace sb {

std::vector<std::wstring> BuildArgs(const BuildOptions& o) {
    std::vector<std::wstring> a = {L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-File", o.script.wstring(),
                                   L"-Iso", o.disc.wstring(), L"-Uitvoer", o.install_dir.wstring(), L"-Werkmap", o.work_dir.wstring()};
    if (!o.pointer_mode && !o.game_dir.empty()) { a.push_back(L"-SpelMap"); a.push_back(o.game_dir.wstring()); }
    if (!o.nand_root.empty()) { a.push_back(L"-NandRoot"); a.push_back(W(o.nand_root)); }
    if (!o.product.empty()) { a.push_back(L"-Product"); a.push_back(W(o.product)); }
    if (!o.exe_name.empty()) { a.push_back(L"-ExeNaam"); a.push_back(W(o.exe_name)); }
    if (!o.progress_json.empty()) { a.push_back(L"-VoortgangJson"); a.push_back(o.progress_json.wstring()); }
    if (o.jobs > 0) { a.push_back(L"-Taken"); a.push_back(std::to_wstring(o.jobs)); }
    auto opt = [&](const wchar_t* name, const fs::path& p) {
        if (!p.empty()) { a.push_back(name); a.push_back(p.wstring()); }
    };
    opt(L"-Python", o.python);
    opt(L"-CMake", o.cmake);
    opt(L"-Ninja", o.ninja);
    opt(L"-DolphinTool", o.dolphintool);
    opt(L"-Bronnen", o.bronnen);
    if (!o.keep_build) a.push_back(L"-Opruimen");
    return a;
}

std::vector<StepPlan> PlanSteps(int jobs, bool pointer_mode, bool image, bool incremental) {
    jobs = std::max(1, jobs);
    double msvc_full = 31.0 * 690.0 / jobs; // calibrated: 1348 s x 16 jobs / 690 steps (PKG10)
    std::vector<StepPlan> p = {
        {1, "Hulpprogramma's controleren", "Checking tools", 4},
        {2, "Schijf controleren", "Checking the disc", 3},
        {3, "Spelbestanden uitpakken", "Extracting game files", pointer_mode ? 2.0 : (image ? 90.0 : 45.0)},
        {4, "REL-modules controleren", "Checking REL modules", 3},
        {5, "Vertaler bouwen", "Building the translator", incremental ? 4.0 : 35.0},
        {6, "Code genereren", "Generating code", 12},
        {7, "Spel compileren", "Compiling the game", incremental ? msvc_full * 0.45 : msvc_full},
        {8, "Map samenstellen", "Assembling the folder", 4},
    };
    return p;
}

double Pipeline::Overall(const std::vector<StepPlan>& plan, const std::vector<int>& done, int cur, double frac) {
    double total = 0, acc = 0;
    for (auto& s : plan) {
        total += s.expected_s;
        if (std::find(done.begin(), done.end(), s.step) != done.end()) acc += s.expected_s;
        else if (s.step == cur) acc += s.expected_s * std::clamp(frac, 0.0, 1.0);
    }
    return total > 0 ? std::clamp(acc / total, 0.0, 1.0) : 0.0;
}

void Pipeline::InitModel(const BuildOptions& o, bool incremental) {
    opt_ = o;
    std::error_code ec;
    bool image = !fs::is_directory(o.disc, ec);
    plan_ = PlanSteps(o.jobs, o.pointer_mode, image, incremental);
    st_ = PipelineState{};
    progress_off_ = log_off_ = 0;
    progress_partial_.clear();
    ema_rate_ = 0;
    last_frac_ = last_frac_t_ = 0;
    step_start_s_ = 0;
}

bool Pipeline::Start(ProcessRunner& runner, const BuildOptions& o, bool incremental, std::string* err) {
    InitModel(o, incremental);
    std::error_code ec;
    fs::create_directories(o.progress_json.parent_path(), ec);
    fs::remove(o.progress_json, ec); // fresh event stream for this run (resume = new run)
    log_off_ = fs::exists(o.log, ec) ? fs::file_size(o.log, ec) : 0;
    AppendText(o.log, "\r\n===== SkyBreak-Setup " + NowIso() + " =====\r\n");
    log_off_ = fs::file_size(o.log, ec);
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    fs::path ps = fs::path(std::wstring(sys, n)) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    start_tick_ = GetTickCount64();
    running_ = runner.StartJob(ps, BuildArgs(o), o.install_dir.parent_path(), o.log, err);
    if (!running_) {
        st_.failed = true;
        st_.fail_kind = "proces";
        st_.fail_msg = err ? *err : "start failed";
    }
    return running_;
}

void Pipeline::ApplyEvent(const Json& ev) {
    std::string status = ev.str("status");
    int step = ev.integer("stap", 0);
    double t = ev.num("t", st_.elapsed_s);
    if (status != "wacht") st_.waiting = false; // REL22: any later event = no longer waiting
    if (status == "log") {
        // Only follow logs in this build's log folder, never arbitrary event paths.
        fs::path path = Normal(P(ev.str("msg")));
        if (Normal(path.parent_path()) == Normal(opt_.work_dir / L"logs") && path != st_.detail_log) {
            st_.detail_log = path;
            log_off_ = 0;
        }
    } else if (status == "wacht") {                    // REL22: SkyBreak of this installation runs; the script polls until it is closed
        st_.waiting = true;
        st_.wait_msg = ev.str("msg");
    } else if (status == "start") {
        st_.step = step;
        st_.step_name = ev.str("naam");
        st_.a = st_.b = 0;
        st_.step_fraction = 0;
        step_start_s_ = t;
        ema_rate_ = 0;
        last_frac_ = 0;
        last_frac_t_ = t;
    } else if (status == "voortgang") {
        if (step == st_.step) {
            st_.a = ev.integer("a", 0);
            st_.b = ev.integer("b", 0);
            double f = st_.b > 0 ? double(st_.a) / st_.b : 0;
            // the counter of the real build replaces a short "Re-running CMake" counter ([1/1]) before it
            st_.step_fraction = f;
            double dt = t - last_frac_t_;
            if (f < last_frac_) { last_frac_ = f; last_frac_t_ = t; }
            if (dt > 0.5 && f >= last_frac_) {
                double rate = (f - last_frac_) / dt;
                ema_rate_ = ema_rate_ <= 0 ? rate : 0.8 * ema_rate_ + 0.2 * rate;
                last_frac_ = f;
                last_frac_t_ = t;
            }
        }
    } else if (status == "klaar") {
        if (std::find(st_.done_steps.begin(), st_.done_steps.end(), step) == st_.done_steps.end()) st_.done_steps.push_back(step);
        if (step == st_.step) st_.step_fraction = 1;
    } else if (status == "fout") {
        st_.failed = true;
        st_.fail_kind = ev.str("soort", "bouw");
        st_.fail_msg = ev.str("msg");
    } else if (status == "einde") {
        st_.finished = true;
    }
}

void Pipeline::Recompute(double now_s) {
    st_.elapsed_s = now_s;
    // live re-estimate of the current step once 5 % is done
    std::vector<StepPlan> plan = plan_;
    double cur_elapsed = std::max(0.0, now_s - step_start_s_);
    for (auto& s : plan) {
        bool counted = st_.b >= 20 || st_.step == 3; // ignore tiny counters ("Re-running CMake" [1/1])
        if (s.step == st_.step && counted && st_.step_fraction >= 0.05 && cur_elapsed > 10) {
            double live = cur_elapsed / st_.step_fraction;
            s.expected_s = live;
        }
    }
    double frac = ((st_.step == 5 || st_.step == 7) && st_.b < 20) ? 0.0 : st_.step_fraction;
    st_.overall = st_.finished ? 1.0 : Overall(plan, st_.done_steps, st_.step, frac);
    double remaining = 0;
    for (auto& s : plan) {
        if (std::find(st_.done_steps.begin(), st_.done_steps.end(), s.step) != st_.done_steps.end()) continue;
        if (s.step == st_.step) remaining += std::max(0.0, s.expected_s - cur_elapsed);
        else if (s.step > st_.step) remaining += s.expected_s;
    }
    st_.eta_s = st_.finished ? 0 : remaining;
}

void Pipeline::ReadProgressFile() {
    std::ifstream f(opt_.progress_json, std::ios::binary);
    if (!f) return;
    f.seekg(0, std::ios::end);
    uint64_t size = uint64_t(f.tellg());
    if (size <= progress_off_) return;
    f.seekg(std::streamoff(progress_off_));
    std::string chunk(size_t(size - progress_off_), '\0');
    f.read(chunk.data(), std::streamsize(chunk.size()));
    progress_off_ = size;
    progress_partial_ += chunk;
    size_t nl;
    while ((nl = progress_partial_.find('\n')) != std::string::npos) {
        std::string line = Trim(progress_partial_.substr(0, nl));
        progress_partial_.erase(0, nl + 1);
        if (line.empty()) continue;
        std::string e;
        Json ev = Json::Parse(line, &e);
        if (e.empty() && ev.is_object()) ApplyEvent(ev);
    }
}

void Pipeline::ReadLogTail() {
    std::ifstream f(DiagnosticLog(), std::ios::binary);
    if (!f) return;
    f.seekg(0, std::ios::end);
    uint64_t size = uint64_t(f.tellg());
    if (size <= log_off_) return;
    uint64_t from = std::max(log_off_, size > 65536 ? size - 65536 : 0);
    f.seekg(std::streamoff(from));
    std::string chunk(size_t(size - from), '\0');
    f.read(chunk.data(), std::streamsize(chunk.size()));
    log_off_ = size;
    for (auto& l : SplitLines(chunk)) {
        if (l.empty()) continue;
        st_.log_tail.push_back(l);
        if (st_.log_tail.size() > 200) st_.log_tail.pop_front();
    }
}

bool Pipeline::Poll(ProcessRunner& runner) {
    if (!running_) return false;
    ReadProgressFile();
    ReadLogTail();
    double now = double(GetTickCount64() - start_tick_) / 1000.0;
    // step 3 (copy/extract): no counter; measure the growing temp folder against ~3.8 GB (every ~2 s)
    static uint64_t last_size_tick = 0;
    if (st_.step == 3 && !opt_.pointer_mode && !opt_.game_dir.empty() && GetTickCount64() - last_size_tick > 2000) {
        last_size_tick = GetTickCount64();
        fs::path tmp = opt_.game_dir;
        tmp += L".uitpakken";
        st_.step_fraction = std::min(0.99, double(DirSize(tmp)) / 3.80e9);
    }
    bool alive = runner.JobRunning();
    if (!alive) {
        ReadProgressFile();
        ReadLogTail();
        running_ = false;
        int code = runner.JobExitCode();
        if (code != 0 && !st_.failed) {
            st_.failed = true;
            st_.fail_kind = code == 1223 ? "geannuleerd" : "proces";
            st_.fail_msg = Format("Bouwscript gestopt met code %d.", code);
        }
        if (code == 0 && !st_.finished) {
            st_.failed = true;
            st_.fail_kind = "proces";
            st_.fail_msg = "Bouwscript stopte zonder 'einde'-melding.";
        }
        if (code == 0 && st_.finished) st_.failed = false;
    }
    Recompute(now);
    return running_;
}

void Pipeline::Cancel(ProcessRunner& runner) {
    if (!running_) return;
    runner.CancelJob();
    running_ = false;
    st_.failed = true;
    st_.fail_kind = "geannuleerd";
    st_.fail_msg = "Geannuleerd. Wat al klaar is, blijft bewaard; Setup gaat later verder.";
}

} // namespace sb
