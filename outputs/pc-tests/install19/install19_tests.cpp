// INSTALL19 unit tests for the SkyBreak-Setup core (links sb_core from work/install19/build).
// CPU only; reads at most a few KB of the user's disc images (header probe); writes only under work/install19/unit.
//   install19-tests.exe            -> every check PASS        (RESULT pass=N fail=0)
//   install19-tests.exe --mutate   -> negative controls: mutants that the checks must catch
//        D4 (revision ignored), R1 (sandbox allow-list off), S1 (leaky desktop audit), U1 (saves deleted by default),
//        P1 (progress stream without 'einde'), A2 (Dolphin archive pin not enforced), N1 (old data folder default),
//        C1 (REVINSTALL19: install into a foreign non-empty folder allowed)
//        REL22: C4 (refuse-non-ascii: the 0.19 rule), K1 K2 (ignore-running: a running SkyBreak is not noticed)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // REL22: CreateProcessW / GetShortPathNameW (K1-K3)
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common.h"
#include "disc_probe.h"
#include "engine.h"
#include "json.h"
#include "pipeline.h"
#include "platform.h"
#include "prereqs.h"
#include "proc.h"

using namespace sb;

static int g_pass = 0, g_fail = 0;
static bool g_mutate = false;
static fs::path g_root, g_work;

static void check(const char* id, bool ok, const std::string& what) {
    if (ok) { ++g_pass; printf("PASS: %s %s\n", id, what.c_str()); }
    else { ++g_fail; printf("FAIL: %s %s\n", id, what.c_str()); }
    fflush(stdout);
}

