"""REL22-CERT console-data scan test (outputs/installer/tools/console_scan.py + its build_dist.py hook).

Synthetic inputs only: every "signed" certificate below is made of random bytes with a made-up name; no real console data.
  K1  clean mini package (sources, the runtime's placeholder certificate as raw bytes and as a 0x.. table, a Dolphin-style
      emulated Wii Remote address 11:02:19:79:00:00) -> 0 hits
  K2  everything the package allow-list ships from the source tree (package-allowlist.txt) -> 0 hits
  K3  an ECC device certificate with a non-zero signature as raw bytes (renamed .dat)       -> C
  K4  the same certificate as a C "0x.." byte table in a .cpp                                  -> C
  K5  an RSA-2048 signed header (TMD/ticket shape, issuer Root-CA00000001-CP...)                 -> C
  K6  keys.bin, and a BootMii "BackupMii" header renamed .dat                                    -> K
  K7  SYSCONF magic "SCv0" renamed .dat, setting.txt                                             -> Y
  K8  title/00010000/534f5545/data/wiiking2.sav and shared2/ files                               -> N
  K9  a Nintendo-OUI MAC (00:17:AB:..) in a text file -> W; the emulated 11:02:19:79:00:00 -> nothing
  K10 a zip with the K3 certificate inside (old-package mode)                                    -> C
  K11 the real es_device.cpp of the runtime                                                       -> 0 hits
  K12 build_dist.py (in-process, mini allow-list, fake Setup exes, own work folders) refuses a package with a keys.bin
      (exit 3, "console-unique"), nothing written to the share folder
  K13 build_dist.py builds the clean mini package: exit 0, zip present, summary check_console_hits == 0
--mutate: mutated copies (no signature check, no NAND-path rule, no Nintendo OUI list, build_dist without the hook)
          -> K3 K4 K5 K8 K9 K10 K12 must FAIL.
"""
import importlib.util
import io
import json
import os
import random
import shutil
import sys
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / 'outputs/installer/tools'
WORK = ROOT / 'work/rel22cert/tests/scan'
MUTATE = '--mutate' in sys.argv
results = []


def check(cid, cond, msg):
    results.append(bool(cond))
    print(f'{"PASS" if cond else "FAIL"}: {cid} {msg}')


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def mutate_copies(dst):
    dst.mkdir(parents=True, exist_ok=True)
    muts = {
        'console_scan.py': [('                if any(sig):', '                if False:  # MUTANT: no signature check'),
                            ("    if TITLE_DIR.search(rel) or '/shared2/' in '/' + rel.lower() or name in SAVE_NAMES:",
                             '    if False:  # MUTANT: no NAND-path rule'),
                            ('            if oui in NINTENDO_OUI:', '            if oui in set():  # MUTANT: no OUI list')],
        'build_dist.py': [('    _, console_hits = console_scan.scan(stage)  # REL22-CERT', '    console_hits = []  # MUTANT: no hook'),
                          ('        _, console_hits2 = console_scan.scan(chk)  # REL22-CERT', '        console_hits2 = []  # MUTANT')],
        'legal_scan.py': [],
    }
    for name, edits in muts.items():
        t = (TOOLS / name).read_text(encoding='utf-8')
        for old, new in edits:
            if t.count(old) != 1:
                sys.exit(f'mutation target not found in {name}: {old!r}')
            t = t.replace(old, new)
        (dst / name).write_text(t, encoding='utf-8')
    return dst


SRC = mutate_copies(WORK / 'mutsrc') if MUTATE else TOOLS
sys.path.insert(0, str(SRC))
cs = load('console_scan', SRC / 'console_scan.py')


def be32(v):
    return v.to_bytes(4, 'big')


def field(s, n):
    b = s.encode('ascii')
    return b + b'\0' * (n - len(b))


def ecc_cert(name, signed):
    rnd = random.Random(22)
    sig = bytes(rnd.randrange(1, 256) for _ in range(0x3C)) if signed else b'\0' * 0x3C
    key = bytes(rnd.randrange(256) for _ in range(0x3C)) if signed else b'\0' * 0x3C
    c = be32(0x00010002) + sig + b'\0' * 0x40 + field('Root-CA00000001-MS00000002', 0x40) + be32(2) + field(name, 0x40)
    c += (be32(0x12345678) if signed else be32(0)) + key + b'\0' * 0x3C
    assert len(c) == 0x180
    return c


