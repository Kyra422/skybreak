// INSTALL19: headless installer flow (see headless.h).
#include "headless.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <thread>

namespace sb {

Choices ChoicesFromAnswers(Engine& e, const Json& a) {
    Choices c;
    c.lang = a.str("lang", e.Lang());
    c.accepted = a.boolean("accept", false);
    c.disc = P(a.str("disc"));
    c.install_dir = a.has("install_dir") ? Normal(P(a.str("install_dir"))) : e.DefaultInstallDir();
    c.data_dir = a.has("data_dir") ? Normal(P(a.str("data_dir"))) : e.DefaultDataDir();
    std::string mode = a.str("copy_mode", "copy");
    c.copy_folder = mode != "pointer";
    c.keep_build = a.boolean("keep_build", true);
    c.startmenu = a.boolean("startmenu", true);
    c.desktop = a.boolean("desktop", true);
    c.old_saves = a.str("old_saves", "copy");
    c.jobs = a.integer("jobs", 0);
    if (a["consent"].is_object()) c.consent = a["consent"];
    return c;
}

namespace {

// REL22: SkyBreak of this installation still running? Headless has nobody to ask, so it waits up to --wait-closed SECONDS
// (default 0) for the player to close it, then gives up with exit 7 and the list in the result ("running"). Never kills it.
bool StillRunning(Engine& e, const fs::path& dir, Json& j) {
    auto run = e.Running(dir);
    for (int waited = 0; !run.empty() && waited < e.opt().wait_closed_s; ++waited) {
        Sleep(1000);
        run = e.Running(dir);
    }
    if (run.empty()) return false;
    Json list = Json::Array();
    for (auto& p : run) list.push(Json(U(p.image)));
    j.set("running", list);
    j.set("error", e.RunningText(run));
    return true;
}

struct Result {
    Json j = Json::Object();
    fs::path file;
    int finish(int code) {
        j.set("exit", code);
        j.set("finished", NowIso());
        if (!file.empty()) WriteText(file, j.Dump(2) + "\n");
        Log("[headless] exit %d", code);
        return code;
    }
};

int Uninstall(Engine& e, Result& r) {
    fs::path dir = e.opt().install_dir;
    if (dir.empty()) {
        e.DetectExisting();
        dir = e.ExistingDir();
    }
    if (dir.empty()) {
        r.j.set("error", "no installation found");
        return r.finish(kExitError);
    }
    r.j.set("mode", "uninstall");
    r.j.set("install_dir", U(dir));
    if (StillRunning(e, dir, r.j)) return r.finish(kExitRunning); // REL22: nothing removed
    Engine::UninstallResult u = e.Uninstall(dir, e.opt().delete_saves, false);
    if (u.running) { // REL22: started in the meantime -> still nothing removed
        r.j.set("error", u.errors.empty() ? std::string() : u.errors[0]);
        return r.finish(kExitRunning);
    }
    Json rem = Json::Array(), kept = Json::Array(), errs = Json::Array();
    for (auto& s : u.removed) rem.push(Json(s));
    for (auto& s : u.kept_unknown) kept.push(Json(s));
    for (auto& s : u.errors) errs.push(Json(s));
    r.j.set("mode", "uninstall");
    r.j.set("install_dir", U(dir));
    r.j.set("removed", rem);
    r.j.set("kept_unknown", kept);
    r.j.set("errors", errs);
    r.j.set("saves_removed", u.saves_removed);
    return r.finish(u.ok ? kExitOk : kExitError);
}

} // namespace

int RunHeadless(Engine& e, const HeadlessIo& io) {
    Result r;
    r.file = io.result;
    r.j.set("started", NowIso());
    r.j.set("sandbox", e.plat().IsSandbox());
    r.j.set("mock_prereqs", e.prereqs().IsMock());
    r.j.set("package_version", e.PackageVersion());
    if (e.opt().uninstall) return Uninstall(e, r);

    std::string text;
    Json a = Json::Object();
    if (!e.opt().answers.empty()) {
        std::string err;
        if (!ReadText(e.opt().answers, text)) { r.j.set("error", "answers file not readable"); return r.finish(kExitError); }
        a = Json::Parse(text, &err);
        if (!err.empty()) { r.j.set("error", "answers: " + err); return r.finish(kExitError); }
    }

    // 1. package integrity
    if (e.opt().verify_package || !a.boolean("skip_package_check", false)) {
        std::vector<std::string> bad;
        std::string err;
        if (!e.VerifyPackage(&bad, &err)) {
            Json b = Json::Array();
            for (auto& s : bad) b.push(Json(s));
            r.j.set("package_bad", b);
            r.j.set("error", err);
            return r.finish(kExitPackage);
        }
        r.j.set("package_ok", true);
        if (e.opt().verify_package) return r.finish(kExitOk);
    }

    Choices c = ChoicesFromAnswers(e, a);
    e.SetLang(c.lang);
    if (!c.accepted) {
        r.j.set("error", "own-copy confirmation (accept) missing");
        return r.finish(kExitError);
    }
    r.j.set("install_dir", U(c.install_dir));
    r.j.set("data_dir", U(c.data_dir));

    // existing installation at the chosen folder -> update / repair / resume
    e.DetectExisting(c.install_dir); // INSTALL19 10 Oct: chosen folder first
    std::string mode = "install";
    if (!e.ExistingDir().empty() && Lower(U(e.ExistingDir())) == Lower(U(c.install_dir))) {
        mode = e.mode() == Mode::Update ? "update" : e.mode() == Mode::Resume ? "resume" : "repair";
        e.state() = e.Existing();
    }
    r.j.set("mode", mode);

    // 2. disc
    DiscProbe d = e.Probe(c.disc);
    r.j.set("disc", Json::Object());
    {
        Json dj = Json::Object();
        dj.set("kind", DiscKindName(d.kind));
        dj.set("verdict", DiscVerdictName(d.verdict));
        dj.set("game_id", d.game_id);
        dj.set("revision", d.revision);
        dj.set("message", e.Lang() == "en" ? d.msg_en : d.msg_nl);
        r.j.set("disc", dj);
    }
    if (!d.ok() && d.verdict != DiscVerdict::NeedsTool) return r.finish(kExitDisc);

    // 3. folders
    Json issues = Json::Array();
    bool blocked = false;
    for (auto& i : e.CheckInstallDir(c.install_dir)) {
        issues.push(Json((i.error ? "FOUT: " : "let op: ") + i.nl));
        blocked |= i.error;
    }
    for (auto& i : e.CheckDiscChoice(c, d)) { // REVINSTALL19 (continuation): pointer mode + non-ASCII disc folder
        issues.push(Json((i.error ? "FOUT: " : "let op: ") + i.nl));
        blocked |= i.error;
    }
    r.j.set("path_issues", issues);
    if (blocked) return r.finish(kExitError);

    // 4. prerequisites (consent per item from answers.consent)
    PrereqReport rep = e.Detect(c, d);
    Json pre = Json::Object(), installs = Json::Array();
    for (auto& it : rep.items) {
        Json pj = Json::Object();
        pj.set("present", it.present);
        pj.set("required", it.required);
        pj.set("path", it.path);
        pj.set("version", it.version);
        pre.set(it.id, pj);
    }
    r.j.set("prereqs", pre);
    for (auto& id : rep.MissingRequired()) {
        bool consent = c.consent.boolean(id, false);
        if (!consent && e.opt().mutate != "skip-consent") {
            r.j.set("error", "missing prerequisite without consent: " + id);
            r.j.set("missing", id);
            return r.finish(kExitPrereq);
        }
        DetectContext ctx;
        ctx.install_dir = c.install_dir;
        ctx.need_dolphintool = d.kind != DiscKind::Folder;
        InstallOutcome o = e.prereqs().Install(id, ctx, nullptr, nullptr);
        Json ij = Json::Object();
        ij.set("id", id);
        ij.set("route", o.route);
        ij.set("ok", o.ok);
        ij.set("code", o.code);
        ij.set("restart", o.restart_needed);
        ij.set("manual", o.manual);
        ij.set("message", o.msg_nl);
        ij.set("consent", consent);
        installs.push(ij);
        r.j.set("installs", installs);
        if (o.restart_needed) {
            e.state().set("phase", "herstart");
            e.state().set("restart_for", id);
            e.SaveState(c.install_dir);
            r.j.set("restart", true);
            return r.finish(kExitPrereq);
        }
    }
    rep = e.Detect(c, d);
    if (!rep.MissingRequired().empty()) {
        r.j.set("error", "prerequisite still missing: " + rep.MissingRequired().front());
        return r.finish(kExitPrereq);
    }

    // 5. space
    Json sp = Json::Array();
    bool red = false;
    for (auto& row : e.Space(c, d, rep)) {
        Json sj = Json::Object();
        sj.set("drive", row.drive);
        sj.set("need_gb", double(row.need) / 1073741824.0);
        sj.set("free_gb", double(row.free) / 1073741824.0);
        sp.push(sj);
        red |= row.red();
    }
    r.j.set("space", sp);
    if (red) return r.finish(kExitSpace);
    r.j.set("estimate", e.TimeEstimate(rep, c, d, false));

    // REL22: an update/repair/resume replaces SkyBreak.exe and bin\SkyBreak-game.exe: ask (here: wait/exit 7) BEFORE writing
    if (StillRunning(e, c.install_dir, r.j)) return r.finish(kExitRunning);

    // 6. sources + saves
    e.state().set("phase", "bezig");
    e.state().set("install_dir", U(c.install_dir));
    e.state().set("data_dir", U(c.data_dir));
    e.state().set("disc", U(d.path));
    e.SaveState(c.install_dir);
    std::string err;
    int copied = 0, unchanged = 0, removed = 0;
    if (!e.SyncSources(c.install_dir, &err, &copied, &unchanged, &removed)) {
        r.j.set("error", err);
        e.SetPhase(c.install_dir, "fout");
        return r.finish(kExitError);
    }
    Json sj = Json::Object();
    sj.set("copied", copied);
    sj.set("unchanged", unchanged);
    sj.set("removed", removed);
    r.j.set("sync", sj);
    std::string msg;
    if (!e.MigrateOldSaves(c, &msg)) { r.j.set("error", msg); return r.finish(kExitError); }
    if (!msg.empty()) r.j.set("saves_migration", msg);

    // 7. build
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    if (!e.StartBuild(c, d, rep, &err)) {
        r.j.set("error", err);
        e.SetPhase(c.install_dir, "fout");
        SetThreadExecutionState(ES_CONTINUOUS);
        return r.finish(kExitError);
    }
    r.j.set("build_args", U(ProcessRunner::CommandLine(L"powershell.exe", BuildArgs(e.pipeline().Options()))));
    int last_pct = -1, last_step = -1;
    bool cancelled = false;
    while (e.pipeline().Poll(e.runner())) {
        const PipelineState& ps = e.pipeline().State();
        int pct = int(ps.overall * 100);
        if (pct / 5 != last_pct / 5 || ps.step != last_step) {
            Log("[build] stap %d %s %d/%d  totaal %d%%  nog ca. %.0f s", ps.step, ps.step_name.c_str(), ps.a, ps.b, pct, ps.eta_s);
            last_pct = pct;
            last_step = ps.step;
        }
        if (e.opt().cancel_at_step && ps.step == e.opt().cancel_at_step && ps.a >= e.opt().cancel_at_a) {
            e.pipeline().Cancel(e.runner());
            cancelled = true;
            break;
        }
        Sleep(500);
    }
    SetThreadExecutionState(ES_CONTINUOUS);
    const PipelineState& ps = e.pipeline().State();
    Json bj = Json::Object();
    bj.set("step", ps.step);
    bj.set("a", ps.a);
    bj.set("b", ps.b);
    bj.set("seconds", ps.elapsed_s);
    bj.set("done_steps", Json::Array());
    Json ds = Json::Array();
    for (int s : ps.done_steps) ds.push(Json(s));
    bj.set("done_steps", ds);
    bj.set("peak_job_memory_mb", double(e.runner().PeakJobMemory() / (1024 * 1024)));
    bj.set("active_processes_after", int(e.runner().ActiveJobProcesses()));
    bj.set("log", U(e.pipeline().Options().log));
    {
        // ninja work of this run, from the newest bouw\logs\bouwen-*.log: the last "[a/b]" total and the compiled objects
        fs::path logs = e.pipeline().Options().work_dir / L"logs", newest;
        std::error_code ec;
        for (fs::directory_iterator it(logs, ec), end; !ec && it != end; it.increment(ec)) {
            std::wstring n = it->path().filename().wstring();
            if (n.rfind(L"bouwen-", 0) == 0 && (newest.empty() || n > newest.filename().wstring())) newest = it->path();
        }
        std::string text;
        int last_b = 0, compiled = 0;
        if (!newest.empty() && ReadText(newest, text)) {
            // only the last "Spel bouwen" invocation (the log is appended per run of the script)
            size_t from = text.rfind("--build");
            for (auto& l : SplitLines(from == std::string::npos ? text : text.substr(from))) {
                int a = 0, b = 0;
                if (l.size() > 3 && l[0] == '[' && sscanf(l.c_str(), "[%d/%d]", &a, &b) == 2) {
                    last_b = b;
                    if (l.find("Building C") != std::string::npos) ++compiled;
                }
            }
        }
        bj.set("ninja_log", U(newest));
        bj.set("ninja_total", last_b);
        bj.set("ninja_compiled", compiled);
    }
    r.j.set("build", bj);
    if (cancelled || ps.fail_kind == "geannuleerd") {
        e.SetPhase(c.install_dir, "geannuleerd");
        e.runner().CloseJob();
        return r.finish(kExitCancelled);
    }
    if (ps.failed || !ps.finished) {
        r.j.set("fail_kind", ps.fail_kind);
        r.j.set("error", ps.fail_msg);
        e.state().set("fail_kind", ps.fail_kind);
        e.SetPhase(c.install_dir, "fout");
        return r.finish(ps.fail_kind == "schijf" ? kExitDisc : ps.fail_kind == "ruimte" ? kExitSpace : kExitError);
    }
    e.runner().CloseJob();

    // 8. finish
    if (StillRunning(e, c.install_dir, r.j)) { // REL22: started during the build -> the built exe is in place, the rest waits
        e.SetPhase(c.install_dir, "fout");
        return r.finish(kExitRunning);
    }
    if (!e.Finish(c, d, &err)) {
        r.j.set("error", err);
        e.SetPhase(c.install_dir, "fout");
        return r.finish(kExitError);
    }
    r.j.set("phase", "klaar");
    return r.finish(kExitOk);
}

} // namespace sb
