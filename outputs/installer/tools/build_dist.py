"""INSTALL19 packager: builds the distributable SkyBreak-<version>.zip (our code + open-source components only).

  1. copy the paths of outputs/installer/package-allowlist.txt into <stage>/bron/ (allow-list, never "all except")
  2. add SkyBreak-Setup.exe, app/SkyBreak.exe + icon (from the locked build), texts, licences, geavanceerd/
  3. legal scan (legal_scan.py): any game-derived file -> no package (exit 3); REL22-CERT: console scan
     (console_scan.py): a real console certificate/keys, SYSCONF, NAND save or Nintendo MAC -> no package (exit 3)
  4. MANIFEST.sha256 (Setup verifies it before installing)
  5. zip -> outputs/vriendenpakket/SkyBreak-<version>.zip (top folder SkyBreak-<version>/)
  6. unpack the zip into work/install19/dist-check and scan that again (what friends receive)

Stdlib only.  python build_dist.py [--build work/install19/build] [--out outputs/vriendenpakket] [--no-check]
"""
import argparse
import fnmatch
import hashlib
import json
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
INSTALLER = HERE.parent
ROOT = INSTALLER.parents[1]
sys.path.insert(0, str(HERE))
import legal_scan  # noqa: E402
import console_scan  # noqa: E402  REL22-CERT: console-unique data (real certificates, keys, SYSCONF, saves)


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def read_allowlist(path):
    dirs, files, excludes = [], [], []
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        kind, _, rest = line.partition(' ')
        rest = rest.strip()
        if kind == 'dir':
            dirs.append(rest)
        elif kind == 'file':
            files.append(rest)
        elif kind == 'exclude':
            excludes.append(rest)
        else:
            raise SystemExit(f'allow-list: unknown line {line!r}')
    return dirs, files, excludes


def excluded(rel, excludes):
    return any(fnmatch.fnmatch(rel, pat) for pat in excludes)


def copy_allowlisted(stage_bron, dirs, files, excludes, runtime_source=None):
    n = 0
    for d in dirs:
        src = Path(runtime_source) if runtime_source and d == 'outputs/nwiirecomp/nWiiRuntime' else ROOT / d
        if not src.is_dir():
            raise SystemExit(f'allow-list dir missing: {d}')
        for p in sorted(src.rglob('*')):
            if not p.is_file():
                continue
            rel = (Path(d) / p.relative_to(src)).as_posix()
            if excluded(rel, excludes):
                continue
            dst = stage_bron / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
            n += 1
    for f in files:
        src = ROOT / f
        if not src.is_file():
            raise SystemExit(f'allow-list file missing: {f}')
        dst = stage_bron / f
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        n += 1
    return n


def fill(text, **kw):
    for k, v in kw.items():
        text = text.replace('{' + k + '}', v)
    return text