def rsa2048_header():
    rnd = random.Random(23)
    return be32(0x00010001) + bytes(rnd.randrange(1, 256) for _ in range(0x100)) + b'\0' * 0x3C + \
        field('Root-CA00000001-CP00000004', 0x40) + b'\0' * 0x100


def c_table(data):
    lines = ['static const unsigned char kBlob[] = {']
    for i in range(0, len(data), 12):
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i + 12]) + ',')
    return '\n'.join(lines + ['};', '']).encode()


def codes(hits):
    return {h.split()[0] for h in hits}


def pkg(name, files):
    d = WORK / name
    if d.exists():
        shutil.rmtree(d)
    for rel, data in files.items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return d


FAKE = ecc_cert('NG7e57da7a', True)  # made-up name, random signature: shaped like a real device certificate
PLACEHOLDER = ecc_cert('NG00000000', False)
CLEAN = {'bron/src/main.cpp': b'int main() { return 0; }\n', 'bron/src/placeholder.bin': PLACEHOLDER,
         'bron/src/placeholder.cpp': c_table(PLACEHOLDER),
         'bron/docs/remote.txt': b'emulated Wii Remote bdaddr 11:02:19:79:00:00 (Dolphin default)\n'}


def main():
    if WORK.exists():
        for p in WORK.iterdir():
            if p.name != 'mutsrc':
                shutil.rmtree(p) if p.is_dir() else p.unlink()
    WORK.mkdir(parents=True, exist_ok=True)

    n, hits = cs.scan(pkg('clean', CLEAN))
    check('K1', n == 4 and not hits, f'clean mini package (placeholder cert raw + table, emulated bdaddr): {len(hits)} hits {hits[:3]}')

    # K2: what the package allow-list ships from this tree
    import fnmatch
    shipped, hits = 0, []
    lines = [ln.split(None, 1) for ln in (ROOT / 'outputs/installer/package-allowlist.txt').read_text(encoding='utf-8').splitlines()]
    excludes = [p[1].strip() for p in lines if len(p) == 2 and p[0] == 'exclude']
    for kind, rel in ((p[0], p[1].strip()) for p in lines if len(p) == 2 and p[0] in ('dir', 'file')):
        base = ROOT / rel
        for f in ([base] if base.is_file() else sorted(x for x in base.rglob('*') if x.is_file())):
            r = f.relative_to(ROOT).as_posix()
            if any(fnmatch.fnmatch(r, e) for e in excludes):
                continue
            shipped += 1
            cs.scan_bytes(r, f.read_bytes(), hits)
    check('K2', shipped > 1000 and not hits, f'allow-listed source tree: {shipped} files, {len(hits)} hits {hits[:3]}')

    _, h = cs.scan(pkg('k3', {'bron/data/blob.dat': b'\x01' * 0x33 + FAKE + b'\x02' * 9}))
    check('K3', 'C' in codes(h), f'signed ECC device certificate (raw, unaligned) -> {sorted(codes(h))}')
    _, h = cs.scan(pkg('k4', {'bron/src/es.cpp': b'// ids 0x16 0x1B\n' + c_table(FAKE)}))
    check('K4', 'C' in codes(h), f'signed certificate as 0x.. table in a .cpp -> {sorted(codes(h))}')
    _, h = cs.scan(pkg('k5', {'bron/data/title.dat': rsa2048_header()}))
    check('K5', 'C' in codes(h), f'RSA-2048 signed header (TMD/ticket shape) -> {sorted(codes(h))}')
    _, h = cs.scan(pkg('k6', {'bron/keys.bin': b'\0' * 0x400, 'bron/x.dat': b'BackupMii v1, ConsoleID: 00000000\n' + b'\0' * 64}))
    check('K6', codes(h) == {'K'} and len(h) == 2, f'keys.bin + BackupMii header -> {h}')
    _, h = cs.scan(pkg('k7', {'bron/sc.dat': b'SCv0' + b'\0' * 60, 'bron/setting.txt': b'\0' * 0x100}))
    check('K7', codes(h) == {'Y'} and len(h) == 2, f'SYSCONF magic + setting.txt -> {h}')
    _, h = cs.scan(pkg('k8', {'bron/nand/title/00010000/534f5545/data/wiiking2.sav': b'\0' * 64,
                              'bron/nand/shared2/menu/x.dat': b'\0' * 16}))
    check('K8', sum(1 for x in h if x.startswith('N ')) == 2, f'NAND save + shared2 file -> {h}')
    # Synthetic negative fixture, constructed at test time; no real console address.
    fake_mac = ':'.join(f'{b:02X}' for b in (0,23,171,18,52,86))
    _, h = cs.scan(pkg('k9', {'bron/a.txt': ('remote '+fake_mac+' paired\n').encode(), 'bron/b.md': b'Dolphin 11:02:19:79:00:00\n'}))
    check('K9', len(h) == 1 and h[0].startswith('W bron/a.txt'), f'Nintendo-OUI MAC -> W, emulated bdaddr -> nothing: {h}')
    zp = WORK / 'old.zip'
    with zipfile.ZipFile(zp, 'w') as z:
        z.writestr('SkyBreak-0.0.0/bron/es.dat', FAKE)
        z.writestr('SkyBreak-0.0.0/bron/main.cpp', 'int main() {}\n')
    n, h = cs.scan(zp)
    check('K10', n == 2 and 'C' in codes(h), f'zip with a signed certificate -> {n} members, {sorted(codes(h))}')
    es = ROOT / 'outputs/nwiirecomp/nWiiRuntime/src/hle/devices/es_device.cpp'
    h = []
    cs.scan_bytes('es_device.cpp', es.read_bytes(), h)
    check('K11', not h, f'runtime es_device.cpp (placeholder certificate): {h}')

    # K12/K13: build_dist.py in-process with a mini allow-list copy and fake Setup exes in our own work folders
    sys.modules.pop('legal_scan', None)
    bd = load('build_dist', SRC / 'build_dist.py')
    bd.ROOT, bd.INSTALLER = ROOT, ROOT / 'outputs/installer'
    fake_build = WORK / 'fake-build'
    fake_build.mkdir(parents=True, exist_ok=True)
    for exe in ('SkyBreak-Setup.exe', 'SkyBreak.exe'):
        (fake_build / exe).write_bytes(b'MZ fake test exe\n')

    def run_dist(tag, tree):
        src = pkg(f'tree-{tag}', tree)

        def copy_mini(stage_bron, dirs, files, excludes, runtime_source=None):
            shutil.copytree(src, stage_bron, dirs_exist_ok=True)
            return sum(1 for p in src.rglob('*') if p.is_file())
        bd.copy_allowlisted = copy_mini
        out = WORK / f'dist-{tag}'
        argv = ['build_dist.py', '--build', str(fake_build), '--stage-root', str(out / 'stage'), '--out', str(out / 'share'),
                '--check-dir', str(out / 'check'), '--disc', str(WORK / 'no-disc')]
        buf, old = io.StringIO(), sys.argv
        sys.argv = argv
        try:
            with redirect_stdout(buf):
                rc = bd.main()
        finally:
            sys.argv = old
        return rc, buf.getvalue(), out

    rc, txt, out = run_dist('bad', {'src/main.cpp': b'int main() { return 0; }\n', 'keys.bin': b'\0' * 0x400})
    share = list((out / 'share').glob('*.zip')) if (out / 'share').exists() else []
    check('K12', rc == 3 and 'console-unique' in txt and not share, f'build_dist with keys.bin: exit {rc}, share zips {len(share)}')
    rc, txt, out = run_dist('good', {k.replace('bron/', ''): v for k, v in CLEAN.items()})
    zips = list((out / 'share').glob('SkyBreak-*.zip'))
    summ = list((out / 'stage').glob('SkyBreak-*.summary.json'))
    s = json.loads(summ[0].read_text(encoding='utf-8')) if summ else {}
    check('K13', rc == 0 and len(zips) == 1 and s.get('check_console_hits') == 0 and s.get('check_legal_hits') == 0,
          f'clean mini package: exit {rc}, zips {len(zips)}, check_console_hits {s.get("check_console_hits")}')

    print(f'RESULT pass={sum(results)} fail={len(results) - sum(results)}')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
