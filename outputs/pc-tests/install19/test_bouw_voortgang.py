"""INSTALL19: JSON progress of Bouw-SkywardSword.ps1 (-VoortgangJson), the stream SkyBreak-Setup reads.

  B1  -AlleenControle on work/pc-game: start/klaar for step 1 and 2, then 'einde'; every line valid JSON (UTF-8, no BOM)
  B2  without -VoortgangJson nothing is written (default behaviour unchanged)
  B3  NEGATIVE PAL folder: a 'fout' line with soort=schijf and the PAL sentence, exit 1
  B4  NEGATIVE missing disc path: 'fout' with soort=schijf before step 1
  B5  (REVINSTALL19) Quote-Arg -Cmd + cmd.exe /d /s /c exactly like Start-Gelogd: a tool in a folder 'amp&test' (an '&' and no
      space, e.g. C:\\Users\\Tom&Jerry\\...) really runs and logs; NEGATIVE: the same call without -Cmd (= the pre-review quoting)
      must NOT run it. Guards: Start-Gelogd uses -Cmd; plain Quote-Arg keeps '2>&1' and '&&' unquoted (Start-Direct's VsDevCmd call
      relies on them: quoting them broke step 1 in the first REVINSTALL19 attempt)
  B6  (REVINSTALL19 continuation) non-ASCII folder ('Zoë 日 (x), t': an accent + a character outside the ANSI code page): the real
      Start-Gelogd (chcp 65001 first) configures + builds a tiny CMake/Ninja/cl project; the SECOND build is a no-op ('no work to
      do') and a touched header rebuilds. NEGATIVE: the pre-fix Start-Gelogd (no chcp) -> the second build is NOT a no-op
      (accent: everything rebuilt every time; outside the code page: 'ninja: error: FindFirstFileExA'). Needs VS 2022 C++ + work/venv
      cmake/ninja; compiles one file.
  B7  (REL22) the REAL Draaiende-SkyBreak + Wacht-Op-Gesloten of the build script: a test double runs ~5 s as
      <Uitvoer>\\bin\\SkyBreak-game.exe of a folder 'Spellen\\Zéldà ✦ test\\SkyBreak' -> listed by full path, the script waits
      ('wacht' JSON line with that path, then 'verder') and goes on once it has closed; a copy with the same name in ANOTHER folder
      is never listed and stays alive (nothing is killed). NEGATIVE: a name-only variant of the check lists both.
  B8  (REL22) -SluitWacht 3 while <Uitvoer>\\SkyBreak.exe keeps running: Fout 'SkyBreak draait nog ...', Fout-Soort = 'draait'
      (Setup shows its hint), the program is still running afterwards
Nothing is extracted; only B6 compiles (one file); B7/B8 start a harmless test double (dummy_proc.py); writes only under
work/install19/voortgang-test.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BOUW = ROOT / 'outputs/release/Bouw-SkywardSword.ps1'
GAME = ROOT / 'work/pc-game'
TMP = ROOT / 'work/install19/voortgang-test'
results = []
sys.stdout.reconfigure(errors='backslashreplace')  # REVINSTALL19 continuation: B6 messages may hold non-ASCII; run-all pipes cp1252


def check(cid, cond, msg):
    results.append(cond)
    print(('PASS: ' if cond else 'FAIL: ') + f'{cid} {msg}', flush=True)


VSDEV = Path(r'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat')  # as build-install19-tests.cmd


def start_gelogd_twice(script_src, tag):
    # REVINSTALL19 continuation (B6): the REAL Start-Gelogd (+ Quote-Arg, Log-Staart) from the build script runs configure, build,
    # build again, touch header, build; paths go in through the environment (a .ps1 without BOM is read in the ANSI code page)
    fns = '\n'.join(re.search(r'(?ms)^function ' + n + r'\b.*?^\}', script_src).group(0) for n in ('Quote-Arg', 'Log-Staart', 'Start-Gelogd'))
    base = TMP / f'Zoë 日 (x), t {tag}'
    srcd, bld, log = base / 'bron', base / 'bouw map', base / 'bouw.log'
    (srcd / 'inc').mkdir(parents=True, exist_ok=True)
    (srcd / 'inc/probe.h').write_text('#pragma once\n#define PROBE_VALUE 41\n', encoding='utf-8')
    (srcd / 'main.cpp').write_text('#include "probe.h"\nint main(){return PROBE_VALUE == 41 ? 0 : 1;}\n', encoding='utf-8')
    (srcd / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(probe CXX)\nadd_executable(probe main.cpp)\n'
                                         'target_include_directories(probe PRIVATE inc)\n', encoding='utf-8')
    ps1 = TMP / f'b6-{tag}.ps1'
    ps1.write_text(fns + """