static void write_bytes(const fs::path& p, const std::vector<uint8_t>& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

static void put_header(std::vector<uint8_t>& b, size_t off, const char* id, int rev, bool wii = true) {
    if (b.size() < off + 0x100) b.resize(off + 0x100, 0);
    memcpy(&b[off], id, 6);
    b[off + 7] = uint8_t(rev);
    if (wii) { b[off + 0x18] = 0x5D; b[off + 0x19] = 0x1C; b[off + 0x1A] = 0x9E; b[off + 0x1B] = 0xA3; }
    const char* name = "SYNTHETIC TEST HEADER";
    memcpy(&b[off + 0x20], name, strlen(name));
}

static fs::path synth_iso(const char* name, const char* id, int rev) {
    std::vector<uint8_t> b(0x800, 0);
    put_header(b, 0, id, rev);
    fs::path p = g_work / name;
    write_bytes(p, b);
    return p;
}

// mutant used by --mutate: a probe that ignores the revision byte
static DiscProbe probe(const fs::path& p) {
    DiscProbe d = ProbeDisc(p);
    if (g_mutate && d.game_id == "SOUE01" && d.revision != 0) { d.revision = 0; ClassifyDisc(d); }
    return d;
}

static void test_json() {
    std::string err;
    Json j = Json::Parse(R"({"a":1,"b":[true,null,"x\u00e9"],"c":{"d":-2.5}})", &err);
    bool ok = err.empty() && j.integer("a") == 1 && j["b"].size() == 3 && j["b"].at(2).as_string() == "x\xC3\xA9" && j["c"].num("d") == -2.5;
    Json k = Json::Parse(j.Dump(2), &err);
    ok &= err.empty() && k.Dump() == j.Dump();
    Json bad = Json::Parse("{\"a\":", &err);
    ok &= !err.empty() && bad.is_null();
    check("J1", ok, "JSON parse/dump round trip, UTF-8 escape, error on truncated input");
    check("V1", CompareVersions("0.19.0", "0.18.9") > 0 && CompareVersions("0.19", "0.19.0") == 0 && CompareVersions("0.9.1", "0.19.0") < 0,
          "version compare");
    // INSTALL19 10 Oct: file hashes (SHA-256 pins, python.org's MD5) against the FIPS/RFC test vector "abc"
    fs::path abc = g_work / L"hash-abc.txt";
    write_bytes(abc, {'a', 'b', 'c'});
    std::string s = Sha256File(abc), m = Md5File(abc);
    check("H1", s == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" && m == "900150983cd24fb0d6963f7d28e17f72" &&
                    Sha256File(g_work / L"missing-file").empty(),
          "SHA-256 and MD5 of \"abc\" match the published test vectors; missing file -> empty");
    std::error_code ec;
    fs::remove(abc, ec);
}

static void test_disc() {
    DiscProbe d = probe(synth_iso("soue01.iso", "SOUE01", 0));
    check("D1", d.ok() && d.kind == DiscKind::Iso, "synthetic ISO SOUE01 rev 0 accepted (" + d.msg_nl + ")");
    d = probe(synth_iso("soup01.iso", "SOUP01", 0));
    check("D2", d.verdict == DiscVerdict::WrongRegion && d.msg_nl.find("Europese (PAL)") != std::string::npos &&
                    d.msg_nl.find("SOUE01, revisie 0") != std::string::npos,
          "PAL refused with the script's sentence: " + d.msg_nl);
    DiscProbe j = probe(synth_iso("souj01.iso", "SOUJ01", 0)), k = probe(synth_iso("souk01.iso", "SOUK01", 0));
    check("D3", j.verdict == DiscVerdict::WrongRegion && j.msg_nl.find("Japanse") != std::string::npos && k.verdict == DiscVerdict::WrongRegion &&
                    k.msg_nl.find("Koreaanse") != std::string::npos,
          "Japanese and Korean refused");
    d = probe(synth_iso("soue01-rev2.iso", "SOUE01", 2));
    check("D4", d.verdict == DiscVerdict::WrongRevision && d.msg_nl.find("revisie 2") != std::string::npos, "SOUE01 rev 2 refused: " + d.msg_nl);
    d = probe(synth_iso("rmce01.iso", "RMCE01", 0));
    check("D5", d.verdict == DiscVerdict::WrongGame && d.msg_nl.find("RMCE01") != std::string::npos, "other game refused: " + d.msg_nl);
    write_bytes(g_work / "empty.iso", {});
    write_bytes(g_work / "ten.iso", std::vector<uint8_t>(10, 0x41));
    std::vector<uint8_t> rnd(4096);
    uint32_t x = 12345;
    for (auto& c : rnd) { x = x * 1103515245 + 12345; c = uint8_t(x >> 16); }
    write_bytes(g_work / "random.bin", rnd);
    DiscProbe e1 = probe(g_work / "empty.iso"), e2 = probe(g_work / "ten.iso"), e3 = probe(g_work / "random.bin"), e4 = probe(g_work / "nope.iso");
    check("D6", e1.verdict == DiscVerdict::Unreadable && e2.verdict == DiscVerdict::Unreadable && e3.verdict == DiscVerdict::Unreadable &&
                    e4.verdict == DiscVerdict::Missing,
          "empty, 10-byte, random and missing files refused");
    // WBFS: hd sector shift 9 -> disc header copy at 0x200
    std::vector<uint8_t> w(0x400, 0);
    memcpy(&w[0], "WBFS", 4);
    w[8] = 9;
    put_header(w, 0x200, "SOUE01", 0);
    write_bytes(g_work / "ok.wbfs", w);
    std::vector<uint8_t> w2 = w;
    w2[8] = 10; // claims 0x400: header not there
    write_bytes(g_work / "badoff.wbfs", w2);
    DiscProbe wb = probe(g_work / "ok.wbfs"), wb2 = probe(g_work / "badoff.wbfs");
    check("D7", wb.ok() && wb.kind == DiscKind::Wbfs && wb2.verdict == DiscVerdict::Unreadable, "WBFS header at the hd-sector size; wrong offset refused");
    std::vector<uint8_t> r(0x200, 0);
    memcpy(&r[0], "RVZ\x01", 4);
    put_header(r, 0x58, "SOUE01", 0);
    write_bytes(g_work / "ok.rvz", r);
    std::vector<uint8_t> wi(0x200, 0);
    memcpy(&wi[0], "WIA\x01", 4);
    put_header(wi, 0x58, "SOUP01", 0);
    write_bytes(g_work / "pal.wia", wi);
    DiscProbe rv = probe(g_work / "ok.rvz"), wa = probe(g_work / "pal.wia");
    check("D8", rv.ok() && rv.kind == DiscKind::Rvz && wa.verdict == DiscVerdict::WrongRegion && wa.kind == DiscKind::Wia,
          "RVZ/WIA disc header at 0x58");
    // the user's own images (read-only, header only), when present
    fs::path docs = RealKnownFolder(Folder::UserProfile);
    fs::path usa = docs / L"Documents" / L"Skyward Sword Recomp" / L"Legend of Zelda, The - Skyward Sword (USA) (En,Fr,Es).rvz";
    fs::path eu = docs / L"Documents" / L"Skyward Sword Recomp" / L"Legend of Zelda, The - Skyward Sword (Europe) (En,Fr,De,Es,It).rvz";
    fs::path mkw = docs / L"Downloads" / L"Mario Kart Wii (USA) (EnFrEs)" / L"Mario Kart Wii (USA) (En,Fr,Es).rvz";
    fs::path tp = docs / L"Downloads" / L"Legend of Zelda The - Twilight Princess (USA)" / L"Legend of Zelda, The - Twilight Princess (USA).rvz";
    std::error_code ec;
    if (fs::exists(usa, ec) && fs::exists(eu, ec)) {
        DiscProbe a = probe(usa), b = probe(eu);
        bool other = true;
        std::string extra;
        for (auto& f : {mkw, tp})
            if (fs::exists(f, ec)) { DiscProbe o = probe(f); other &= o.verdict == DiscVerdict::WrongGame; extra += " " + o.game_id; }
        check("D9", a.ok() && a.game_id == "SOUE01" && b.verdict == DiscVerdict::WrongRegion && b.game_id == "SOUP01" && other,
              "user's rvz: USA " + a.game_id + " ok, Europe " + b.game_id + " refused, other games" + extra + " refused");
    } else {
        check("D9", true, "user's rvz files not present (skipped)");
    }
    // extracted folder: work/pc-game passes CheckDisc; a copy with one changed main.dol byte fails
    fs::path game = g_root / L"work" / L"pc-game";
    if (fs::exists(game / L"sys" / L"main.dol", ec)) {
        DiscProbe ok = probe(game);
        fs::path bad = g_work / L"badgame";
        fs::remove_all(bad, ec);
        fs::create_directories(bad / L"sys");
        fs::create_directories(bad / L"files");
        fs::copy_file(game / L"sys" / L"boot.bin", bad / L"sys" / L"boot.bin");
        fs::copy_file(game / L"sys" / L"main.dol", bad / L"sys" / L"main.dol");
        fs::copy_file(game / L"files" / L"rels.arc", bad / L"files" / L"rels.arc");
        {
            std::fstream f(bad / L"sys" / L"main.dol", std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(0x1000);
            char c = 0x5A;
            f.write(&c, 1);
        }
        DiscProbe nb = probe(bad);
        fs::remove_all(bad, ec); // the copy holds game data: removed again
        check("D10", ok.ok() && ok.kind == DiscKind::Folder && nb.verdict == DiscVerdict::BadFolder,
              "work/pc-game accepted (SHA-1), copy with one changed main.dol byte refused: " + nb.msg_nl.substr(0, 60));
    } else {
        check("D10", true, "work/pc-game not present (skipped)");
    }
}

static void test_pipeline() {
    Pipeline p;
    BuildOptions o;
    o.disc = g_work / "ok.rvz";
    o.jobs = 16;
    p.InitModel(o, false);
    std::vector<std::string> lines = {
        R"({"t":0.1,"stap":1,"naam":"1. Hulpprogramma's controleren","status":"start"})",
        R"({"t":2,"stap":1,"naam":"1","status":"klaar","msg":"Python 3.14"})",
        R"({"t":2,"stap":2,"naam":"2","status":"start"})",
        R"({"t":3,"stap":2,"naam":"2","status":"klaar"})",
        R"({"t":3,"stap":3,"naam":"3","status":"start"})", R"({"t":60,"stap":3,"naam":"3","status":"klaar"})",
        R"({"t":60,"stap":4,"naam":"4","status":"start"})", R"({"t":61,"stap":4,"naam":"4","status":"klaar"})",
        R"({"t":61,"stap":5,"naam":"5","status":"start"})", R"({"t":80,"stap":5,"naam":"5","status":"voortgang","a":20,"b":40})",
        R"({"t":95,"stap":5,"naam":"5","status":"klaar"})", R"({"t":95,"stap":6,"naam":"6","status":"start"})",
        R"({"t":105,"stap":6,"naam":"6","status":"klaar"})", R"({"t":105,"stap":7,"naam":"7","status":"start"})",
        R"({"t":107,"stap":7,"naam":"7","status":"voortgang","a":1,"b":1})", R"({"t":200,"stap":7,"naam":"7","status":"voortgang","a":100,"b":690})",
        R"({"t":700,"stap":7,"naam":"7","status":"voortgang","a":412,"b":690})", R"({"t":1300,"stap":7,"naam":"7","status":"klaar"})",
        R"({"t":1300,"stap":8,"naam":"8","status":"start"})", R"({"t":1301,"stap":8,"naam":"8","status":"klaar"})",
        R"({"t":1302,"stap":0,"naam":"","status":"einde"})"};
    if (g_mutate) lines.pop_back(); // mutant: the script never reports 'einde'
    double last = -1;
    bool mono = true;
    double at412 = 0;
    for (auto& l : lines) {
        p.ApplyEvent(Json::Parse(l));
        double t = Json::Parse(l).num("t");
        p.Recompute(t);
        const PipelineState& s = p.State();
        if (s.overall + 1e-9 < last) mono = false;
        last = s.overall;
        if (s.a == 412 && at412 == 0) at412 = s.overall;
    }
    const PipelineState& s = p.State();
    check("P1", mono && s.finished && s.overall == 1.0 && s.done_steps.size() == 8 && at412 > 0.5 && at412 < 0.9,
          Format("progress monotonic, 8 steps done, finished, overall at 412/690 = %.2f", at412));
    Pipeline f;
    f.InitModel(o, false);
    f.ApplyEvent(Json::Parse(R"({"t":1,"stap":2,"naam":"2","status":"start"})"));
    f.ApplyEvent(Json::Parse(R"({"t":2,"stap":2,"naam":"2","status":"fout","soort":"schijf","msg":"Dit is de Europese (PAL) versie"})"));
    check("P2", f.State().failed && f.State().fail_kind == "schijf", "a 'fout' line marks the run failed with its kind");
    {   // REL22 P4: the build script waits for a running SkyBreak ('wacht'), Setup shows it; any later event ('verder') clears it
        Pipeline w;
        w.InitModel(o, true);
        w.ApplyEvent(Json::Parse(R"({"t":1,"stap":8,"naam":"8","status":"start"})"));
        w.ApplyEvent(Json::Parse(R"J({"t":2,"stap":8,"naam":"8","status":"wacht","msg":"C:\\S\\bin\\SkyBreak-game.exe (pid 7)"})J"));
        bool waiting = w.State().waiting && w.State().wait_msg.find("SkyBreak-game.exe") != std::string::npos && !w.State().failed;
        w.ApplyEvent(Json::Parse(R"({"t":9,"stap":8,"naam":"8","status":"verder"})"));
        bool cleared = !w.State().waiting;
        w.ApplyEvent(Json::Parse(R"({"t":10,"stap":8,"naam":"8","status":"fout","soort":"draait","msg":"SkyBreak draait nog"})"));
        check("P4", waiting && cleared && w.State().failed && w.State().fail_kind == "draait",
              Format("'wacht' shown (%d), cleared by 'verder' (%d), timeout = fout soort 'draait' (%s)", int(waiting), int(cleared),
                     w.State().fail_kind.c_str()));
    }
    BuildOptions b;
    b.script = L"C:\\x\\Bouw-SkywardSword.ps1";
    b.disc = L"C:\\games\\pc-game";
    b.install_dir = L"C:\\SkyBreak";
    b.work_dir = L"C:\\SkyBreak\\bouw";
    b.pointer_mode = true;
    b.nand_root = "%APPDATA%/SkywardSwordPC/nand";
    b.product = "SkyBreak 0.19.0";
    b.progress_json = L"C:\\SkyBreak\\setup\\voortgang.jsonl";
    b.bronnen = L"C:\\SkyBreak\\bron\\work\\nwii-build\\_deps";
    b.jobs = 12;
    std::wstring cl = ProcessRunner::CommandLine(L"powershell.exe", BuildArgs(b));
    std::string c = U(cl);
    bool args_ok = c.find("-VoortgangJson") != std::string::npos && c.find("-ExeNaam SkyBreak-game") != std::string::npos &&
                   c.find("-NandRoot %APPDATA%/SkywardSwordPC/nand") != std::string::npos && c.find("-Bronnen") != std::string::npos &&
                   c.find("-SpelMap") == std::string::npos && c.find("-Product \"SkyBreak 0.19.0\"") != std::string::npos &&
                   c.find("-ExecutionPolicy Bypass") != std::string::npos && c.find("-Taken 12") != std::string::npos;
    b.pointer_mode = false;
    b.game_dir = L"C:\\SkyBreak\\game";
    std::string c2 = U(ProcessRunner::CommandLine(L"powershell.exe", BuildArgs(b)));
    check("P3", args_ok && c2.find("-SpelMap C:\\SkyBreak\\game") != std::string::npos, "build script arguments (pointer and copy mode)");
}

static void test_proc_platform() {
    ProcessRunner r;
    r.SetSandbox(!g_mutate); // mutant: allow-list switched off in a sandbox run
    std::string why;
    bool refused = !r.Allowed(L"C:\\x\\winget.exe", &why) && !r.Allowed(L"C:\\x\\vs_BuildTools.exe", nullptr) &&
                   !r.Allowed(L"C:\\Windows\\System32\\msiexec.exe", nullptr) && !r.Allowed(L"C:\\x\\random-tool.exe", nullptr) &&
                   r.Allowed(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", nullptr);
    // a path that does not exist: even the mutant (allow-list off) cannot start anything here
    ProcResult pr = r.Run(L"C:\\nonexistent-install19\\winget.exe", {L"--version"}, 5000);
    bool audited = !pr.started && !r.AuditLines().empty() && r.AuditLines().back().find("REFUSED") != std::string::npos;
    check("R1", refused && audited, "sandbox refuses winget/VS installer/msiexec/unknown exes (audited), allows powershell");
    ProcessRunner real;
    check("R2", real.Allowed(L"C:\\x\\winget.exe", nullptr), "real mode: the allow-list does not apply (consent page gates installs)");

    fs::path sbx = g_work / L"sandbox-platform";
    std::error_code ec;
    fs::remove_all(sbx, ec);
    auto p = MakeSandboxPlatform(sbx);
    if (g_mutate) p->mutate = "leak-desktop";
    bool inside = true;
    for (Folder f : {Folder::Desktop, Folder::StartMenuPrograms, Folder::RoamingAppData, Folder::LocalAppData, Folder::Temp})
        inside &= IsInside(p->KnownFolder(f), sbx);
    Json reg = Json::Object();
    reg.set("DisplayName", "SkyBreak");
    reg.set("EstimatedSize", 123);
    bool w = p->WriteUninstallEntry(reg);
    Json back;
    bool rd = p->ReadUninstallEntry(back) && back.str("DisplayName") == "SkyBreak" && back.integer("EstimatedSize") == 123;
    bool del = p->DeleteUninstallEntry() && !p->ReadUninstallEntry(back);
    std::string err;
    fs::path lnk = p->KnownFolder(Folder::Desktop) / L"SkyBreak.lnk";
    bool sc = p->CreateShortcut(lnk, sbx / L"SkyBreak.exe", sbx, sbx / L"skybreak.ico", "test", &err) && fs::is_regular_file(lnk, ec);
    bool audit_sandbox = true;
    for (auto& l : p->AuditLines()) audit_sandbox &= EndsWith(l, "|sandbox");
    check("S1", inside && w && rd && del && sc && audit_sandbox, "sandbox: known folders inside, registry.json write/read/delete, .lnk, audit all 'sandbox'");
}

static void test_mock_prereqs() {
    Json m = Json::Parse(R"({"present":["python"],"winget":false,"tar_lzma":false,
        "actions":{"vs":{"exit":3010,"seconds":0.1},"dolphintool":{"exit":0},"python":{"exit":0,"sha256_ok":false}}})");
    auto env = MakeMockPrereqs(m, Json::Array());
    DetectContext ctx;
    ctx.need_dolphintool = true;
    PrereqReport r = env->Detect(ctx);
    bool det = r.get("python") && r.get("python")->present && r.get("vs") && !r.get("vs")->present && r.MissingRequired().size() == 2;
    InstallOutcome vs = env->Install("vs", ctx, nullptr, nullptr);
    InstallOutcome dt = env->Install("dolphintool", ctx, nullptr, nullptr);
    InstallOutcome py = env->Install("python", ctx, nullptr, nullptr);
    check("M1", det && vs.ok && vs.restart_needed && vs.route == "bootstrapper" && dt.manual && dt.route == "browser" && !dt.ok && !py.ok,
          "mock: detection, VS 3010 restart via bootstrapper (no winget), DolphinTool browser route (tar without liblzma), hash mismatch refused");
    SystemInfo si;
    si.cores = 32;
    si.ram_gb = 15.9;
    check("M2", SuggestedJobs(si) == 10 && [] { SystemInfo a; a.cores = 4; a.ram_gb = 64; return SuggestedJobs(a) == 2; }(),
          "jobs = min(cores-2, RAM/1.5)");
}

// INSTALL19 (2026-10-10): the DolphinTool 7z route after the download (pin check + tar extraction), fed with the
// player's own archive %USERPROFILE%\Downloads\dolphin-2606a-x64.7z (read only; skipped when absent). Nothing is downloaded
// and DolphinTool.exe is never started; the extracted copy under work/install19/unit is removed again.
static void test_dolphin_archive() {
    Json items = LoadPrereqManifest(g_root / L"outputs" / L"installer" / L"prereqs.json");
    Json dt;
    for (const Json& it : items.elements())
        if (it.str("id") == "dolphintool") dt = it;
    std::string pin = dt.str("sha256");
    std::vector<std::string> entries = dt["extract"].strings();
    const wchar_t* sysroot = _wgetenv(L"SystemRoot");
    fs::path tar = fs::path(sysroot ? sysroot : L"C:\\Windows") / L"System32" / L"tar.exe";
    const wchar_t* up = _wgetenv(L"USERPROFILE");
    fs::path arc = fs::path(up ? up : L"C:\\") / L"Downloads" / L"dolphin-2606a-x64.7z";
    fs::path tmp = g_work / L"dolphin-uitpakken", dir = g_work / L"dolphin";
    ProcessRunner run;
    run.SetSandbox(true); // tar is on the sandbox allow-list
    std::error_code ec;
    bool have = fs::is_regular_file(arc, ec);
    if (have) {
        fs::remove_all(dir, ec);
        std::string err;
        bool ok = ExtractPinnedArchive(run, tar, arc, pin, entries, tmp, dir, &err);
        bool lic = fs::is_directory(dir / L"LICENSES", ec) && !fs::is_empty(dir / L"LICENSES", ec);
        bool size_ok = double(fs::file_size(arc, ec)) == dt.num("size_bytes", 0);
        check("A1", ok && size_ok && fs::is_regular_file(dir / L"DolphinTool.exe", ec) && fs::is_regular_file(dir / L"COPYING", ec) && lic &&
                        !fs::exists(tmp, ec) && dt.str("url") == "https://dl.dolphin-emu.org/releases/2606a/dolphin-2606a-x64.7z" && !dt.str("page_url").empty(),
              "pinned Dolphin 2606a x64 7z: size + SHA-256 match the player's own archive, DolphinTool.exe + COPYING + LICENSES\\ extracted, "
              "temp folder gone; verified official URL configured for automatic download " + err);
        fs::remove_all(dir, ec);
    } else {
        check("A1", true, "Downloads\\dolphin-2606a-x64.7z not present (skipped)");
    }
    // negative: a wrong pin (one hex digit changed) and an empty pin extract nothing
    fs::path bad = arc;
    if (!have) { bad = g_work / L"not-a-dolphin.7z"; write_bytes(bad, {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C, 0, 4}); }
    std::string wrong = have ? pin : Sha256File(bad);
    if (!wrong.empty()) wrong[0] = wrong[0] == '0' ? '1' : '0';
    if (g_mutate) wrong = Sha256File(bad); // mutant: a checker that trusts whatever arrived
    std::string e1, e2;
    fs::remove_all(dir, ec);
    bool r1 = ExtractPinnedArchive(run, tar, bad, wrong, entries, tmp, dir, &e1);
    bool r2 = ExtractPinnedArchive(run, tar, bad, "", entries, tmp, dir, &e2);
    bool nothing = !fs::exists(dir / L"DolphinTool.exe", ec) && !fs::exists(tmp, ec);
    check("A2", !r1 && !r2 && e1.find("SHA-256") != std::string::npos && e2.find("SHA-256") != std::string::npos && nothing,
          "wrong / empty SHA-256 pin: refused, nothing extracted (" + e1 + "; " + e2 + ")");
    fs::remove_all(dir, ec);
    fs::remove_all(tmp, ec);
    if (!have) fs::remove(bad, ec);
}

// INSTALL19 (2026-10-10) user decision: the save folder stays %APPDATA%\SkywardSwordPC\nand (no move, no copy).
static void test_data_dir_default() {
    fs::path sbx = g_work / L"sandbox-datadir";
    std::error_code ec;
    fs::remove_all(sbx, ec);
    Options o;
    o.sandbox = sbx;
    o.headless = true;
    Engine e(o);
    fs::path roaming = e.plat().KnownFolder(Folder::RoamingAppData);
    WriteText(e.OldSavesDir() / L"title" / L"wiiking2.sav", "old save");
    Choices c;
    c.data_dir = g_mutate ? roaming / L"SkyBreak" : e.DefaultDataDir(); // mutant: the old default %APPDATA%\SkyBreak
    c.old_saves = "copy";
    std::string msg;
    bool same = Normal(e.DefaultDataDir()) == Normal(roaming / L"SkywardSwordPC") && Normal(c.data_dir / L"nand") == Normal(e.OldSavesDir());
    bool migrated = e.MigrateOldSaves(c, &msg);
    bool nothing_new = !fs::exists(roaming / L"SkyBreak", ec) && fs::is_regular_file(e.OldSavesDir() / L"title" / L"wiiking2.sav", ec);
    bool toml_ok = e.NandRootToml(c) == Replace(U(Normal(e.OldSavesDir())), "\\", "/");
    bool no_choice = !e.OldSavesElsewhere(c);
    // another folder chosen: the copy offer appears and copies (the old folder stays)
    Choices c2 = c;
    c2.data_dir = sbx / L"Elders (x86), saves";
    bool other = e.OldSavesElsewhere(c2) && e.MigrateOldSaves(c2, &msg) && fs::is_regular_file(c2.data_dir / L"nand" / L"title" / L"wiiking2.sav", ec) &&
                 fs::is_regular_file(e.OldSavesDir() / L"title" / L"wiiking2.sav", ec);
    // real mode (read only: no file is written): the toml gets the script's own default string
    Options ro;
    ro.headless = true;
    Engine real(ro);
    Choices c3;
    c3.data_dir = real.DefaultDataDir();
    bool real_toml = real.NandRootToml(c3) == "%APPDATA%/SkywardSwordPC/nand";
    check("N1", same && migrated && nothing_new && toml_ok && no_choice && other && real_toml,
          "default data folder = %APPDATA%\\SkywardSwordPC (saves stay in its nand, nothing copied or moved, no copy offer); "
          "[nand] root = %APPDATA%/SkywardSwordPC/nand; another folder -> copy offer, old folder kept");
    fs::remove_all(sbx, ec);
}

// REVINSTALL19 (2026-10-10 review): install folder with the player's own content + space rules.
//   C1 a non-empty folder that is no SkyBreak install is refused ('niet leeg'); empty, missing, a SkyBreak install and only
//      Setup's own pre-state leftovers (setup\downloads, hulpmiddelen\python) are allowed. Mutant: the check switched off.
//   C2 --mock-free-gb works only in a sandbox; an install whose game copy already exists needs 3.8 GB less (update/repair).
//   C3 (continuation) a leftover game.uitpakken counts as free on a resume; path length in characters. Mutant: old-space-rules.
//   C4 (continuation) non-ASCII install folder refused (build step 6 fails on it), ASCII with spaces/parentheses allowed; pointer
//      mode to a non-ASCII disc folder refused, copy mode of that folder allowed. Mutant: allow-non-ascii.
static void test_install_dir_and_space() {
    fs::path sbx = g_work / L"sandbox-installdir";
    std::error_code ec;
    fs::remove_all(sbx, ec);
    Options o;
    o.sandbox = sbx;
    o.headless = true;
    if (g_mutate) o.mutate = "allow-foreign-dir"; // mutant: a Setup that installs into anybody's folder
    Engine e(o);
    auto refused = [&](const fs::path& d) {
        for (auto& i : e.CheckInstallDir(d))
            if (i.error && i.nl.find("niet leeg") != std::string::npos) return true;
        return false;
    };
    fs::path foreign = sbx / L"Spellen (eigen), D", empty = sbx / L"leeg", ours = sbx / L"SkyBreak-hervat", left = sbx / L"SkyBreak-na-download";
    WriteText(foreign / L"bin" / L"mijn-tool.txt", "van de speler");
    fs::create_directories(empty, ec);
    WriteText(ours / L"setup" / L"install.json", "{\"phase\": \"geannuleerd\"}");
    WriteText(ours / L"bin" / L"x.txt", "x");
    WriteText(left / L"setup" / L"downloads" / L"python-embed.zip", "z");
    WriteText(left / L"hulpmiddelen" / L"python" / L"python.exe", "p");
    bool c1 = refused(foreign) && !refused(empty) && !refused(sbx / L"bestaat-niet") && !refused(ours) && !refused(left);
    check("C1", c1, "install folder: foreign non-empty refused; empty / missing / SkyBreak install / Setup's own download leftovers allowed");

    Options om = o;
    om.mutate.clear();
    om.mock_free_gb = 1.0;
    Engine es(om);
    DiscProbe d;
    d.kind = DiscKind::Iso; // copy mode (extracted into <install>\game)
    Choices c;
    c.install_dir = sbx / L"SkyBreak-ruimte";
    PrereqReport rep;
    auto rows = es.Space(c, d, rep);
    bool mocked = rows.size() == 1 && rows[0].free == 1024ull * 1024 * 1024 && rows[0].red();
    uint64_t need_new = rows.empty() ? 0 : rows[0].need;
    WriteText(c.install_dir / L"game" / L"sys" / L"main.dol", "stand-in");
    auto rows2 = es.Space(c, d, rep);
    bool less = !rows2.empty() && need_new - rows2[0].need == uint64_t(3.8 * (1024ull * 1024 * 1024)); // same expression as Engine::Space
    Options real;
    real.headless = true;
    real.mock_free_gb = 1.0; // no --sandbox: must be ignored
    Engine er(real);
    Choices cr;
    cr.install_dir = g_work;
    auto rr = er.Space(cr, d, rep);
    bool ignored = !rr.empty() && rr[0].free != 1024ull * 1024 * 1024 && rr[0].free > 0;
    check("C2", mocked && less && ignored, "space: --mock-free-gb 1 -> red in a sandbox, ignored without one; existing game copy -> 3.8 GB less needed");

    // C3 (REVINSTALL19 continuation): a leftover <install>\game.uitpakken of a cancelled step 3 counts as free space on a resume
    // (the build script deletes it first), and the install path length counts characters (UTF-16), not UTF-8 bytes.
    // Mutant old-space-rules: leftover ignored + UTF-8 byte count -> C3 FAIL.
    Options o3 = om;
    o3.mutate = g_mutate ? "old-space-rules" : "";
    Engine e3(o3);
    Choices c3;
    c3.install_dir = sbx / L"SkyBreak-hervat-zonder-rest";
    uint64_t need_plain = e3.Space(c3, d, rep)[0].need;
    Choices c4;
    c4.install_dir = sbx / L"SkyBreak-hervat-met-rest";
    WriteText(c4.install_dir / L"game.uitpakken" / L"files" / L"rest.bin", std::string(1 << 20, 'x'));
    uint64_t need_left = e3.Space(c4, d, rep)[0].need;
    bool reclaimed = need_plain - need_left == (1u << 20);
    fs::path cjk = sbx / std::wstring(20, L'\x65E5'); // 20 CJK characters: < 150 characters, > 150 UTF-8 bytes
    bool too_long = false;
    for (auto& i : e3.CheckInstallDir(cjk))
        if (i.error && i.nl.find("te lang") != std::string::npos) too_long = true;
    bool fits = Normal(cjk).wstring().size() <= 150 && U(Normal(cjk)).size() > 150;
    check("C3", reclaimed && !too_long && fits,
          Format("resume: leftover game.uitpakken (1 MiB) counted as free %d; %zu-character CJK path accepted %d (UTF-8 %zu bytes)",
                 int(reclaimed), Normal(cjk).wstring().size(), int(!too_long), U(Normal(cjk)).size()));

    // C4 (REVINSTALL19 continuation; REL22: the UTF-8 activeCodePage manifests make non-ASCII folders work -> ALLOWED on
    // Windows 10 1903+, still refused on older Windows (mutant old-windows, always checked). NEGATIVE: refuse-non-ascii (0.19 rule).
    Options o4 = om;
    o4.mutate = g_mutate ? "refuse-non-ascii" : "";
    Engine e4(o4);
    Options o4old = om;
    o4old.mutate = "old-windows";
    Engine e4old(o4old);
    auto has_error = [](const std::vector<PathIssue>& v) {
        for (auto& i : v) if (i.error && i.nl.find("bijzondere tekens") != std::string::npos) return true;
        return false;
    };
    fs::path zeld = sbx / L"Spellen" / L"Z\x00E9ld\x00E0 \x2726 test";
    bool accent = !has_error(e4.CheckInstallDir(sbx / L"Zo\x00EB" / L"SkyBreak"));
    bool cjk_ok = !has_error(e4.CheckInstallDir(sbx / L"Spellen \x65E5" / L"SkyBreak"));
    bool zeld_ok = !has_error(e4.CheckInstallDir(zeld));
    bool ascii_ok = !has_error(e4.CheckInstallDir(sbx / L"Spellen (x86), D" / L"SkyBreak"));
    DiscProbe pd;
    pd.kind = DiscKind::Folder;
    pd.game_root = sbx / L"Schijf Zo\x00EB";
    Choices cp;
    cp.copy_folder = false;
    bool pointer_ok = !has_error(e4.CheckDiscChoice(cp, pd));
    bool old_refused = has_error(e4old.CheckDiscChoice(cp, pd)) && has_error(e4old.CheckInstallDir(zeld));
    cp.copy_folder = true;
    bool copy_ok = !has_error(e4.CheckDiscChoice(cp, pd));
    pd.game_root = sbx / L"Schijf (USA), kopie";
    cp.copy_folder = false;
    bool pointer_ascii_ok = !has_error(e4.CheckDiscChoice(cp, pd)) && !has_error(e4old.CheckDiscChoice(cp, pd));
    check("C4", accent && cjk_ok && zeld_ok && ascii_ok && pointer_ok && copy_ok && pointer_ascii_ok && old_refused && NonAsciiPathsWork(""),
          Format("REL22 non-ASCII install folder allowed (Windows 10 1903+): accent %d, CJK %d, 'Z\xC3\xA9ld\xC3\xA0 \xE2\x9C\xA6 test' %d; ASCII %d; "
                 "pointer to a non-ASCII disc folder %d, copy %d, pointer to ASCII %d; refused on older Windows %d; this Windows >= 1903 %d",
                 int(accent), int(cjk_ok), int(zeld_ok), int(ascii_ok), int(pointer_ok), int(copy_ok), int(pointer_ascii_ok), int(old_refused),
                 int(NonAsciiPathsWork(""))));
    fs::remove_all(sbx, ec);
}

static void test_uninstall() {
    fs::path sbx = g_work / L"sandbox-uninstall";
    std::error_code ec;
    fs::remove_all(sbx, ec);
    Options o;
    o.sandbox = sbx;
    o.headless = true;
    Engine e(o);
    fs::path inst = e.DefaultInstallDir(), data = e.DefaultDataDir(), pointer_game = sbx / L"mijn-uitgepakte-schijf";
    for (auto* d : {L"bin", L"bron", L"bouw", L"setup", L"hulp", L"logs"}) fs::create_directories(inst / d);
    WriteText(inst / L"bin" / L"SkyBreak-game.exe", "x");
    WriteText(inst / L"SkyBreak.exe", "x");
    WriteText(inst / L"instellingen.toml", "[nand]\n");
    WriteText(inst / L"mijn-eigen-notities.txt", "unknown file: must stay");
    WriteText(data / L"nand" / L"title" / L"wiiking2.sav", "sentinel");
    WriteText(pointer_game / L"sys" / L"main.dol", "pointer-mode game folder: must stay");
    Json st = Json::Object();
    Json created = Json::Array();
    for (auto* n : {"SkyBreak.exe", "bin", "bron", "bouw", "hulp", "logs", "setup", "instellingen.toml"}) created.push(Json(n));
    st.set("created", created);
    st.set("pointer_mode", true);
    st.set("game_dir", U(pointer_game));
    st.set("nand_dir", U(data / L"nand"));
    fs::path lnk = e.plat().KnownFolder(Folder::Desktop) / L"SkyBreak.lnk";
    std::string err;
    e.plat().CreateShortcut(lnk, inst / L"SkyBreak.exe", inst, {}, "t", &err);
    Json lnks = Json::Array();
    lnks.push(Json(U(lnk)));
    st.set("shortcuts", lnks);
    WriteText(inst / L"setup" / L"install.json", st.Dump(2));
    Json reg = Json::Object();
    reg.set("InstallLocation", U(inst));
    e.plat().WriteUninstallEntry(reg);
    Engine::UninstallResult u = e.Uninstall(inst, g_mutate /* mutant: saves deleted by default */, false);
    Json back;
    bool ok = u.ok && !fs::exists(inst / L"bin", ec) && !fs::exists(inst / L"setup", ec) && fs::exists(inst / L"mijn-eigen-notities.txt", ec) &&
              u.kept_unknown.size() == 1 && fs::exists(data / L"nand" / L"title" / L"wiiking2.sav", ec) && !u.saves_removed &&
              fs::exists(pointer_game / L"sys" / L"main.dol", ec) && !fs::exists(lnk, ec) && !e.plat().ReadUninstallEntry(back);
    check("U1", ok, "uninstall: created entries, shortcut and Apps entry gone; unknown file, saves and pointer-mode game folder kept");
    // second run on a fresh copy: --delete-saves removes the saves (and only them)
    fs::create_directories(inst / L"setup");
    WriteText(inst / L"setup" / L"install.json", st.Dump(2));
    Engine::UninstallResult u2 = e.Uninstall(inst, true, false);
    check("U2", u2.saves_removed && !fs::exists(data / L"nand", ec) && fs::exists(pointer_game / L"sys" / L"main.dol", ec),
          "uninstall --delete-saves: saves gone, pointer-mode game folder still there");
    fs::remove_all(sbx, ec);
}

// REL22 K1-K3: a running SkyBreak of THIS installation (start program or game exe) is found by full path; a copy with the same
// names in ANOTHER installation is not. Uninstall and Finish then change nothing. The test double (dummy-skybreak.exe, built by
// build-install19-tests.cmd) only sleeps; it is stopped through its .stop file, never killed. NEGATIVE: ignore-running fails K1 K2.
static PROCESS_INFORMATION start_dummy(const fs::path& exe) {
    PROCESS_INFORMATION pi{};
    STARTUPINFOW si{};
    si.cb = sizeof si;
    std::wstring cmd = L"\"" + exe.wstring() + L"\" 60";
    std::error_code ec;
    fs::remove(fs::path(exe.wstring() + L".stop"), ec);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) pi = {};
    return pi;
}

static void stop_dummy(PROCESS_INFORMATION& pi, const fs::path& exe) {
    if (!pi.hProcess) return;
    WriteText(fs::path(exe.wstring() + L".stop"), "stop");
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    pi = {};
    std::error_code ec;
    fs::remove(fs::path(exe.wstring() + L".stop"), ec);
}

static void test_running() {
    std::error_code ec;
    fs::path dummy = g_root / L"work" / L"install19" / L"unit-build" / L"dummy-skybreak.exe";
    fs::path sbx = g_work / L"running";
    fs::remove_all(sbx, ec);
    fs::path inst = sbx / L"Spellen" / L"Z\x00E9ld\x00E0 \x2726 test" / L"SkyBreak", other = sbx / L"andere installatie" / L"SkyBreak";
    fs::path a = inst / L"SkyBreak.exe", b = inst / L"bin" / L"SkyBreak-game.exe", c = other / L"bin" / L"SkyBreak-game.exe";
    for (auto& p : {a, b, c}) {
        fs::create_directories(p.parent_path(), ec);
        fs::copy_file(dummy, p, fs::copy_options::overwrite_existing, ec);
    }
    Json st = Json::Object();
    Json created = Json::Array();
    for (const char* n : {"SkyBreak.exe", "bin", "setup"}) created.push(Json(n));
    st.set("created", created);
    st.set("shortcuts", Json::Array());
    WriteText(inst / L"setup" / L"install.json", st.Dump(2));

    Options o;
    o.sandbox = sbx / L"box";
    o.package_dir = g_root / L"outputs" / L"installer";
    o.mutate = g_mutate ? "ignore-running" : "";
    Engine e(o);
    PROCESS_INFORMATION pa = start_dummy(a), pb = start_dummy(b), pc = start_dummy(c);
    Sleep(300);
    auto r = e.Running(inst);
    auto ro = e.Running(other);
    bool ab = r.size() == 2, only_other = ro.size() == 1;
    for (auto& x : r) ab = ab && (x.image.filename() == L"SkyBreak.exe" || x.image.filename() == L"SkyBreak-game.exe") && IsInside(x.image, inst);
    for (auto& x : ro) only_other = only_other && IsInside(x.image, other);
    // the same folder under its 8.3 short name (when the volume has short names) is the same installation
    wchar_t shortbuf[MAX_PATH];
    DWORD sn = GetShortPathNameW(inst.c_str(), shortbuf, MAX_PATH);
    bool short_ok = true;
    std::string short_note = "no 8.3 names on this volume";
    if (sn && sn < MAX_PATH && fs::path(shortbuf) != inst) {
        short_ok = e.Running(fs::path(shortbuf)).size() == 2;
        short_note = "8.3 path " + U(fs::path(shortbuf));
    }
    check("K1", pa.hProcess && pb.hProcess && pc.hProcess && ab && only_other && short_ok,
          Format("running start program + game exe of 'Z\xC3\xA9ld\xC3\xA0 \xE2\x9C\xA6 test' found by full path: %zu (want 2); the same names in "
                 "another installation: %zu of its own, not counted here; %s ok %d", r.size(), ro.size(), short_note.c_str(), int(short_ok)));

    Engine::UninstallResult u = e.Uninstall(inst, false, false);
    std::string ferr;
    Choices ch;
    ch.install_dir = inst;
    bool fin = e.Finish(ch, DiscProbe{}, &ferr);
    bool intact = fs::exists(a, ec) && fs::exists(b, ec) && fs::exists(inst / L"setup" / L"install.json", ec);
    check("K2", u.running && !u.ok && u.removed.empty() && !fin && ferr.find("SkyBreak is still running") != std::string::npos && intact,
          Format("while it runs: uninstall refused (running %d, removed %zu), finish refused (%d), folder intact %d - nothing killed",
                 int(u.running), u.removed.size(), int(!fin), int(intact)));
    stop_dummy(pa, a);
    stop_dummy(pb, b);
    stop_dummy(pc, c);
    // K3: closed -> the same uninstall goes through (fresh state so K3 is independent of K2)
    for (auto& p : {a, b}) {
        fs::create_directories(p.parent_path(), ec);
        fs::copy_file(dummy, p, fs::copy_options::overwrite_existing, ec);
    }
    WriteText(inst / L"setup" / L"install.json", st.Dump(2));
    Sleep(200);
    bool none = e.Running(inst).empty();
    Engine::UninstallResult u3 = e.Uninstall(inst, false, false);
    check("K3", none && !u3.running && u3.ok && !fs::exists(b, ec) && fs::exists(c, ec),
          Format("after closing: nothing running %d, uninstall ok %d (bin removed, the other installation untouched)", int(none), int(u3.ok)));
    fs::remove_all(sbx, ec);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--mutate")) g_mutate = true;
    g_root = Normal(ExeDir() / L".." / L".." / L"..");
    if (!fs::exists(g_root / L"outputs" / L"installer")) g_root = Normal(fs::current_path());
    g_work = g_root / L"work" / L"install19" / L"unit";
    std::error_code ec;
    fs::create_directories(g_work, ec);
    SetLogFile(g_work / L"unit.log");
    printf("INSTALL19 unit tests%s (root %s)\n", g_mutate ? " --mutate" : "", U(g_root).c_str());
    test_json();
    test_disc();
    test_pipeline();
    test_proc_platform();
    test_mock_prereqs();
    test_dolphin_archive(); // INSTALL19 2026-10-10
    test_data_dir_default(); // INSTALL19 2026-10-10
    test_install_dir_and_space(); // REVINSTALL19 2026-10-10
    test_uninstall();
    test_running(); // REL22
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
