"""INSTALL19: SkyBreak-Setup end-to-end flows WITHOUT installing anything on this PC.

Every run is `SkyBreak-Setup.exe --headless --sandbox <dir> --mock-prereqs <json> --build-script fake-bouw.ps1`
from a mini package (built here: Setup + stub copied from work/install19/build, a small bron/, MANIFEST.sha256).
Shortcuts, the Apps entry (registry.json) and all folders land in work/install19/flow/<scenario>/. The build is the
test double fake-bouw.ps1 (same progress stream, nothing compiled).

  F1  install, all prerequisites present: exit 0; .lnk (desktop + Start menu) in the sandbox -> sandbox SkyBreak.exe;
      registry.json entry; install.json 'klaar'; [nand] root = sandbox %APPDATA%/SkywardSwordPC/nand (user decision); bin\\SkyBreak-game.exe; no installs
  F2  VS missing and NO consent -> exit 3, nothing installed            (NEGATIVE mutant --mutate skip-consent must fail F2)
  F3  VS missing, consent, installer exit 3010 -> exit 3, restart state 'herstart'
  F4  winget missing, consent -> bootstrapper route, then install completes (exit 0)
  F5  Python download with a wrong SHA-256 -> refused, exit 3
  F6  disc image + DolphinTool missing + tar without liblzma -> browser route (URL only logged), exit 3
  F7  PAL disc -> exit 2
  F8  package with one file deleted -> exit 6 (MANIFEST)
  F9  cancel at step 7, 10/40 -> exit 4, 'geannuleerd', no job process left; second run resumes and finishes
  F15 cancel + resume in a chosen (non-default) install folder -> mode 'resume' (10 Oct, found by the E2E)
  F10 update 0.19.0 -> 0.19.1: only changed sources copied; sentinel save and an edited settings line kept
  F11 uninstall (default): install folder, shortcuts, Apps entry gone; sentinel save kept
  F12 uninstall --delete-saves: saves gone; the pointer-mode game folder (work/pc-game) untouched
  F14 paths with spaces, a comma and parentheses (WORK, the disc copy, install + data folders) arrive intact
  F13 SkyBreak.exe start program: starts bin\\<exenaam>.exe "<game>" "<toml>" without a window, log in logs\\, exit code passed on
  F16 (REVINSTALL19) low disk space, mocked (--mock-free-gb 1, honoured only with --sandbox): exit 5 before anything is copied or
      built; control run with 1000 GB free installs (exit 0)
  F17 (REVINSTALL19) install into an existing folder with the player's own files (bin\\, game\\): refused (exit 1, 'niet leeg'),
      the player's files untouched                                 (NEGATIVE mutant --mutate allow-foreign-dir must fail F17)
  F18 (REVINSTALL19) repair started from the INSTALLED copy <install>\\setup\\SkyBreak-Setup.exe (what Windows Settings > Apps runs):
      exit 0, mode 'repair', version stays 0.19.1 in install.json and the Apps entry (was: refused as "inside the zip folder", and the
      copy read version 0.0.0)                                     (NEGATIVE mutant --mutate old-zip-check must fail F18)
  G1  REAL desktop / Start menu .lnk lists and HKCU\\...\\Uninstall\\SkyBreak unchanged by all runs (read-only checks)
      (REVINSTALL19: shortcut size+mtime, Apps entry values and Setup's files of a REAL install are compared; a real install by
      the user is allowed - since 10 Oct 20:03 there is one)
  F19 (REVINSTALL19 continuation, REL22 reversed) the UTF-8 activeCodePage manifests make non-ASCII paths work, so they are
      ACCEPTED and installed (Windows 10 1903+): install folders 'Spellen Zoë', 'Spellen 日' and 'Spellen/Zéldà ✦ test' (exit 0,
      phase klaar, start program + [nand] root in place), pointer mode to a disc folder 'Schijf Zoë' (exit 0)
                                                               (NEGATIVE mutant --mutate refuse-non-ascii = the 0.19 rule must fail F19)
  F20 (REL22) a running SkyBreak of THIS installation (dummy at <install>\\bin\\SkyBreak-game.exe) blocks the update and the
      uninstall up front: exit 7, 'running' lists exactly that path, install.json/files unchanged, nothing killed; a copy with the
      same names in ANOTHER installation (like the user's real one) is not listed   (NEGATIVE --mutate ignore-running must fail F20)
  F21 (REL22) --wait-closed: the start program <install>\\SkyBreak.exe runs ~4 s; the update waits for it and then finishes
      (exit 0, waited >= 2.5 s)                                  (NEGATIVE --mutate ignore-running must fail F21)
  G2  every platform side effect of every run was in the sandbox (audit), no installer was started
                                                               (NEGATIVE mutant --mutate leak-desktop must fail G2)
  (REVINSTALL19 continuation) Setup writes install.json/registry.json as UTF-8: they are read as UTF-8 (read_text() = cp1252
  made F1/F14 fail falsely under a non-ASCII folder).
  test_setup_flow.py           -> RESULT pass=23 fail=0
  test_setup_flow.py --mutate  -> exactly F2 F17 F18 F19 F20 F21 G2 fail  (REVINSTALL19: + F17 F18, continuation: + F19; REL22: + F20 F21)
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
BUILD = ROOT / 'work/install19/build'
# INSTALL19 10 Oct: every flow runs under a path with spaces, a comma and parentheses (package, sandbox, install, data, disc)
WORK = ROOT / 'work/install19/flow (x86), test'
GAME = ROOT / 'work/pc-game'
DISC = WORK / 'Schijf (USA), kopie'  # boot.bin + main.dol + rels.arc of work/pc-game; holds game data -> always removed again
FAKE = HERE / 'fake-bouw.ps1'
MUTATE = '--mutate' in sys.argv
sys.stdout.reconfigure(errors='backslashreplace')  # REVINSTALL19 continuation: F19 prints non-ASCII paths; run-all pipes cp1252
results = []
UNINSTALL_KEY = r'HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\SkyBreak'


def check(cid, cond, msg):
    results.append(cond)
    print(('PASS: ' if cond else 'FAIL: ') + f'{cid} {msg}', flush=True)


def sha256(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def make_package(name, version='0.19.0', changed=False):
    pkg = WORK / name / 'SkyBreak-test'
    if pkg.parent.exists():
        shutil.rmtree(pkg.parent)
    (pkg / 'bron/sub').mkdir(parents=True)
    (pkg / 'bron/VERSION').write_text(version + '\n')
    shutil.copy2(ROOT / 'outputs/installer/prereqs.json', pkg / 'bron/prereqs.json')
    for i in range(5):
        (pkg / f'bron/sub/bron{i}.txt').write_text(f'bronbestand {i}\n' + ('gewijzigd\n' if changed and i == 2 else ''))
    shutil.copy2(BUILD / 'SkyBreak-Setup.exe', pkg / 'SkyBreak-Setup.exe')
    (pkg / 'app').mkdir()
    shutil.copy2(BUILD / 'SkyBreak.exe', pkg / 'app/SkyBreak.exe')
    shutil.copy2(ROOT / 'outputs/branding/skybreak.ico', pkg / 'app/skybreak.ico')
    lines = [f'{sha256(p)} *{p.relative_to(pkg).as_posix()}' for p in sorted(pkg.rglob('*')) if p.is_file()]
    (pkg / 'MANIFEST.sha256').write_text('\n'.join(lines) + '\n')
    return pkg


def setup(pkg, sandbox, answers=None, mock=None, extra=(), mutate=None):
    sandbox.mkdir(parents=True, exist_ok=True)
    args = [str(pkg / 'SkyBreak-Setup.exe'), '--headless', '--sandbox', str(sandbox), '--build-script', str(FAKE),
            '--result', str(sandbox / 'result.json')]
    if answers is not None:
        (sandbox / 'answers.json').write_text(json.dumps(answers))
        args += ['--answers', str(sandbox / 'answers.json')]
    if mock is not None:
        (sandbox / 'mock.json').write_text(json.dumps(mock))
        args += ['--mock-prereqs', str(sandbox / 'mock.json')]
    if mutate:
        args += ['--mutate', mutate]
    args += list(extra)
    env = dict(os.environ, FAKE_GAME_EXE=str(Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/cmd.exe'))
    r = subprocess.run(args, capture_output=True, text=True, env=env, timeout=600)
    res = {}
    if (sandbox / 'result.json').is_file():
        res = json.loads((sandbox / 'result.json').read_text(encoding='utf-8'))
    return r.returncode, res


def answers(disc=DISC, **kw):
    a = {'accept': True, 'lang': 'nl', 'disc': str(disc), 'copy_mode': 'pointer'}
    a.update(kw)
    return a


ALL = {'present': ['vs', 'python', 'dolphintool']}


def lnk_target(lnk):
    # read-only. REVINSTALL19: Shell.Application GetLink (Unicode) instead of WScript.Shell.CreateShortcut().TargetPath, which returns
    # '' for a target with characters outside the ANSI code page; path in via env, result out via a UTF-8 file (no console code page)
    out = Path(lnk).parent / ('lnk-target-' + str(os.getpid()) + '.txt')
    ps = ("$l = (New-Object -ComObject Shell.Application).Namespace($env:SB_LDIR).ParseName($env:SB_LNAME).GetLink; "
          "[IO.File]::WriteAllText($env:SB_OUT, $l.Path, (New-Object System.Text.UTF8Encoding($false)))")
    subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True, text=True,
                   env=dict(os.environ, SB_LDIR=str(Path(lnk).parent), SB_LNAME=Path(lnk).name, SB_OUT=str(out)))
    try:
        return out.read_text(encoding='utf-8').strip()
    except OSError:
        return ''
    finally:
        out.unlink(missing_ok=True)


def real_state():
    profile = Path(os.environ['USERPROFILE'])
    appdata = Path(os.environ['APPDATA'])
    # REVINSTALL19 10 Oct: the user now has a REAL SkyBreak install (own GUI run 19:47-20:03), so G1 can no longer demand absence.
    # It fingerprints what a leaking sandbox run would write (shortcut names + size + mtime, the Apps entry values, Setup's files in
    # the real install folder) and requires before == after. Things that change while the player plays (logs, settings, save
    # contents) are left out; save files are compared by name + size only.
    lnk_fp = lambda p: (p.stat().st_size, p.stat().st_mtime_ns)
    desk = sorted((p.name,) + lnk_fp(p) for p in (profile / 'Desktop').glob('*.lnk')) if (profile / 'Desktop').is_dir() else []
    od = sorted((p.name,) + lnk_fp(p) for p in (profile / 'OneDrive/Desktop').glob('*.lnk')) if (profile / 'OneDrive/Desktop').is_dir() else []
    sm = appdata / 'Microsoft/Windows/Start Menu/Programs'
    start = sorted((str(p.relative_to(sm)),) + lnk_fp(p) for p in sm.rglob('*.lnk')) if sm.is_dir() else []
    r = subprocess.run(['reg', 'query', UNINSTALL_KEY], capture_output=True, text=True)
    skybreak_files = [str(p) for p in [appdata / 'SkyBreak', Path(os.environ['LOCALAPPDATA']) / 'Programs/SkyBreak'] if p.exists()]
    real_inst = Path(os.environ['LOCALAPPDATA']) / 'Programs/SkyBreak'
    inst_fp = sorted((rel,) + lnk_fp(real_inst / rel) for rel in ('SkyBreak.exe', 'skybreak.ico', 'setup/install.json', 'setup/SkyBreak-Setup.exe',
                                                                   'bin/SkyBreak-game.exe', 'hulp/spelmap.txt', 'VERSIE.txt')
                     if (real_inst / rel).is_file())
    # INSTALL19 10 Oct: Setup's default data folder is now the player's REAL save folder %APPDATA%\SkywardSwordPC (user decision),
    # so its tree is fingerprinted too (read only: names, sizes, mtimes) and must not change during sandbox runs
    old = appdata / 'SkywardSwordPC'
    saves = sorted((str(q.relative_to(old)), q.stat().st_size) for q in old.rglob('*') if q.is_file()) if old.is_dir() else []
    return {'desktop': desk, 'onedrive_desktop': od, 'startmenu': start, 'reg_rc': r.returncode, 'reg_out': r.stdout.strip(),
            'skybreak_dirs': skybreak_files, 'real_install_files': inst_fp, 'real_save_tree': saves}


def main():
    if not (BUILD / 'SkyBreak-Setup.exe').is_file():
        print('FAIL: F0 work/install19/build/SkyBreak-Setup.exe missing (outputs/installer/build-setup.cmd via locked.sh)')
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    WORK.mkdir(parents=True)
    for rel in ('sys/boot.bin', 'sys/main.dol', 'files/rels.arc'):
        (DISC / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(GAME / rel, DISC / rel)
    try:
        return run_flows()
    finally:
        shutil.rmtree(DISC, ignore_errors=True)  # never leave a copy of game data behind


def run_flows():
    before = real_state()
    pkg = make_package('pkg')
    sandboxes = []

    # F1 install
    sb1 = WORK / 'f1'
    sandboxes.append(sb1)
    rc, res = setup(pkg, sb1, answers(), ALL, mutate='leak-desktop' if MUTATE else None)
    inst = sb1 / 'AppData/Local/Programs/SkyBreak'
    data = sb1 / 'AppData/Roaming/SkywardSwordPC'  # INSTALL19 10 Oct user decision: the save folder stays %APPDATA%/SkywardSwordPC/nand
    lnks = [sb1 / 'Desktop/SkyBreak.lnk', sb1 / 'StartMenu/Programs/SkyBreak.lnk']
    reg = json.loads((sb1 / 'registry.json').read_text(encoding='utf-8')) if (sb1 / 'registry.json').is_file() else {}
    entry = reg.get(UNINSTALL_KEY, {})
    st = json.loads((inst / 'setup/install.json').read_text(encoding='utf-8')) if (inst / 'setup/install.json').is_file() else {}
    toml = (inst / 'instellingen.toml').read_text(encoding='utf-8') if (inst / 'instellingen.toml').is_file() else ''
    targets = [lnk_target(l) for l in lnks if l.is_file()]
    ok = (rc == 0 and all(l.is_file() for l in lnks) and all(Path(t).resolve() == (inst / 'SkyBreak.exe').resolve() for t in targets)
          and len(targets) == 2 and Path(entry.get('InstallLocation', '')).resolve() == inst.resolve() and entry.get('DisplayName') == 'SkyBreak'
          and 'SkyBreak-Setup.exe" --uninstall' in entry.get('UninstallString', '') and st.get('phase') == 'klaar'
          and (data / 'nand').as_posix() in toml and (inst / 'bin/SkyBreak-game.exe').is_file() and (inst / 'SkyBreak.exe').is_file()
          and not res.get('installs') and res.get('mode') == 'install')
    check('F1', ok, f'install in sandbox: exit {rc}, lnk -> {[Path(t).name for t in targets]}, Apps entry {entry.get("DisplayName")}, phase {st.get("phase")}')

    # F14 (10 Oct) special characters: install, data and disc paths with spaces, a comma and parentheses reach the build script intact
    spel = (inst / 'hulp/spelmap.txt').read_text(encoding='utf-8-sig').strip() if (inst / 'hulp/spelmap.txt').is_file() else ''
    special = all(all(ch in str(p) for ch in ' ,()') for p in (inst, data, DISC))
    check('F14', rc == 0 and special and spel and Path(spel).resolve() == DISC.resolve() and (data / 'nand').as_posix() in toml
          and Path(st.get('game_dir', '')).resolve() == DISC.resolve(),
          f'paths with spaces/comma/parentheses: disc "{DISC.name}" -> hulp/spelmap.txt, [nand] root under "{WORK.name}", install.json game_dir')

    # F2 VS missing, no consent
    sb2 = WORK / 'f2'
    sandboxes.append(sb2)
    rc, res = setup(pkg, sb2, answers(consent={'vs': False}), {'present': ['python', 'dolphintool']},
                    mutate='skip-consent' if MUTATE else None)
    check('F2', rc == 3 and not res.get('installs') and res.get('missing') == 'vs',
          f'VS missing without consent: exit {rc}, installs {res.get("installs")}')

    # F3 consent + 3010
    sb3 = WORK / 'f3'
    sandboxes.append(sb3)
    rc, res = setup(pkg, sb3, answers(consent={'vs': True}), {'present': ['python', 'dolphintool'], 'actions': {'vs': {'exit': 3010, 'seconds': 0.2}}})
    st3 = json.loads((sb3 / 'AppData/Local/Programs/SkyBreak/setup/install.json').read_text(encoding='utf-8')) if (sb3 / 'AppData/Local/Programs/SkyBreak/setup/install.json').is_file() else {}
    check('F3', rc == 3 and res.get('restart') is True and st3.get('phase') == 'herstart',
          f'VS installer 3010: exit {rc}, restart {res.get("restart")}, phase {st3.get("phase")}')

    # F4 winget missing -> bootstrapper
    sb4 = WORK / 'f4'
    sandboxes.append(sb4)
    rc, res = setup(pkg, sb4, answers(consent={'vs': True}), {'present': ['python', 'dolphintool'], 'winget': False, 'actions': {'vs': {'exit': 0}}})
    inst4 = res.get('installs') or [{}]
    check('F4', rc == 0 and inst4[0].get('route') == 'bootstrapper' and inst4[0].get('ok') is True,
          f'no winget: route {inst4[0].get("route")}, exit {rc}')

    # F5 python sha mismatch
    sb5 = WORK / 'f5'
    sandboxes.append(sb5)
    rc, res = setup(pkg, sb5, answers(consent={'python': True}), {'present': ['vs', 'dolphintool'], 'actions': {'python': {'sha256_ok': False}}})
    inst5 = res.get('installs') or [{}]
    check('F5', rc == 3 and inst5[0].get('ok') is False and 'SHA-256' in inst5[0].get('message', ''),
          f'python hash mismatch refused: exit {rc}, {inst5[0].get("message")}')

    # F6 image + dolphintool missing + tar without lzma -> browser route
    sb6 = WORK / 'f6'
    sandboxes.append(sb6)
    rvz = WORK / 'synth-soue01.rvz'
    b = bytearray(0x200)
    b[0:4] = b'RVZ\x01'
    b[0x58:0x5E] = b'SOUE01'
    struct.pack_into('>I', b, 0x58 + 0x18, 0x5D1C9EA3)
    rvz.write_bytes(bytes(b))
    rc, res = setup(pkg, sb6, answers(disc=rvz, copy_mode='copy', consent={'dolphintool': True}),
                    {'present': ['vs', 'python'], 'tar_lzma': False})
    inst6 = res.get('installs') or [{}]
    check('F6', rc == 3 and inst6[0].get('route') == 'browser' and inst6[0].get('manual') is True,
          f'DolphinTool via browser route: exit {rc}, route {inst6[0].get("route")}')

    # F7 PAL disc
    sb7 = WORK / 'f7'
    sandboxes.append(sb7)
    pal = WORK / 'synth-soup01.iso'
    b = bytearray(0x800)
    b[0:6] = b'SOUP01'
    struct.pack_into('>I', b, 0x18, 0x5D1C9EA3)
    pal.write_bytes(bytes(b))
    rc, res = setup(pkg, sb7, answers(disc=pal), ALL)
    check('F7', rc == 2 and res.get('disc', {}).get('verdict') == 'wrong_region', f'PAL disc refused: exit {rc}, {res.get("disc", {}).get("message", "")[:70]}')

    # F8 incomplete package
    bad = make_package('pkg-bad')
    (bad / 'bron/sub/bron3.txt').unlink()
    sb8 = WORK / 'f8'
    sandboxes.append(sb8)
    rc, res = setup(bad, sb8, answers(), ALL)
    check('F8', rc == 6 and any('bron3' in x for x in res.get('package_bad', [])), f'package with a missing file: exit {rc}, {res.get("package_bad")}')

    # F9 cancel + resume
    sb9 = WORK / 'f9'
    sandboxes.append(sb9)
    rc, res = setup(pkg, sb9, answers(), ALL, extra=['--cancel-at', '7:10'])
    inst9 = sb9 / 'AppData/Local/Programs/SkyBreak'
    st9 = json.loads((inst9 / 'setup/install.json').read_text(encoding='utf-8')) if (inst9 / 'setup/install.json').is_file() else {}
    left = res.get('build', {}).get('active_processes_after', -1)
    rc2, res2 = setup(pkg, sb9, answers(), ALL)
    log2 = Path(res2.get('build', {}).get('log', 'x'))
    resumed = log2.is_file() and 'hervat vanaf' in log2.read_text(encoding='utf-8', errors='replace')
    check('F9', rc == 4 and st9.get('phase') == 'geannuleerd' and left == 0 and rc2 == 0 and res2.get('mode') == 'resume' and resumed,
          f'cancel at 7:10: exit {rc}, phase {st9.get("phase")}, job processes left {left}; resume: exit {rc2}, mode {res2.get("mode")}, resumed {resumed}')

    # F15 (10 Oct, found by E2E E2) cancel + resume in a CHOSEN install folder (no Apps entry yet): mode must be 'resume'
    sb15 = WORK / 'f15'
    sandboxes.append(sb15)
    inst15 = sb15 / 'Mijn spellen (eigen map), SkyBreak'
    rc, res = setup(pkg, sb15, answers(install_dir=str(inst15)), ALL, extra=['--cancel-at', '7:10'])
    st15 = json.loads((inst15 / 'setup/install.json').read_text(encoding='utf-8')) if (inst15 / 'setup/install.json').is_file() else {}
    rc2, res2 = setup(pkg, sb15, answers(install_dir=str(inst15)), ALL)
    log15 = Path(res2.get('build', {}).get('log', 'x'))
    resumed15 = log15.is_file() and 'hervat vanaf' in log15.read_text(encoding='utf-8', errors='replace')
    check('F15', rc == 4 and st15.get('phase') == 'geannuleerd' and rc2 == 0 and res2.get('mode') == 'resume' and resumed15
          and Path(res2.get('install_dir', '')).resolve() == inst15.resolve(),
          f'chosen folder "{inst15.name}": cancel exit {rc} ({st15.get("phase")}), second run exit {rc2}, mode {res2.get("mode")}, resumed {resumed15}')

    # F10 update (on F1's installation)
    save = data / 'nand/title/00010000/534f5545/data/wiiking2.sav'
    save.parent.mkdir(parents=True, exist_ok=True)
    save.write_bytes(b'SENTINEL-SAVE')
    tomlp = inst / 'instellingen.toml'
    tomlp.write_text(tomlp.read_text(encoding='utf-8') + '\n# eigen regel van de speler\n', encoding='utf-8')
    pkg2 = make_package('pkg2', version='0.19.1', changed=True)
    rc, res = setup(pkg2, sb1, answers(), ALL)
    sync = res.get('sync', {})
    st10 = json.loads((inst / 'setup/install.json').read_text(encoding='utf-8')) if (inst / 'setup/install.json').is_file() else {}
    check('F10', rc == 0 and res.get('mode') == 'update' and sync.get('copied') == 2 and sync.get('unchanged', 0) >= 5 and save.read_bytes() == b'SENTINEL-SAVE'
          and '# eigen regel van de speler' in tomlp.read_text(encoding='utf-8') and st10.get('version') == '0.19.1',
          f'update: exit {rc}, mode {res.get("mode")}, sync {sync}, version {st10.get("version")}, save + settings kept')

    # F13 start program (before uninstalling F1): bin\SkyBreak-game.exe is a renamed cmd.exe here (stdin = NUL: prints its banner, exits 0)
    env = dict(os.environ, SKYBREAK_NO_DIALOG='1')
    r = subprocess.run([str(inst / 'SkyBreak.exe')], capture_output=True, env=env, timeout=60)
    logs = sorted((inst / 'logs').glob('speel-*.log'))
    log_text = logs[-1].read_text(encoding='utf-8', errors='replace') if logs else ''
    where_rc = 0
    (inst / 'hulp/exenaam.txt').write_text('bestaat-niet')
    r2 = subprocess.run([str(inst / 'SkyBreak.exe')], capture_output=True, env=env, timeout=60)
    (inst / 'hulp/exenaam.txt').write_text('SkyBreak-game')
    check('F13', r.returncode == where_rc and len(logs) == 1 and log_text.strip() != '' and r2.returncode == 2,
          f'SkyBreak.exe: game exit {r.returncode} passed on (expected {where_rc}), log {logs[-1].name if logs else None}; missing exe -> exit {r2.returncode}')

    # F18 (REVINSTALL19) repair from the installed copy (after F10: installed 0.19.1, <install>\setup holds pkg2's Setup)
    rc, res = setup(inst / 'setup', sb1, answers(), ALL, mutate='old-zip-check' if MUTATE else None)
    st18 = json.loads((inst / 'setup/install.json').read_text(encoding='utf-8')) if (inst / 'setup/install.json').is_file() else {}
    reg18 = json.loads((sb1 / 'registry.json').read_text(encoding='utf-8')) if (sb1 / 'registry.json').is_file() else {}
    ver18 = reg18.get(UNINSTALL_KEY, {}).get('DisplayVersion')
    check('F18', rc == 0 and res.get('mode') == 'repair' and st18.get('version') == '0.19.1' and ver18 == '0.19.1' and st18.get('phase') == 'klaar',
          f'repair from <install>\\setup: exit {rc}, mode {res.get("mode")}, install.json version {st18.get("version")}, Apps entry {ver18}, '
          f'issues {res.get("path_issues", [])[:1]}')

    # F11 uninstall default (F1)
    rc, res = setup(pkg2, sb1, None, None, extra=['--uninstall', '--install-dir', str(inst)])
    reg = json.loads((sb1 / 'registry.json').read_text(encoding='utf-8')) if (sb1 / 'registry.json').is_file() else {}
    check('F11', rc == 0 and not inst.exists() and not any(l.exists() for l in lnks) and UNINSTALL_KEY not in reg and save.read_bytes() == b'SENTINEL-SAVE',
          f'uninstall: exit {rc}, install dir gone {not inst.exists()}, shortcuts gone, Apps entry gone, save kept {save.is_file()}')

    # F12 uninstall --delete-saves (F4)
    inst4d = sb4 / 'AppData/Local/Programs/SkyBreak'
    save4 = sb4 / 'AppData/Roaming/SkywardSwordPC/nand/x.sav'
    save4.parent.mkdir(parents=True, exist_ok=True)
    save4.write_bytes(b'S')
    dol_before = (DISC / 'sys/main.dol').stat().st_mtime_ns
    rc, res = setup(pkg, sb4, None, None, extra=['--uninstall', '--install-dir', str(inst4d), '--delete-saves'])
    check('F12', rc == 0 and res.get('saves_removed') is True and not save4.exists() and (DISC / 'sys/main.dol').stat().st_mtime_ns == dol_before,
          f'uninstall --delete-saves: exit {rc}, saves removed {res.get("saves_removed")}, pointer-mode disc folder untouched')

    # F16 (REVINSTALL19) low disk space (mocked, sandbox only): refused with exit 5 before sources are copied or anything is built
    sb16 = WORK / 'f16'
    sandboxes.append(sb16)
    inst16 = sb16 / 'AppData/Local/Programs/SkyBreak'
    rc, res = setup(pkg, sb16, answers(), ALL, extra=['--mock-free-gb', '1'])
    sp = (res.get('space') or [{}])[0]
    nothing16 = not inst16.exists() and not (sb16 / 'Desktop/SkyBreak.lnk').exists() and UNINSTALL_KEY not in (
        json.loads((sb16 / 'registry.json').read_text(encoding='utf-8')) if (sb16 / 'registry.json').is_file() else {})
    rc_ok, res_ok = setup(pkg, sb16, answers(), ALL, extra=['--mock-free-gb', '1000'])  # control: enough space -> installs
    check('F16', rc == 5 and abs(sp.get('free_gb', -1) - 1.0) < 0.01 and sp.get('need_gb', 0) > 1.0 and nothing16 and rc_ok == 0,
          f'low disk (1 GB free, {sp.get("need_gb", 0):.1f} GB needed): exit {rc}, nothing written {nothing16}; control 1000 GB free: exit {rc_ok}')

    # F17 (REVINSTALL19) a typed existing folder with the player's own content (e.g. "D:\Games"): uninstall removes created entries BY
    # NAME (bin, logs, setup, game ...) and the build script empties its own <install>\game, so Setup must refuse such a folder
    sb17 = WORK / 'f17'
    sandboxes.append(sb17)
    games = sb17 / 'Spellen (eigen), D'
    (games / 'bin').mkdir(parents=True)
    (games / 'bin/mijn-tool.txt').write_text('van de speler')
    (games / 'game').mkdir()
    (games / 'game/mijn-save.txt').write_text('van de speler')
    rc, res = setup(pkg, sb17, answers(install_dir=str(games)), ALL, mutate='allow-foreign-dir' if MUTATE else None)
    issues = res.get('path_issues', [])
    own17 = (games / 'bin/mijn-tool.txt').is_file() and (games / 'game/mijn-save.txt').is_file()
    check('F17', rc == 1 and any('niet leeg' in i for i in issues) and own17 and not (games / 'setup').exists() and not (games / 'bron').exists(),
          f'foreign non-empty install folder: exit {rc}, issue {[i for i in issues if "niet leeg" in i][:1]}, player files intact {own17}')

    # F19 (REVINSTALL19 continuation; REL22: reversed) non-ASCII paths used to stop the real build at step 6 (E2E 'Zoë', 'Zoë 日').
    # With the UTF-8 activeCodePage manifests of nwiirecomp.exe + the game exe they work, so Setup accepts and installs them.
    sb19 = WORK / 'f19'
    sandboxes.append(sb19)
    mut19 = 'refuse-non-ascii' if MUTATE else None
    rcs19, why19 = [], []
    for name in ('Spellen Zoë', 'Spellen 日', 'Spellen/Zéldà ✦ test'):
        d19 = sb19 / name / 'SkyBreak'
        rc, res = setup(pkg, sb19, answers(install_dir=str(d19)), ALL, mutate=mut19)
        st19 = json.loads((d19 / 'setup/install.json').read_text(encoding='utf-8')) if (d19 / 'setup/install.json').is_file() else {}
        toml19 = (d19 / 'instellingen.toml').read_text(encoding='utf-8') if (d19 / 'instellingen.toml').is_file() else ''
        rcs19.append(rc == 0 and st19.get('phase') == 'klaar' and (d19 / 'SkyBreak.exe').is_file() and '[nand]' in toml19
                     and Path(st19.get('install_dir', '')).resolve() == d19.resolve())
        why19.append(rc)
    disc19 = sb19 / 'Schijf Zoë'
    for rel in ('sys/boot.bin', 'sys/main.dol', 'files/rels.arc'):
        (disc19 / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(GAME / rel, disc19 / rel)
    try:
        rc, res = setup(pkg, sb19, answers(disc=disc19, install_dir=str(sb19 / 'SkyBreak-ascii')), ALL, mutate=mut19)
    finally:
        shutil.rmtree(disc19, ignore_errors=True)  # never leave a copy of game data behind
    rcs19.append(rc == 0 and (sb19 / 'SkyBreak-ascii/setup/install.json').is_file())
    why19.append(rc)
    check('F19', all(rcs19), f'REL22: non-ASCII install folders (accent, CJK, "Zéldà ✦ test") and pointer mode to a non-ASCII disc folder '
          f'accepted and installed: {rcs19}, exit codes {why19}')

    # F20 / F21 (REL22) SkyBreak of THIS installation still running: Setup asks (headless: waits --wait-closed s, else exit 7) and
    # never kills it. A separate installation, so the mutant run (which really tries to overwrite/remove) cannot disturb the others.
    from dummy_proc import Dummy
    sb20 = WORK / 'f20'
    sandboxes.append(sb20)
    mut20 = 'ignore-running' if MUTATE else None
    inst20 = sb20 / 'Spellen' / 'Zéldà ✦ test' / 'SkyBreak'
    rc, res = setup(pkg, sb20, answers(install_dir=str(inst20)), ALL)
    other20 = WORK / 'f20-andere installatie' / 'SkyBreak'  # same names elsewhere (like the user's real install): must not count
    st_before = (inst20 / 'setup/install.json').read_bytes() if (inst20 / 'setup/install.json').is_file() else b''
    pkg3 = make_package('pkg3', version='0.19.2', changed=True)
    with Dummy(inst20 / 'bin/SkyBreak-game.exe') as d1, Dummy(other20 / 'bin/SkyBreak-game.exe') as d2:
        rc_u, res_u = setup(pkg3, sb20, answers(install_dir=str(inst20)), ALL, mutate=mut20)
        rc_x, res_x = setup(pkg3, sb20, None, None, extra=('--uninstall', '--install-dir', str(inst20)), mutate=mut20)
        alive = d1.proc.poll() is None and d2.proc.poll() is None
    run_u = [Path(p).resolve() for p in res_u.get('running', [])]
    intact = (st_before != b'' and (inst20 / 'setup/install.json').is_file() and (inst20 / 'setup/install.json').read_bytes() == st_before
              and (inst20 / 'SkyBreak.exe').is_file() and (inst20 / 'instellingen.toml').is_file())
    check('F20', rc == 0 and rc_u == 7 and rc_x == 7 and run_u == [(inst20 / 'bin/SkyBreak-game.exe').resolve()] and intact and alive
          and 'SkyBreak draait nog' in res_u.get('error', ''),
          f'while bin\\SkyBreak-game.exe runs: update exit {rc_u}, uninstall exit {rc_x} (want 7), listed {[p.name for p in run_u]}, '
          f'files + install.json unchanged {intact}, both dummies still alive (never killed) {alive}')
    import time as _t
    with Dummy(inst20 / 'SkyBreak.exe', seconds=4):
        t0 = _t.monotonic()
        rc21, res21 = setup(pkg3, sb20, answers(install_dir=str(inst20)), ALL, extra=('--wait-closed', '30'), mutate=mut20)
        waited = _t.monotonic() - t0
    st21 = json.loads((inst20 / 'setup/install.json').read_text(encoding='utf-8')) if (inst20 / 'setup/install.json').is_file() else {}
    check('F21', rc21 == 0 and waited >= 2.5 and st21.get('version') == '0.19.2' and (inst20 / 'SkyBreak.exe').is_file(),
          f'--wait-closed 30 while the start program runs ~4 s: exit {rc21}, waited {waited:.1f} s, version {st21.get("version")}')

    # G1 real system unchanged, G2 audit
    after = real_state()
    diff = sorted(k for k in before if before[k] != after.get(k))
    check('G1', before == after,  # REVINSTALL19: no absence requirement any more (the user's own real install is allowed)
          f'real desktop ({len(after["desktop"])} lnk), Start menu ({len(after["startmenu"])} lnk), HKCU uninstall key '
          f'({"present = the user own install" if after["reg_rc"] == 0 else "absent"}), real SkyBreak files and save names unchanged; changed: {diff}')
    bad_lines, procs = [], []
    for s in sandboxes:
        a = s / 'audit-platform.txt'
        if a.is_file():
            bad_lines += [l for l in a.read_text(encoding='utf-8').splitlines() if l and not l.endswith('|sandbox')]
        p = s / 'audit-proc.txt'
        if p.is_file():
            procs += [l for l in p.read_text(encoding='utf-8').splitlines() if '|run|' in l or '|job|' in l]
    started_installers = [l for l in procs if any(x in l.lower() for x in ('winget', 'vs_buildtools', 'msiexec', 'setup.exe'))]
    jobs_ok = all('powershell.exe' in l.lower() for l in procs if '|job|' in l)
    check('G2', not bad_lines and not started_installers and jobs_ok,
          f'audit: {len(bad_lines)} non-sandbox side effects {bad_lines[:2]}, {len(started_installers)} installers started, jobs only powershell {jobs_ok}')
    if not MUTATE and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    print(f'RESULT pass={sum(results)} fail={len(results) - sum(results)}')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