function Voortgang { }
$script:LaatsteJson = ''
$cm = $env:B6_CMAKE
$c1 = Start-Gelogd $cm @('-S', $env:B6_SRC, '-B', $env:B6_BLD, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$env:B6_NINJA") $env:B6_LOG 'cfg'
$c2 = Start-Gelogd $cm @('--build', $env:B6_BLD) $env:B6_LOG 'b1'
Add-Content -LiteralPath $env:B6_LOG -Value '=== B6 SECOND ===' -Encoding UTF8
$c3 = Start-Gelogd $cm @('--build', $env:B6_BLD) $env:B6_LOG 'b2'
Add-Content -LiteralPath $env:B6_LOG -Value '=== B6 TOUCH ===' -Encoding UTF8
Start-Sleep -Milliseconds 1100
Set-Content -LiteralPath $env:B6_HDR -Value '#pragma once' -Encoding ASCII
Add-Content -LiteralPath $env:B6_HDR -Value '#define PROBE_VALUE 41' -Encoding ASCII
$c4 = Start-Gelogd $cm @('--build', $env:B6_BLD) $env:B6_LOG 'b3'
"B6RC $c1 $c2 $c3 $c4"
""", encoding='utf-8-sig')  # BOM: PowerShell 5.1 reads a .ps1 without one in the ANSI code page
    venv = ROOT / 'work/venv/Scripts'
    env = dict(os.environ, B6_CMAKE=str(venv / 'cmake.exe'), B6_NINJA=(venv / 'ninja.exe').as_posix(), B6_SRC=str(srcd),
               B6_BLD=str(bld), B6_LOG=str(log), B6_HDR=str(srcd / 'inc/probe.h'))
    line = f'"{VSDEV}" -arch=amd64 -host_arch=amd64 >nul && powershell -NoProfile -ExecutionPolicy Bypass -File "{ps1}"'
    r = subprocess.run('cmd /d /s /c "' + line + '"', capture_output=True, text=True, errors='replace', env=env)
    m = re.search(r'B6RC (\S+) (\S+) (\S+) (\S+)', r.stdout)
    text = log.read_text(encoding='utf-8', errors='replace') if log.is_file() else ''
    second = text.split('=== B6 SECOND ===')[-1].split('=== B6 TOUCH ===')[0] if '=== B6 SECOND ===' in text else ''
    touched = text.split('=== B6 TOUCH ===')[-1] if '=== B6 TOUCH ===' in text else ''
    rcs = m.groups() if m else ('?',) * 4
    noop = 'no work to do' in second and rcs[2] == '0'
    rebuilt = 'main.cpp' in touched and rcs[3] == '0'
    ok = rcs[:2] == ('0', '0') and noop and rebuilt and not str(base).isascii()
    err = [l.strip() for l in second.splitlines() if 'error' in l.lower()][:1]
    return ok, f'exit codes {list(rcs)}, second build no-op {noop}, header touch rebuilt {rebuilt}{" " + repr(err[0][:120]) if err else ""}'


WACHT_PS = r"""
$ErrorActionPreference = 'Stop'
$VoortgangJson = $env:B7_JSON
$SluitWacht = [int]$env:B7_WACHT
$totaalKlok = [Diagnostics.Stopwatch]::StartNew()
$script:HuidigeStap = 8
$script:HuidigeNaam = '8. test'
function Fout([string]$Melding) { throw [System.Exception]::new($Melding) }
%FUNCS%
$lijst = @(Draaiende-SkyBreak $env:B7_INST)
$k = [Diagnostics.Stopwatch]::StartNew()
$uit = @('LIST ' + (($lijst | ForEach-Object { $_.Pad }) -join '|'))
try { Wacht-Op-Gesloten $env:B7_INST; $uit += "WAIT ok $([int]$k.Elapsed.TotalMilliseconds)" }
catch { $uit += "WAIT fout $([int]$k.Elapsed.TotalMilliseconds) $(Fout-Soort $_.Exception) $($_.Exception.Message)" }
[IO.File]::WriteAllText($env:B7_OUT, ($uit -join "`n"), (New-Object System.Text.UTF8Encoding($false)))
"""


def wacht_run(script_src, inst, wacht, tag, name_only=False):
    # REL22 (B7/B8): the real functions of the build script, extracted by name (multi-line definitions only)
    fns = [re.search(r'(?ms)^function ' + re.escape(n) + r'\(.*?^\}', script_src).group(0)
           for n in ('Voortgang', 'Fout-Soort', 'Draaiende-SkyBreak', 'Wacht-Op-Gesloten')]
    if name_only:  # NEGATIVE: what a check by process NAME would see
        fns[2] = fns[2].replace("if ([string]::Equals($p.Pad, $start, [StringComparison]::OrdinalIgnoreCase) -or",
                                "if ((Split-Path -Leaf $p.Pad) -in @('SkyBreak.exe', 'SkyBreak-game.exe') -or")
    ps1 = TMP / f'wacht-{tag}.ps1'
    ps1.write_text(WACHT_PS.replace('%FUNCS%', '\n'.join(fns)), encoding='ascii')
    out, js = TMP / f'wacht-{tag}.txt', TMP / f'wacht-{tag}.jsonl'
    for p in (out, js):
        p.unlink(missing_ok=True)
    env = dict(os.environ, B7_JSON=str(js), B7_WACHT=str(wacht), B7_INST=str(inst), B7_OUT=str(out))
    subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(ps1)], capture_output=True, text=True,
                   env=env, timeout=180)
    lines = out.read_text(encoding='utf-8').splitlines() if out.is_file() else []
    listed = lines[0][5:].split('|') if lines and lines[0].startswith('LIST ') and lines[0][5:] else []
    wait = lines[1].split(' ', 4) if len(lines) > 1 else []
    _, ev = events(js) if js.is_file() else (b'', [])
    return [str(Path(x)) for x in listed], wait, ev


def bouw(iso, progress=None):
    args = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(BOUW), '-Iso', str(iso),
            '-Werkmap', str(TMP / 'bouw'), '-Uitvoer', str(TMP / 'uit'), '-AlleenControle']
    if progress:
        args += ['-VoortgangJson', str(progress)]
    r = subprocess.run(args, capture_output=True, text=True, errors='replace')
    return r.returncode, r.stdout + r.stderr


def events(p):
    raw = p.read_bytes() if p.exists() else b''
    lines = [l for l in raw.decode('utf-8').splitlines() if l.strip()]
    return raw, [json.loads(l) for l in lines]


def run_like_start_gelogd(func_src, tool, log, flag=' -Cmd'):
    """REVINSTALL19: build the command line with Quote-Arg (flag ' -Cmd' like Start-Gelogd, '' = pre-review) and start it via cmd.exe."""
    if log.exists():
        log.unlink()
    ps = (func_src + "\n$regel = (Quote-Arg '" + str(tool) + "'" + flag + ") + ' ' + (Quote-Arg 'arg1'" + flag + ")\n"
          "$psi = New-Object System.Diagnostics.ProcessStartInfo; $psi.FileName = $env:ComSpec\n"
          "$psi.Arguments = '/d /s /c \"' + $regel + ' >> \"" + str(log) + "\" 2>&1\"'\n"
          "$psi.UseShellExecute = $false; $psi.CreateNoWindow = $true\n"
          "$p = [System.Diagnostics.Process]::Start($psi); $p.WaitForExit(); exit $p.ExitCode\n")
    subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', ps], capture_output=True, text=True,
                   errors='replace', timeout=120)
    return log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''


def main():
    shutil.rmtree(TMP, ignore_errors=True)
    TMP.mkdir(parents=True)
    prog = TMP / 'voortgang.jsonl'
    rc, out = bouw(GAME, prog)
    raw, ev = events(prog)
    seq = [(e.get('stap'), e.get('status')) for e in ev]
    check('B1', rc == 0 and not raw.startswith(b'\xef\xbb\xbf') and seq[:4] == [(1, 'start'), (1, 'klaar'), (2, 'start'), (2, 'klaar')]
          and ev[-1].get('status') == 'einde' and all('t' in e for e in ev),
          f'-AlleenControle work/pc-game: exit {rc}, events {seq}')
    before = raw
    rc2, _ = bouw(GAME)
    jsonl = sorted(p.name for p in TMP.rglob('*.jsonl'))
    check('B2', rc2 == 0 and jsonl == ['voortgang.jsonl'] and prog.read_bytes() == before,
          f'without -VoortgangJson: nothing written (jsonl files {jsonl})')

    pal = TMP / 'fake/pal'
    (pal / 'sys').mkdir(parents=True)
    b = bytearray(0x440)
    b[0:6] = b'SOUP01'
    struct.pack_into('>I', b, 0x18, 0x5D1C9EA3)
    (pal / 'sys/boot.bin').write_bytes(bytes(b))
    (pal / 'sys/main.dol').write_bytes(b'x' * 64)
    prog3 = TMP / 'pal.jsonl'
    rc3, out3 = bouw(pal, prog3)
    _, ev3 = events(prog3)
    fout = [e for e in ev3 if e.get('status') == 'fout']
    check('B3', rc3 == 1 and len(fout) == 1 and fout[0].get('soort') == 'schijf' and 'Europese (PAL)' in fout[0].get('msg', '') and fout[0].get('stap') == 2,
          f'NEGATIVE PAL folder: exit {rc3}, fout {fout[:1]}')
    prog4 = TMP / 'missing.jsonl'
    rc4, _ = bouw(TMP / 'bestaat-niet.iso', prog4)
    _, ev4 = events(prog4)
    check('B4', rc4 == 1 and ev4 and ev4[-1].get('status') == 'fout' and ev4[-1].get('soort') == 'schijf' and ev4[-1].get('stap') == 0,
          f'NEGATIVE missing disc: exit {rc4}, last event {ev4[-1:]}')

    # B5 (REVINSTALL19) cmd.exe metacharacter in a path without spaces
    src = BOUW.read_text(encoding='utf-8')
    fn = re.search(r'(?ms)^function Quote-Arg.*?^\}', src).group(0)
    gelogd = re.search(r'(?ms)^function Start-Gelogd.*?^\}', src).group(0)
    uses_cmd = 'Quote-Arg $Exe -Cmd' in gelogd and 'Quote-Arg $_ -Cmd' in gelogd
    amp = TMP / 'amp&test'
    amp.mkdir(parents=True)
    tool = amp / 'hulp.cmd'
    tool.write_bytes(b'@echo OK-AMP %1\r\n')
    new_log = run_like_start_gelogd(fn, tool, TMP / 'amp-new.log', ' -Cmd')
    old_log = run_like_start_gelogd(fn, tool, TMP / 'amp-old.log', '')  # NEGATIVE: pre-review quoting
    ops = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command',
                          fn + "\n(@('2>&1', '&&', '>nul') | ForEach-Object { Quote-Arg $_ }) -join ' '"],
                         capture_output=True, text=True, errors='replace').stdout.strip()
    check('B5', ' ' not in str(tool) and 'OK-AMP arg1' in new_log and 'OK-AMP' not in old_log and uses_cmd and ops == '2>&1 && >nul',
          f"path with '&' and no space via cmd /c: Quote-Arg -Cmd runs it {'OK-AMP arg1' in new_log}; NEGATIVE without -Cmd runs it "
          f"{'OK-AMP' in old_log} (must be False); Start-Gelogd uses -Cmd {uses_cmd}; Start-Direct operators unquoted: {ops!r}")
    # B6 (REVINSTALL19 continuation) non-ASCII build folder through the real Start-Gelogd
    new_ok, new_msg = start_gelogd_twice(src, 'new')
    old_src = src.replace("'/d /s /c \"chcp 65001 >nul & ' + $regel", "'/d /s /c \"' + $regel")
    old_ok, old_msg = start_gelogd_twice(old_src, 'old') if old_src != src else (True, 'pre-fix variant not derivable')
    check('B6', new_ok and not old_ok, f'non-ASCII folder via Start-Gelogd: {new_msg}; NEGATIVE pre-fix (no chcp): {old_msg} (must not be a no-op)')

    # B7 / B8 (REL22) running SkyBreak of THIS output folder: wait (never kill), by full path
    from dummy_proc import Dummy
    inst = TMP / 'Spellen' / 'Zéldà ✦ test' / 'SkyBreak'
    other = TMP / 'andere map' / 'SkyBreak'
    game, game_other = inst / 'bin/SkyBreak-game.exe', other / 'bin/SkyBreak-game.exe'
    with Dummy(game_other, seconds=60) as d_other:
        with Dummy(game, seconds=5):
            listed, wait, ev = wacht_run(src, inst, 60, 'b7')
        with Dummy(game, seconds=5):
            listed_neg, _, _ = wacht_run(src, inst, 0, 'b7neg', name_only=True)
        other_alive = d_other.proc.poll() is None
    st = [e.get('status') for e in ev]
    wacht_msg = next((e.get('msg', '') for e in ev if e.get('status') == 'wacht'), '')
    ms = int(wait[1]) if len(wait) > 1 and wait[1].isdigit() else -1
    check('B7', listed == [str(game)] and wait[:1] == ['ok'] and 3000 <= ms <= 30000 and st[:1] == ['wacht'] and 'verder' in st
          and str(game) in wacht_msg and other_alive and str(game_other) in listed_neg and str(game) in listed_neg,
          f'listed {[Path(x).parent.parent.name for x in listed]} (want only this folder), waited {ms} ms then went on, events {st}, '
          f'the copy in another folder never listed and still alive {other_alive}; NEGATIVE name-only check also lists the other folder {str(game_other) in listed_neg} (must be True)')
    start = inst / 'SkyBreak.exe'
    with Dummy(start, seconds=60) as d8:
        _, wait8, ev8 = wacht_run(src, inst, 3, 'b8')
        alive8 = d8.proc.poll() is None
    ms8 = int(wait8[1]) if len(wait8) > 1 and wait8[1].isdigit() else -1
    check('B8', wait8[:1] == ['fout'] and len(wait8) > 3 and wait8[2] == 'draait' and 'SkyBreak draait nog' in ' '.join(wait8[3:])
          and 2500 <= ms8 <= 20000 and alive8,
          f'-SluitWacht 3 while SkyBreak.exe runs: {" ".join(wait8[:3])} after {ms8} ms, program still running {alive8}')
    shutil.rmtree(TMP, ignore_errors=True)
    print(f'RESULT pass={sum(results)} fail={len(results) - sum(results)}')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
