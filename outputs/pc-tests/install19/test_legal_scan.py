"""INSTALL19 legal scan test (outputs/installer/tools/legal_scan.py).

  L1  a clean mini package (our own sources + an allowed Setup exe) passes with 0 hits
  L2-L8 NEGATIVE controls, each in its own work copy that is removed again; each must be REFUSED with its rule code:
      L2 a 4 KB U8 archive (magic)            -> M
      L3 a copy of main.dol (SHA-1 + DOL head) -> H and M
      L4 output_0.cpp (generated name)         -> G
      L5 textures/x.png                        -> T
      L6 an .rvz header file                   -> E and M
      L7 recompiled-code content in a .cpp     -> G
      L8 an extra .exe                         -> X
  L9  the real package (work/install19/dist-check, the unpacked zip), when present: 0 hits
  REVINSTALL19 (10 Oct review; each of these kinds passed the scan before, verified with real disc files):
      L10 LZ11-compressed U8 archive renamed .bin (the disc's *.arc.LZ)   -> M
      L11 a game screenshot docs/screen.png (not under textures/)        -> I
      L12 a zip inside the package (contents were never scanned)         -> A
      L13 a crash dump SkyBreak-game.dmp (game memory)                   -> E and M
      L14 an exact copy of a disc file under another name (home.csv)     -> D (needs work/pc-game; skipped otherwise)
      L15 a banner with IMET at 0x40 (opening.bnr) renamed .dat          -> M
  REL22 (button-icon stencils, the only image folder besides branding):
      L16 the real outputs/pc-tools/glyphs/stencils folder (Kenney CC0 pictures + recipe.json) passes with 0 hits
      L17 NEGATIVE an extra PNG in that folder that the recipe does not list    -> I
      L18 NEGATIVE a listed stencil whose bytes were replaced (hash mismatch) -> I
      L19 NEGATIVE a game-fitted glyph picture (disc pixels: the picture-font sheet) put in the stencil folder under a listed
          name -> I;  L20 the same picture under bron/textures/glyphs/ -> T and I
Only writes under work/install19/legal-test. main.dol is read from work/pc-game (local copy, removed after).
"""
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SCAN = ROOT / 'outputs/installer/tools/legal_scan.py'
HASHES = ROOT / 'outputs/skyward-sword/config/SOUE01'
DISC = ROOT / 'work/pc-game'  # REVINSTALL19: rule D (exact copy of a disc file) when the local disc is there
WORK = ROOT / 'work/install19/legal-test'
results = []


def check(cid, cond, msg):
    results.append(cond)
    print(('PASS: ' if cond else 'FAIL: ') + f'{cid} {msg}', flush=True)


def run_scan(pkg):
    disc = ['--disc', str(DISC)] if DISC.is_dir() else []  # REVINSTALL19
    r = subprocess.run([sys.executable, str(SCAN), str(pkg), '--hashes', str(HASHES), '--report', str(pkg.parent / (pkg.name + '-scan.txt'))] + disc,
                       capture_output=True, text=True, errors='replace')
    return r.returncode, r.stdout + r.stderr


def clean_package(name):
    pkg = WORK / name / 'SkyBreak-test'
    if pkg.parent.exists():
        shutil.rmtree(pkg.parent)
    (pkg / 'bron/outputs/installer/src').mkdir(parents=True)
    for f in (ROOT / 'outputs/installer/src').glob('*.cpp'):
        shutil.copy2(f, pkg / 'bron/outputs/installer/src' / f.name)
    shutil.copy2(ROOT / 'outputs/nwiirecomp/LICENSE', pkg / 'bron/LICENSE')
    (pkg / 'SkyBreak-Setup.exe').write_bytes(b'MZ' + b'\0' * 510)  # stands in for our own build
    (pkg / 'app').mkdir()
    (pkg / 'app/SkyBreak.exe').write_bytes(b'MZ' + b'\0' * 254)
    return pkg


