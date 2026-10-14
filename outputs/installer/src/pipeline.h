#pragma once
// INSTALL19: runs outputs/release/Bouw-SkywardSword.ps1 (from <install>\bron) in a Job object and turns its
// JSON progress lines (-VoortgangJson) plus the ninja counter into a weighted overall progress and an ETA.
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "common.h"
#include "json.h"

namespace sb {

class ProcessRunner;

struct BuildOptions {
    fs::path script;        // ...\bron\outputs\release\Bouw-SkywardSword.ps1 (or a test double)
    fs::path disc;          // image file or extracted folder
    fs::path install_dir;   // -Uitvoer
    fs::path work_dir;      // -Werkmap (<install>\bouw)
    fs::path game_dir;      // -SpelMap (<install>\game); empty = pointer mode (the extracted folder is used in place)
    std::string nand_root;  // -NandRoot (toml form)
    std::string product;    // -Product
    std::string exe_name = "SkyBreak-game";
    int jobs = 0;
    fs::path python, cmake, ninja, dolphintool;
    fs::path bronnen;       // -Bronnen (<install>\bron\work\nwii-build\_deps)
    fs::path progress_json; // -VoortgangJson
    fs::path log;           // stdout/stderr of the script
    bool keep_build = true; // false -> -Opruimen
    bool pointer_mode = false;
};

std::vector<std::wstring> BuildArgs(const BuildOptions& o);

// Expected seconds per step (calibrated on PKG10: 690 ninja steps, 1348 s MSVC with 16 jobs).
struct StepPlan {
    int step;
    const char* name_nl;
    const char* name_en;
    double expected_s;
};
std::vector<StepPlan> PlanSteps(int jobs, bool pointer_mode, bool image, bool incremental);

struct PipelineState {
    int step = 0;              // current script step (1..9), 0 = not started
    std::string step_name;
    std::vector<int> done_steps;
    int a = 0, b = 0;          // ninja counter of the current step
    double step_fraction = 0;  // 0..1 within the current step
    double overall = 0;        // 0..1 weighted
    double elapsed_s = 0;
    double eta_s = -1;         // -1 unknown
    bool finished = false;     // 'einde' seen and process exited 0
    bool failed = false;
    std::string fail_kind;     // schijf / hulpmiddel / ruimte / bouw / pakket / proces
    std::string fail_msg;
    bool waiting = false;      // REL22: the script waits at step 8 until SkyBreak of this installation is closed ('wacht')
    std::string wait_msg;
    fs::path detail_log;       // active CMake/Ninja output, supplied by the build script
    std::deque<std::string> log_tail; // last lines of the script output
};

class Pipeline {
public:
    bool Start(ProcessRunner& runner, const BuildOptions& o, bool incremental, std::string* err);
    // Poll the progress file and the log; returns true while the process runs.
    bool Poll(ProcessRunner& runner);
    void Cancel(ProcessRunner& runner);
    const PipelineState& State() const { return st_; }
    const BuildOptions& Options() const { return opt_; }
    const std::vector<StepPlan>& Plan() const { return plan_; }
    bool Running() const { return running_; }
    fs::path DiagnosticLog() const { return st_.detail_log.empty() ? opt_.log : st_.detail_log; }

    // Pure progress model (unit-tested): apply one JSON line from -VoortgangJson.
    void ApplyEvent(const Json& ev);
    void Recompute(double now_s);
    // Pure helper: weighted overall fraction for (done steps, current step, fraction).
    static double Overall(const std::vector<StepPlan>& plan, const std::vector<int>& done, int cur, double frac);

    // For tests: set up the model without a process.
    void InitModel(const BuildOptions& o, bool incremental);
    void SetStateForSnapshot(const PipelineState& s) { st_ = s; }

private:
    void ReadProgressFile();
    void ReadLogTail();
    BuildOptions opt_;
    std::vector<StepPlan> plan_;
    PipelineState st_;
    bool running_ = false;
    uint64_t progress_off_ = 0, log_off_ = 0;
    std::string progress_partial_;
    uint64_t start_tick_ = 0;
    double step_start_s_ = 0;
    double ema_rate_ = 0;      // fraction per second within the current step (EMA)
    double last_frac_ = 0, last_frac_t_ = 0;
};

} // namespace sb
