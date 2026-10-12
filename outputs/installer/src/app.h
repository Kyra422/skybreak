#pragma once
// INSTALL19: the SkyBreak-Setup wizard (Dear ImGui). Same palette and widgets as the launcher: dark background,
// parchment text, gold focus outline; steps on the left, content on the right with a description pane; Back/Next
// at the bottom. Mouse, keyboard and controller (ImGui XInput navigation).
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "engine.h"

struct ImFont;

namespace sb {

struct UiFonts {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr;
    ImFont* tiny = nullptr;
};

class App {
public:
    enum Page { kWelcome, kDisc, kFolders, kPrereqs, kSummary, kBuild, kDone, kError, kModify, kUninstall, kUninstallDone, kPageCount };

    App(Engine& e, UiFonts fonts, void* logo_tex, int logo_w, int logo_h);
    void Frame(float width, float height); // draws one full-window frame
    bool WantsQuit() const { return quit_; }
    void StartUninstallMode();
    void StartModifyMode();

    // UI snapshots (--ui-snapshot): put the wizard in a representative state for page `p`.
    void SnapshotState(Page p);
    void SnapshotConsent(const std::string& id) { SnapshotState(kPrereqs); consent_id_ = id; consent_ticked_ = true; open_consent_ = true; }
    void SnapshotRunning(); // REL22: the "SkyBreak is still running" popup over the summary page
    static const char* PageId(Page p);

private:
    // pages
    void PageWelcome();
    void PageDisc();
    void PageFolders();
    void PagePrereqs();
    void PageSummary();
    void PageBuild();
    void PageDone();
    void PageError();
    void PageModify();
    void PageUninstall();
    void PageUninstallDone();
    void ConsentPopup();

    // chrome
    void DrawBackground(float w, float h);
    void DrawHeader(float w);
    void DrawSteps(float x, float y, float w, float h);
    void DrawFooter(float w, float h);
    bool NavButtons(bool can_back, bool can_next, const std::string& next_label);
    void Describe(const std::string& text); // description pane text for the item just drawn
    void StatusCard(bool ok, bool warn, const std::string& title, const std::string& body);
    std::string T(const char* nl, const char* en) const { return e_.tr(nl, en); }

    // actions
    void Go(Page p);
    void BeginProbe(const fs::path& p);
    void BeginDetect();
    void BeginInstallPrereq(const std::string& id);
    void BeginBuild();
    void FinishBuild();
    void CompleteFinish(); // REL22: the Finish part of FinishBuild (after the running check)
    void ApplyModify();    // REL22: the Apply button of the Modify page
    // REL22: SkyBreak of the installation still running -> "close it first" popup with Retry / Cancel; never killed.
    enum class Pending { None, Build, Finish, Modify, UninstallKeep, UninstallDelete };
    bool GuardRunning(Pending what, const fs::path& dir); // true = nothing runs: go on; false = the popup asks the player
    void RunPending(Pending what);
    void RunningPopup();
    Pending pending_ = Pending::None;
    fs::path pending_dir_;
    std::vector<RunningProgram> running_;
    uint64_t running_poll_ = 0;
    bool open_running_ = false;
    void PollWorkers();
    bool PickFile(fs::path& out, bool folder);

    Engine& e_;
    UiFonts f_;
    void* logo_ = nullptr;
    int logo_w_ = 0, logo_h_ = 0;
    Page page_ = kWelcome;
    bool quit_ = false;
    bool snapshot_ = false;

    Choices c_;
    std::string disc_text_;
    DiscProbe probe_;
    bool probe_valid_ = false;
    std::future<DiscProbe> probe_job_;
    PrereqReport rep_;
    bool rep_valid_ = false;
    std::future<PrereqReport> detect_job_;
    std::string consent_id_;
    bool consent_ticked_ = false;
    bool open_consent_ = false;
    std::future<InstallOutcome> install_job_;
    std::atomic<double> install_progress_{0};
    std::string install_msg_;
    std::vector<std::string> install_log_;
    std::string install_dir_text_, data_dir_text_;
    std::string desc_, page_desc_;
    std::string error_title_, error_body_, error_hint_;
    bool confirm_cancel_ = false;
    bool build_started_ = false;
    std::string done_msg_;
    bool del_saves_ = false, del_shots_ = false, confirm_saves_ = false;
    Engine::UninstallResult uninstall_res_;
    std::string modify_msg_;
    uint64_t last_poll_ = 0;
    bool focus_next_ = false;
    void* taskbar_ = nullptr;
    void* hwnd_ = nullptr;
public:
    void SetHwnd(void* h) { hwnd_ = h; }
    void OnDrop(const fs::path& p); // drag and drop onto the window
};

} // namespace sb
