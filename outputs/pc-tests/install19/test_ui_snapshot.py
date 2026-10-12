"""INSTALL19: SkyBreak-Setup UI snapshots, rendered offscreen on WARP (the Direct3D 11 software renderer: no GPU).

  U1  --ui-snapshot writes all 11 pages + the consent page + (REL22) the "SkyBreak is still running" popup in NL and EN
      (26 PNG, valid, 1280x800)
  U2  every page differs per language and the pages differ from each other (no blank or duplicated frames)
  U3  the SkyBreak logo (embedded RCDATA) is drawn top-left: warm red pixels in the logo box
  U4  gold focus/accent colour present (step marker, active step) on every page
  U5  the consent page (over Benodigdheden) is drawn in NL and EN
  U6  REL22: the "SkyBreak is still running" popup (over Overzicht) is drawn in NL and EN
  test_ui_snapshot.py --mutate -> negative control: Setup started with --mutate no-logo -> U3 must FAIL
Writes PNGs to $NWII_TEST_EVIDENCE_DIR/install19 (run_all) or outputs/pc-testresultaten/install19. Runs in --sandbox
(work/install19/snap-sandbox) so the Folders page shows sandbox paths and nothing outside it is touched.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SETUP = ROOT / 'work/install19/build/SkyBreak-Setup.exe'
MUTATE = '--mutate' in sys.argv
OUT = Path(os.environ['NWII_TEST_EVIDENCE_DIR']) / 'install19' if os.environ.get('NWII_TEST_EVIDENCE_DIR') else ROOT / 'outputs/pc-testresultaten/install19'
if MUTATE:
    OUT = ROOT / 'work/install19/snap-mutant'
results = []


def check(cid, cond, msg):
    results.append(cond)
    print(('PASS: ' if cond else 'FAIL: ') + f'{cid} {msg}', flush=True)


def read_png(p):
    data = p.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            bpp = {6: 4, 2: 3}[ctype]
        elif typ == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, bpp, rows


def count(rows, bpp, x0, y0, x1, y1, pred):
    n = 0
    for y in range(y0, y1):
        r = rows[y]
        for x in range(x0, x1):
            px = r[x * bpp:x * bpp + 3]
            if pred(px[0], px[1], px[2]):
                n += 1
    return n


def main():
    if not SETUP.is_file():
        print('FAIL: U1 work/install19/build/SkyBreak-Setup.exe missing')
        return 1
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True, exist_ok=True)
    sbx = ROOT / 'work/install19/snap-sandbox'
    args = [str(SETUP), '--ui-snapshot', str(OUT), '--warp', '--sandbox', str(sbx), '--package', str(ROOT / 'outputs/installer')]
    if MUTATE:
        args += ['--mutate', 'no-logo']
    r = subprocess.run(args, capture_output=True, timeout=600)
    pngs = sorted(OUT.glob('*.png'))
    sizes_ok = []
    for p in pngs:
        head = p.read_bytes()[:24]
        sizes_ok.append(head[:8] == b'\x89PNG\r\n\x1a\n' and struct.unpack('>II', head[16:24]) == (1280, 800))
    check('U1', r.returncode == 0 and len(pngs) == 26 and all(sizes_ok), f'{len(pngs)} PNG (exit {r.returncode}) in {OUT}')  # REL22: 24 -> 26
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in pngs}
    pages = sorted({p.name.split('-', 1)[1] for p in pngs})
    lang_diff = all(hashes.get('nl-' + pg) != hashes.get('en-' + pg) for pg in pages)
    check('U2', len(set(hashes.values())) == len(hashes) == 26 and lang_diff, f'{len(set(hashes.values()))} distinct images, NL != EN on all {len(pages)} pages')
    red_ok, gold_ok = [], []
    for p in pngs:
        if '-4b-' in p.name or '-5b-' in p.name:
            continue  # the consent / running dialogs dim the page behind them (checked in U5 / U6)
        w, h, bpp, rows = read_png(p)
        red_ok.append(count(rows, bpp, 24, 14, 100, 90, lambda r, g, b: r > 170 and g < 110 and b < 100) > 150)
        gold_ok.append(count(rows, bpp, 24, 110, 260, 460, lambda r, g, b: r > 220 and 180 < g < 240 and 100 < b < 170) > 40)
    check('U3', len(red_ok) == 22 and all(red_ok), f'SkyBreak logo drawn on {sum(red_ok)}/22 pages')
    check('U4', len(gold_ok) == 22 and all(gold_ok), f'gold accent in the step list on {sum(gold_ok)}/22 pages')
    dlg = []
    for lang in ('nl', 'en'):
        c, base = OUT / f'{lang}-4b-toestemming.png', OUT / f'{lang}-4-benodigdheden.png'
        if c.is_file() and base.is_file():
            w, h, bpp, rows = read_png(c)
            # a bright modal in the middle of a dimmed page: parchment text pixels in the centre band
            dlg.append(count(rows, bpp, 340, 200, 940, 600, lambda r, g, b: r > 200 and g > 190 and b > 150) > 400 and c.read_bytes() != base.read_bytes())
    check('U5', len(dlg) == 2 and all(dlg), f'consent page (what, publisher, source, size, licence, admin, changes) drawn in NL and EN: {dlg}')
    run = []  # REL22
    for lang in ('nl', 'en'):
        c, base = OUT / f'{lang}-5b-draait-nog.png', OUT / f'{lang}-5-overzicht.png'
        if c.is_file() and base.is_file():
            w, h, bpp, rows = read_png(c)
            run.append(count(rows, bpp, 340, 200, 940, 600, lambda r, g, b: r > 200 and g > 190 and b > 150) > 300
                       and count(rows, bpp, 320, 150, 960, 650, lambda r, g, b: r > 220 and 150 < g < 200 and b < 110) > 20
                       and c.read_bytes() != base.read_bytes())
    check('U6', len(run) == 2 and all(run), f'"SkyBreak is still running" popup (text + amber list + buttons) drawn in NL and EN: {run}')
    shutil.rmtree(sbx, ignore_errors=True)
    if MUTATE:
        shutil.rmtree(OUT, ignore_errors=True)
    print(f'RESULT pass={sum(results)} fail={len(results) - sum(results)}')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