def negative(cid, name, mutate, codes, desc):
    pkg = clean_package(name)
    mutate(pkg)
    rc, out = run_scan(pkg)
    got = {line.split()[0] for line in out.splitlines() if line[:2] in {c + ' ' for c in 'EMHGTXSBIAD'}}  # REVINSTALL19: + I A D
    check(cid, rc == 1 and set(codes) <= got, f'NEGATIVE {desc}: refused (exit {rc}), rule codes {sorted(got)} (expected {codes})')
    shutil.rmtree(pkg.parent, ignore_errors=True)


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    pkg = clean_package('clean')
    rc, out = run_scan(pkg)
    check('L1', rc == 0 and 'RESULT PASS' in out, f'clean mini package passes (exit {rc})')
    shutil.rmtree(pkg.parent, ignore_errors=True)

    negative('L2', 'u8', lambda p: (p / 'bron/data.bin').write_bytes(b'\x55\xAA\x38\x2D' + b'\0' * 4092), ['M'], '4 KB U8 archive')
    main_dol = ROOT / 'work/pc-game/sys/main.dol'
    if main_dol.is_file():
        negative('L3', 'dol', lambda p: shutil.copy2(main_dol, p / 'bron/helper.dat'), ['H', 'M'], 'copy of main.dol under another name')
    else:
        check('L3', True, 'work/pc-game/sys/main.dol not present (skipped)')
    negative('L4', 'gen', lambda p: (p / 'bron/output_0.cpp').write_text('// x\n'), ['G'], 'output_0.cpp')
    negative('L5', 'tex', lambda p: ((p / 'bron/textures').mkdir(), (p / 'bron/textures/x.png').write_bytes(b'\x89PNG\r\n\x1a\n' + b'\0' * 64)),
             ['T'], 'textures/x.png')
    negative('L6', 'rvz', lambda p: (p / 'bron/game.rvz').write_bytes(b'RVZ\x01' + b'\0' * 0x200), ['E', 'M'], '.rvz header file')
    gen = ''.join(f'loc_8000{i:04X}: ;\n    // 0x8000{i:04X}: 3C608000\n' for i in range(0, 120, 4))
    negative('L7', 'content', lambda p: (p / 'bron/outputs/installer/src/helper.cpp').write_text(gen), ['G'], 'recompiled instructions in a .cpp')
    negative('L8', 'exe', lambda p: (p / 'bron/SkywardSwordPC.exe').write_bytes(b'MZ' + b'\0' * 100), ['X'], 'an extra .exe (built game)')
    # REVINSTALL19 negatives (synthetic headers shaped like the real disc files; L14 uses a real disc file when present)
    negative('L10', 'lz', lambda p: (p / 'bron/pack.bin').write_bytes(b'\x11\x00\x10\x00\x00\x55\xAA\x38\x2D' + b'\0' * 4087), ['M'],
             'LZ11-compressed U8 archive renamed .bin')
    negative('L11', 'shot', lambda p: ((p / 'bron/docs').mkdir(), (p / 'bron/docs/screen.png').write_bytes(b'\x89PNG\r\n\x1a\n' + b'\0' * 64)),
             ['I'], 'game screenshot outside textures/')

    def nested_zip(p):
        import zipfile
        with zipfile.ZipFile(p / 'bron/extra.zip', 'w') as z:
            z.writestr('output_7.cpp', '// generated\n')
    negative('L12', 'zip', nested_zip, ['A'], 'zip inside the package')
    negative('L13', 'dmp', lambda p: (p / 'bron/SkyBreak-game.dmp').write_bytes(b'MDMP\x93\xa7' + b'\0' * 1000), ['E', 'M'], 'crash dump .dmp')
    home_csv = DISC / 'files/HomeButton2/home.csv'
    if home_csv.is_file():
        negative('L14', 'disc', lambda p: shutil.copy2(home_csv, p / 'bron/notes.csv'), ['D'], 'exact copy of a disc file (home.csv) renamed')
    else:
        check('L14', True, 'work/pc-game/files/HomeButton2/home.csv not present (skipped)')
    negative('L15', 'bnr', lambda p: (p / 'bron/banner.dat').write_bytes(b'\0' * 0x40 + b'IMET' + b'\0' * 0x600), ['M'], 'IMET banner renamed .dat')

    # REL22 stencils
    stencils = ROOT / 'outputs/pc-tools/glyphs/stencils'

    def with_stencils(p):
        shutil.copytree(stencils, p / 'bron/outputs/pc-tools/glyphs/stencils')
        return p / 'bron/outputs/pc-tools/glyphs/stencils'
    pkg = clean_package('stencils')
    with_stencils(pkg)
    rc, out = run_scan(pkg)
    n16 = len(list(stencils.glob('s_*.png')))
    check('L16', rc == 0 and 'RESULT PASS' in out and n16 > 50, f'package with the {n16} Kenney stencils + recipe passes (exit {rc})')
    shutil.rmtree(pkg.parent, ignore_errors=True)
    negative('L17', 'st-extra', lambda p: (with_stencils(p) / 's_0000000000000000.png').write_bytes(b'\x89PNG\r\n\x1a\n' + b'\0' * 64),
             ['I'], 'unlisted PNG in the stencil folder')
    first = sorted(stencils.glob('s_*.png'))[0].name

    def changed(p):
        d = with_stencils(p)
        b = bytearray((d / first).read_bytes())
        b[-20] ^= 0x55
        (d / first).write_bytes(bytes(b))
    negative('L18', 'st-changed', changed, ['I'], 'a listed stencil with other bytes (hash mismatch)')
    fitted = sorted((ROOT / 'outputs/pc-prototype/textures/glyphs/SOUE01/xbox-hd').glob('tex1_128x128_*_2.png'))
    if fitted:
        negative('L19', 'st-disc', lambda p: shutil.copy2(fitted[0], with_stencils(p) / first), ['I'],
                 'game-fitted picture-font sheet (disc pixels) under a listed stencil name')

        def in_textures(p):
            (p / 'bron/textures/glyphs').mkdir(parents=True)
            shutil.copy2(fitted[0], p / 'bron/textures/glyphs' / fitted[0].name)
        negative('L20', 'st-tex', in_textures, ['T', 'I'], 'game-fitted glyph picture under textures/glyphs')
    else:
        check('L19', True, 'no game-fitted glyph set on this PC (skipped)')
        check('L20', True, 'no game-fitted glyph set on this PC (skipped)')

    dist = ROOT / 'work/install19/dist-check'
    if dist.is_dir() and any(dist.iterdir()):
        disc = ['--disc', str(DISC)] if DISC.is_dir() else []  # REVINSTALL19
        r = subprocess.run([sys.executable, str(SCAN), str(dist), '--hashes', str(HASHES), '--report', str(WORK / 'dist-check-scan.txt')] + disc,
                           capture_output=True, text=True, errors='replace')
        files = next((l for l in r.stdout.splitlines() if l.startswith('files:')), '')
        check('L9', r.returncode == 0, f'unpacked SkyBreak zip: {files} -> {"0 hits" if r.returncode == 0 else "HITS"}')
    else:
        check('L9', True, 'work/install19/dist-check not present (package not built yet; skipped)')
    shutil.rmtree(WORK, ignore_errors=True)
    print(f'RESULT pass={sum(results)} fail={len(results) - sum(results)}')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