def write_crlf(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8'))


def make_manifest(stage):
    lines = []
    for p in sorted(stage.rglob('*')):
        if p.is_file() and p.name != 'MANIFEST.sha256':
            lines.append(f'{sha256(p)} *{p.relative_to(stage).as_posix()}')
    (stage / 'MANIFEST.sha256').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    return len(lines)


def make_zip(stage, zip_path):
    tmp = zip_path.with_suffix('.zip.tmp')
    if tmp.exists():
        tmp.unlink()
    top = stage.name
    with zipfile.ZipFile(tmp, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(stage.rglob('*')):
            if p.is_file():
                info = zipfile.ZipInfo(f'{top}/{p.relative_to(stage).as_posix()}', date_time=time.localtime(p.stat().st_mtime)[:6])
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                with open(p, 'rb') as f:
                    z.writestr(info, f.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    tmp.replace(zip_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', default=str(ROOT / 'work/install19/build'))
    ap.add_argument('--stage-root', default=str(ROOT / 'work/install19/dist'))
    ap.add_argument('--out', default=str(ROOT / 'outputs/vriendenpakket'))
    ap.add_argument('--check-dir', default=str(ROOT / 'work/install19/dist-check'))
    ap.add_argument('--no-check', action='store_true')
    # REVINSTALL19: legal rule D (exact copy of any disc file) uses the local extracted disc when it is there
    ap.add_argument('--disc', default=str(ROOT / 'work/pc-game'))
    ap.add_argument('--runtime-source', help='Use an explicitly tested runtime source directory instead of the development tree')
    a = ap.parse_args()

    version = (INSTALLER / 'VERSION').read_text(encoding='utf-8').strip()
    build = Path(a.build)
    for exe in ('SkyBreak-Setup.exe', 'SkyBreak.exe'):
        if not (build / exe).is_file():
            raise SystemExit(f'{build / exe} missing: run outputs/installer/build-setup.cmd first (via locked.sh)')
    stage = Path(a.stage_root) / f'SkyBreak-{version}'
    if stage.exists():
        # Resolve and verify the deletion target before touching an earlier package.
        resolved=stage.resolve();allowed=(ROOT/'work').resolve()
        if not resolved.is_relative_to(allowed) or resolved==allowed or stage.is_symlink() or not (stage/'MANIFEST.sha256').is_file():
            raise SystemExit('Existing stage is not a verified package within work/: choose an unused --stage-root')
        shutil.rmtree(stage)
    (stage / 'bron').mkdir(parents=True)

    dirs, files, excludes = read_allowlist(INSTALLER / 'package-allowlist.txt')
    n_src = copy_allowlisted(stage / 'bron', dirs, files, excludes, a.runtime_source)
    (stage / 'bron' / 'VERSION').write_text(version + '\n', encoding='utf-8', newline='\n')
    shutil.copy2(INSTALLER / 'prereqs.json', stage / 'bron' / 'prereqs.json')

    shutil.copy2(build / 'SkyBreak-Setup.exe', stage / 'SkyBreak-Setup.exe')
    (stage / 'app').mkdir()
    shutil.copy2(build / 'SkyBreak.exe', stage / 'app' / 'SkyBreak.exe')
    shutil.copy2(ROOT / 'outputs/branding/skybreak.ico', stage / 'app' / 'skybreak.ico')
    setup_sha = sha256(stage / 'SkyBreak-Setup.exe')

    pk = INSTALLER / 'pakket'
    kw = dict(version=version, setup_sha256=setup_sha)
    write_crlf(stage / 'README.txt', fill((pk / 'LEESMIJ.txt').read_text(encoding='utf-8'), **kw))

    shutil.copy2(pk/'COMMUNITY-TESTEN.md',stage/'COMMUNITY-TESTING.md')
    lic = fill((pk / 'LICENTIE-kop.txt').read_text(encoding='utf-8'), **kw) + (ROOT / 'outputs/nwiirecomp/LICENSE').read_text(encoding='utf-8')
    write_crlf(stage / 'LICENSE.txt', lic)
    write_crlf(stage / 'THIRD-PARTY-NOTICES.txt', fill((pk / 'DERDEN-LICENTIES.txt').read_text(encoding='utf-8'), **kw))
    (stage / 'advanced').mkdir()
    for p in sorted((pk / 'geavanceerd').iterdir()):
        name = {'Bouw-SkyBreak.cmd': 'Build-SkyBreak.cmd', 'Installeer-Benodigdheden.cmd': 'Install-Prerequisites.cmd', 'Installeer-Benodigdheden.ps1': 'Install-Prerequisites.ps1'}.get(p.name, p.name)
        text = p.read_text(encoding='utf-8-sig').replace('Installeer-Benodigdheden.ps1', 'Install-Prerequisites.ps1').replace('Bouw-SkyBreak.cmd', 'Build-SkyBreak.cmd')
        (stage / 'advanced' / name).write_text(text, encoding='utf-8-sig' if name.endswith('.ps1') else 'utf-8')

    # legal scan BEFORE the manifest and the zip
    hashes = legal_scan.load_hashes(ROOT / 'outputs/skyward-sword/config/SOUE01')
    allow = {'SkyBreak-Setup.exe', 'app/SkyBreak.exe'}
    disc = legal_scan.DiscIndex(a.disc)  # REVINSTALL19 (empty index when the folder is absent)
    n, ext_count, hits = legal_scan.scan(stage, hashes, allow, disc)
    report = [f'INSTALL19 legal scan of {stage.name} ({time.strftime("%Y-%m-%d %H:%M")})',
              f'files: {n}; known game hashes: {len(hashes)}; disc files indexed: {disc.files}',
              'per extension: ' + ', '.join(f'{k} {v}' for k, v in sorted(ext_count.items(), key=lambda kv: -kv[1])),
              f'hits: {len(hits)}'] + hits + ['RESULT ' + ('PASS (0 game-derived files)' if not hits else f'FAIL ({len(hits)} hits)')]
    (stage / 'LEGAL-SCAN.txt').write_text('\n'.join(report) + '\n', encoding='utf-8', newline='\n')
    if hits:
        print('\n'.join(report[:4] + hits[:40]))
        print('PACKAGE REFUSED: game-derived files found (see LEGAL-SCAN.txt)')
        return 3
    _, console_hits = console_scan.scan(stage)  # REL22-CERT
    if console_hits:
        print('\n'.join(console_hits[:40]))
        print('PACKAGE REFUSED: console-unique data found (console_scan.py: real certificate/keys/SYSCONF/save/MAC)')
        return 3

    n_manifest = make_manifest(stage)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    zip_path = out / f'SkyBreak-{version}.zip'
    make_zip(stage, zip_path)
    summary = {'version': version, 'zip': str(zip_path), 'zip_bytes': zip_path.stat().st_size, 'zip_sha256': sha256(zip_path),
               'setup_sha256': setup_sha, 'files': n_manifest, 'bron_files': n_src, 'legal_hits': 0}

    if not a.no_check:
        chk = Path(a.check_dir)
        if chk.exists():
            resolved=chk.resolve();allowed=(ROOT/'work').resolve()
            if not resolved.is_relative_to(allowed) or resolved==allowed or chk.is_symlink() or not any(chk.glob('SkyBreak-*/MANIFEST.sha256')):
                raise SystemExit('Existing check directory is not a verified package within work/: choose an unused --check-dir')
            shutil.rmtree(chk)
        chk.mkdir(parents=True)
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(chk)
        n2, _, hits2 = legal_scan.scan(chk, hashes, {f'SkyBreak-{version}/SkyBreak-Setup.exe', f'SkyBreak-{version}/app/SkyBreak.exe'}, disc)
        summary['check_dir'] = str(chk / f'SkyBreak-{version}')
        summary['check_files'] = n2
        summary['check_legal_hits'] = len(hits2)
        summary['disc_files_indexed'] = disc.files  # REVINSTALL19
        _, console_hits2 = console_scan.scan(chk)  # REL22-CERT
        summary['check_console_hits'] = len(console_hits2)
        hits2 = hits2 + console_hits2
        if hits2:
            print('\n'.join(hits2[:40]))
            zip_path.unlink(missing_ok=True)  # REVINSTALL19: never leave a refused zip in the share folder
            print('UNPACKED ZIP FAILS THE LEGAL SCAN (zip removed)')
            return 3
    (Path(a.stage_root) / f'SkyBreak-{version}.summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
