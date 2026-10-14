"""INSTALL19 end-to-end: ONE real build through the release pipeline, from the unpacked SkyBreak zip, into a sandbox.

Run ONCE and only through the build lock (long, CPU-heavy, ends with a short game run):
  LOCK_MAX_WAIT=300 bash work/claude-tools/locked.sh install19 work/venv/Scripts/python.exe outputs/pc-tests/install19/e2e_sandbox.py

  E0  package: work/install19/dist-check/SkyBreak-<v> (unpacked zip) passes the legal scan and its MANIFEST
  Install folder <sandbox>/'Spellen (x86), SkyBreak', data folder <sandbox>/'Data, (Roaming) SkyBreak' and the update package
  under 'dist-update (kopie), 2': spaces, commas and parentheses through the real build, the game and the stub (10 Oct).
  E1  Setup --headless --sandbox work/install19/sandbox (disc work/pc-game, pointer mode, real prerequisite detection,
      nothing installed) cancelled at step 7 ninja 10/x: exit 4, 'geannuleerd', no job process (cl/ninja) left
  E2  same command again = resume: exit 0, mode 'resume', phase 'klaar', all 8 steps done
  E3  installed layout: SkyBreak.exe, bin\\SkyBreak-game.exe, hulp\\exenaam.txt, hulp\\spelmap.txt -> work/pc-game,
      VERSIE.txt 'SkyBreak <v>', instellingen.toml [nand] root = the sandbox data folder
  E4  sandbox shortcuts -> sandbox SkyBreak.exe, registry.json Apps entry
  E5  the built game boots to the title screen: probe.py headless boot-title (sandbox exe + sandbox settings + fresh
      sandbox NAND); title sequence reaches the file select; exit 0 (GPU guard: event 4101 count before/after)
  E6  SkyBreak.exe start program runs the game (NWII_HEADLESS, 30 s): exit 0, log in <install>\\logs with the sandbox NAND root
  E7  update: a package copy with VERSION+1 and one runtime source line changed -> mode 'update', 2 files synced,
      incremental build (< 50 ninja steps), sentinel save + edited settings line kept
  E8  uninstall: install folder, shortcuts and Apps entry gone, sentinel save kept
  E9  real desktop / Start menu / HKCU uninstall key / real SkyBreak folders unchanged; audit: everything in the sandbox
Writes work/install19/e2e/summary.json.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_setup_flow import real_state, lnk_target, UNINSTALL_KEY  # noqa: E402

PY = str(ROOT / 'work/venv/Scripts/python.exe')
GAME = ROOT / 'work/pc-game'
SANDBOX = ROOT / 'work/install19/sandbox'
OUTDIR = ROOT / 'work/install19/e2e'
# INSTALL19 10 Oct: install folder, data folder and the update package live under paths with spaces, a comma and parentheses
# REVINSTALL19: Setup writes install.json/registry.json as UTF-8 -> read them as UTF-8 (cp1252 broke non-ASCII paths)
INST = SANDBOX / 'Spellen (x86), SkyBreak'
DATA = SANDBOX / 'Data, (Roaming) SkyBreak'
results, summary = [], {}


def check(cid, cond, msg):
    results.append(cond)
    summary[cid] = {'ok': bool(cond), 'msg': msg}
    print(('PASS: ' if cond else 'FAIL: ') + f'{cid} {msg}', flush=True)


def gpu_4101():
    r = subprocess.run(['powershell', '-NoProfile', '-Command',
                        "Get-WinEvent -FilterHashtable @{LogName='System'; Id=4101; StartTime=(Get-Date).AddHours(-1)} -ErrorAction SilentlyContinue | Measure-Object | Select -Expand Count"],
                       capture_output=True, text=True)
    try:
        return int(r.stdout.strip() or 0)
    except ValueError:
        return -1


def setup(pkg, extra=(), answers=None, timeout=3 * 3600):
    OUTDIR.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%H%M%S')
    res_file = OUTDIR / f'result-{stamp}.json'
    args = [str(pkg / 'SkyBreak-Setup.exe'), '--headless', '--sandbox', str(SANDBOX), '--result', str(res_file),
            '--log', str(OUTDIR / f'setup-{stamp}.log')]
    if answers is not None:
        a = OUTDIR / 'answers.json'
        a.write_text(json.dumps(answers, indent=2))
        args += ['--answers', str(a)]
    args += list(extra)
    env = dict(os.environ)
    # this PC's VS 2022 has no 'C++ CMake tools': CMake + Ninja come from PATH (the project's venv), as a player could do
    env['PATH'] = str(ROOT / 'work/venv/Scripts') + os.pathsep + env.get('PATH', '')
    t0 = time.time()
    r = subprocess.run(args, capture_output=True, text=True, env=env, timeout=timeout)
    res = json.loads(res_file.read_text(encoding='utf-8')) if res_file.is_file() else {}
    res['_seconds'] = round(time.time() - t0, 1)
    print(f'[e2e] Setup {" ".join(extra) or "install"}: exit {r.returncode} in {res["_seconds"]} s', flush=True)
    return r.returncode, res


def tasks(name):
    r = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'], capture_output=True, text=True)
    return [l for l in r.stdout.splitlines() if l.lower().startswith(f'"{name.lower()}"')]


def main():
    ver = (ROOT / 'outputs/installer/VERSION').read_text().strip()
    pkg = ROOT / f'work/install19/dist-check/SkyBreak-{ver}'
    if not (pkg / 'SkyBreak-Setup.exe').is_file():
        print(f'FAIL: E0 {pkg} missing: run outputs/installer/tools/build_dist.py first')
        return 1
    shutil.rmtree(SANDBOX, ignore_errors=True)
    shutil.rmtree(OUTDIR, ignore_errors=True)
    OUTDIR.mkdir(parents=True)
    before = real_state()
    t_all = time.time()

    scan = subprocess.run([PY, str(ROOT / 'outputs/installer/tools/legal_scan.py'), str(pkg.parent), '--hashes',
                           str(ROOT / 'outputs/skyward-sword/config/SOUE01'), '--report', str(OUTDIR / 'legal-scan.txt')],
                          capture_output=True, text=True)
    rc, res = setup(pkg, ['--verify-package'])
    check('E0', scan.returncode == 0 and rc == 0, f'unpacked zip: legal scan exit {scan.returncode}, MANIFEST verify exit {rc}')

    ans = {'accept': True, 'lang': 'nl', 'disc': str(GAME), 'copy_mode': 'pointer', 'jobs': 16, 'install_dir': str(INST), 'data_dir': str(DATA)}
    rc, res = setup(pkg, ['--cancel-at', '7:10'], ans)
    inst = Path(res.get('install_dir', INST))
    st = json.loads((inst / 'setup/install.json').read_text(encoding='utf-8')) if (inst / 'setup/install.json').is_file() else {}
    time.sleep(2)
    left = res.get('build', {}).get('active_processes_after', -1)
    stray = tasks('ninja.exe') + tasks('cl.exe')
    summary['prereqs'] = res.get('prereqs')
    check('E1', rc == 4 and st.get('phase') == 'geannuleerd' and st.get('step') == 7 and left == 0 and not stray,
          f'cancel at 7:{st.get("a")}/{st.get("b")}: exit {rc}, phase {st.get("phase")}, job processes left {left}, stray cl/ninja {len(stray)}, '
          f'{res.get("_seconds")} s; prereqs {[(k, v.get("present")) for k, v in (res.get("prereqs") or {}).items()]}')

    rc, res = setup(pkg, [], ans)
    st = json.loads((inst / 'setup/install.json').read_text(encoding='utf-8')) if (inst / 'setup/install.json').is_file() else {}
    b = res.get('build', {})
    summary['full_build'] = {'seconds': res.get('_seconds'), 'ninja_total': b.get('ninja_total'), 'ninja_compiled': b.get('ninja_compiled'),
                             'peak_job_memory_mb': b.get('peak_job_memory_mb'), 'log': b.get('log')}
    check('E2', rc == 0 and res.get('mode') == 'resume' and st.get('phase') == 'klaar' and sorted(b.get('done_steps', [])) == list(range(1, 9)),
          f'resume: exit {rc}, mode {res.get("mode")}, phase {st.get("phase")}, steps {b.get("done_steps")}, ninja {b.get("ninja_compiled")} compiled of {b.get("ninja_total")}, '
          f'{round((res.get("_seconds") or 0) / 60, 1)} min, peak job memory {b.get("peak_job_memory_mb")} MB; error {res.get("error", "")[:200]}')
    if rc != 0:
        (OUTDIR / 'summary.json').write_text(json.dumps(summary, indent=2))
        print(f'RESULT pass={sum(results)} fail={len(results) - sum(results)} (stopped: build failed; see {b.get("log")})')
        return 1

    data = DATA
    game_exe = inst / 'bin/SkyBreak-game.exe'
    toml = (inst / 'instellingen.toml').read_text(encoding='utf-8')
    versie = (inst / 'VERSIE.txt').read_text(encoding='utf-8-sig').splitlines()[0]
    spelmap = (inst / 'hulp/spelmap.txt').read_text(encoding='utf-8-sig').strip()
    check('E3', (inst / 'SkyBreak.exe').is_file() and game_exe.is_file() and game_exe.stat().st_size > 50e6
          and (inst / 'hulp/exenaam.txt').read_text().strip() == 'SkyBreak-game' and Path(spelmap).resolve() == GAME.resolve()
          and versie.startswith(f'SkyBreak {ver}') and (data / 'nand').as_posix() in toml and not (inst / 'bin/SkywardSwordPC.exe').exists(),
          f'layout: SkyBreak-game.exe {game_exe.stat().st_size // 1048576} MB, VERSIE "{versie[:40]}", spelmap -> work/pc-game, [nand] root = sandbox data')
    lnks = [SANDBOX / 'Desktop/SkyBreak.lnk', SANDBOX / 'StartMenu/Programs/SkyBreak.lnk']
    reg = json.loads((SANDBOX / 'registry.json').read_text(encoding='utf-8')) if (SANDBOX / 'registry.json').is_file() else {}
    targets = [lnk_target(l) for l in lnks if l.is_file()]
    check('E4', len(targets) == 2 and all(Path(t).resolve() == (inst / 'SkyBreak.exe').resolve() for t in targets)
          and Path(reg.get(UNINSTALL_KEY, {}).get('InstallLocation', '')).resolve() == inst.resolve(),
          f'shortcuts -> sandbox SkyBreak.exe, Apps entry {reg.get(UNINSTALL_KEY, {}).get("DisplayName")} {reg.get(UNINSTALL_KEY, {}).get("DisplayVersion")}')

    # game: boot to the title screen with the sandbox exe, settings and a fresh sandbox NAND
    g0 = gpu_4101()
    nand = data / 'nand'
    probe_json = OUTDIR / 'probe-boot-title.json'
    pr = subprocess.run([PY, str(ROOT / 'outputs/pc-profile/probe.py'), '--seconds', '120', '--input-replay', 'boot-title', '--exe', str(game_exe),
                         '--game-dir', str(GAME), '--config', str(inst / 'instellingen.toml'), '--cwd', str(inst), '--nand-root', str(nand)],
                        capture_output=True, text=True, timeout=900)
    probe_json.write_text(pr.stdout, encoding='utf-8')
    g1 = gpu_4101()
    try:
        pj = json.loads(pr.stdout)
    except Exception:
        pj = {}
    seq = []
    for s in pj.get('title_events', []):
        n = s.split('name=')[1].split(' ')[0] if 'name=' in s else s
        if not seq or seq[-1] != n:
            seq.append(n)
    summary['probe'] = {'exit': pj.get('exit_code'), 'title_seq': seq, 'fatal': pj.get('fatal_lines', [])[:5], 'log': pj.get('log'), 'gpu_4101': [g0, g1]}
    title_ok = any('FileSelect' in s or 'Title' in s for s in seq)
    check('E5', pj.get('exit_code') == 0 and title_ok and not pj.get('fatal_lines') and g1 == g0,
          f'boot-title 120 s: exit {pj.get("exit_code")}, title sequence {seq}, 4101 events {g0}->{g1}')
    if g1 > g0:
        print('GPU GUARD: new 4101 event -> stopping all further game runs')

    if g1 == g0:
        env = dict(os.environ, NWII_HEADLESS='1', NWII_RUN_SECONDS='30', SKYBREAK_NO_DIALOG='1', NWII_RTC='0')
        g2 = gpu_4101()
        r = subprocess.run([str(inst / 'SkyBreak.exe')], env=env, timeout=600)
        g3 = gpu_4101()
        logs = sorted((inst / 'logs').glob('speel-*.log'))
        lt = logs[-1].read_text(encoding='utf-8', errors='replace') if logs else ''
        nand_line = next((l for l in lt.splitlines() if l.startswith('[NAND] root=')), '')
        # REVINSTALL19: the sandbox folder's own name, not the word 'sandbox' (a sandbox elsewhere, e.g. work/revinstall19/sbx, failed E6)
        check('E6', r.returncode == 0 and logs and 'SkyBreak' in nand_line and SANDBOX.name in nand_line and g3 == g2,
              f'SkyBreak.exe: exit {r.returncode}, log {logs[-1].name if logs else None}, "{nand_line[:120]}", 4101 {g2}->{g3}')
    else:
        check('E6', False, 'skipped because of the GPU guard')

    # update: VERSION + 1 and one runtime source line changed
    upd = ROOT / f'work/install19/dist-update (kopie), 2/SkyBreak-{ver}'
    shutil.rmtree(upd.parent, ignore_errors=True)
    shutil.copytree(pkg, upd)
    parts = ver.split('.')
    newver = '.'.join(parts[:-1] + [str(int(parts[-1]) + 1)])
    (upd / 'bron/VERSION').write_text(newver + '\n', encoding='utf-8', newline='\n')
    src = upd / 'bron/outputs/nwiirecomp/nWiiRuntime/src/ui/ui_disc.cpp'
    src.write_text(src.read_text(encoding='utf-8') + '\n// INSTALL19 E2E update test: one changed line\n', encoding='utf-8', newline='\n')
    man = []
    for p in sorted(upd.rglob('*')):
        if p.is_file() and p.name != 'MANIFEST.sha256':
            man.append(f'{hashlib.sha256(p.read_bytes()).hexdigest()} *{p.relative_to(upd).as_posix()}')
    (upd / 'MANIFEST.sha256').write_text('\n'.join(man) + '\n', encoding='utf-8', newline='\n')
    save = nand / 'INSTALL19-sentinel.sav'
    save.parent.mkdir(parents=True, exist_ok=True)
    save.write_bytes(b'SENTINEL')
    (inst / 'instellingen.toml').write_text(toml + '\n# eigen regel van de speler (E2E)\n', encoding='utf-8')
    def probe_update():  # INSTALL19 2026-10-10: where does the changed line get lost (bron / export / object)?
        out = {}
        for k, pth in (('bron', inst / 'bron/outputs/nwiirecomp/nWiiRuntime/src/ui/ui_disc.cpp'),
                       ('export', inst / 'bouw/export/nWiiRuntime/src/ui/ui_disc.cpp')):
            out[k] = {'marker': pth.is_file() and 'INSTALL19 E2E update test' in pth.read_text(encoding='utf-8', errors='replace'),
                      'mtime': round(pth.stat().st_mtime, 1) if pth.is_file() else None}
        objs = list((inst / 'bouw/build').rglob('ui_disc.cpp.obj')) if (inst / 'bouw/build').is_dir() else []
        out['obj'] = {'path': str(objs[0]) if objs else None, 'mtime': round(objs[0].stat().st_mtime, 1) if objs else None}
        exe = inst / 'bouw/build/bin/SkywardSwordPC.exe'
        out['exe_mtime'] = round(exe.stat().st_mtime, 1) if exe.is_file() else None
        return out
    summary['update_probe_before'] = probe_update()
    rc, res = setup(upd, [], ans)
    summary['update_probe_after'] = probe_update()
    print(f"[e2e] update probe before {summary['update_probe_before']}", flush=True)
    print(f"[e2e] update probe after  {summary['update_probe_after']}", flush=True)
    vj = inst / 'setup/voortgang.jsonl'
    if vj.is_file():
        shutil.copy2(vj, OUTDIR / 'update-voortgang.jsonl')
    b = res.get('build', {})
    sync = res.get('sync', {})
    st = json.loads((inst / 'setup/install.json').read_text(encoding='utf-8')) if (inst / 'setup/install.json').is_file() else {}
    summary['update'] = {'seconds': res.get('_seconds'), 'ninja_total': b.get('ninja_total'), 'ninja_compiled': b.get('ninja_compiled'), 'sync': sync}
    # keep the build logs of the update (the uninstall below removes the install folder)
    for sub in ('bouw/logs', 'setup/logs'):
        if (inst / sub).is_dir():
            shutil.copytree(inst / sub, OUTDIR / ('update-' + sub.replace('/', '-')), dirs_exist_ok=True)
    check('E7', rc == 0 and res.get('mode') == 'update' and sync.get('copied') == 2 and 0 < (b.get('ninja_total') or 999) < 50
          and (b.get('ninja_compiled') or 0) >= 1 and st.get('version') == newver
          and save.read_bytes() == b'SENTINEL' and '# eigen regel van de speler (E2E)' in (inst / 'instellingen.toml').read_text(encoding='utf-8'),
          f'update {ver} -> {newver}: exit {rc}, sync {sync}, ninja {b.get("ninja_compiled")} compiled of {b.get("ninja_total")} steps, '
          f'{round((res.get("_seconds") or 0) / 60, 1)} min, save + settings kept')

    rc, res = setup(upd, ['--uninstall', '--install-dir', str(inst)])
    reg = json.loads((SANDBOX / 'registry.json').read_text(encoding='utf-8')) if (SANDBOX / 'registry.json').is_file() else {}
    check('E8', rc == 0 and not inst.exists() and not any(l.exists() for l in lnks) and UNINSTALL_KEY not in reg and save.is_file(),
          f'uninstall: exit {rc}, install folder gone {not inst.exists()}, shortcuts + Apps entry gone, sentinel save kept {save.is_file()}')

    after = real_state()
    audit = (SANDBOX / 'audit-platform.txt').read_text(encoding='utf-8').splitlines() if (SANDBOX / 'audit-platform.txt').is_file() else []
    bad = [l for l in audit if l and not l.endswith('|sandbox')]
    procs = (SANDBOX / 'audit-proc.txt').read_text(encoding='utf-8').splitlines() if (SANDBOX / 'audit-proc.txt').is_file() else []
    installers = [l for l in procs if ('|run|' in l or '|job|' in l) and any(x in l.lower() for x in ('winget', 'vs_buildtools', 'msiexec'))]
    check('E9', before == after and not bad and not installers,
          f'real system unchanged; {len(audit)} audited side effects all in the sandbox; {len(procs)} process launches, 0 installers')
    summary['minutes_total'] = round((time.time() - t_all) / 60, 1)
    (OUTDIR / 'summary.json').write_text(json.dumps(summary, indent=2))
    shutil.rmtree(upd.parent, ignore_errors=True)
    print(f'RESULT pass={sum(results)} fail={len(results) - sum(results)} ({summary["minutes_total"]} min)')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
