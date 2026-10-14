"""INSTALL19 legal scan: refuses a SkyBreak package that contains anything derived from the game.

The distributable may contain only our own code and open-source components; the game is always built locally
from the player's own disc. This scan checks every file of a staged package (or an unpacked zip):

  E  deny extension (.dol .rel .arc .szs .brres .brfnt .brlyt .brlan .thp .tpl .bti .brsar .brstm .u8 .iso .rvz
     .wbfs .gcz .wia .ciso .nkit .wad .sav .pdb .obj .lib .ilk) and disc system files (boot.bin bi2.bin
     apploader.img fst.bin h3.bin cert.bin tmd.bin ticket.bin)
  M  deny magic: U8 (55 AA 38 2D), Yaz0, RARC, bres, THP\\0, RFNT/RFNA/RFNU, TPL (00 20 AF 30), RSAR/RSTM/RLYT/RLAN,
     RVZ/WIA/WBFS/CISO, a Wii/GameCube disc header (magic at 0x18/0x1C), a DOL header (text0 at 0x100, entry in 0x80xxxxxx)
  H  SHA-1 equal to main.dol, rels.arc or any of the 636 REL modules (config/SOUE01/build.sha1 + config.yml hashes)
  G  generated game code: output_N.cpp / main_output.cpp / rel_N.cpp / rel_aot_table.cpp names, or content with
     recompiled-instruction comments ("// 0x8xxxxxxx: XXXXXXXX") or loc_8xxxxxxx labels (>= 20 hits)
  T  any image under a textures/ directory (HD packs and glyph sets are made from the disc / not shipped in v1)
  X  an .exe other than the allow-listed SkyBreak-Setup.exe and app/SkyBreak.exe (only our own builds)
  S  larger than 8 MB unless allow-listed (totaldb.dsy, freeze_symbols.txt)
  B  large embedded byte tables in source (> 20000 "0x.." literals in one text file; not for vendored _deps libraries)
  REVINSTALL19 (10 Oct review: real disc/derived files of these kinds passed before):
  E  + .lz (370 LZ11 stage/object archives on the disc) .bnr .brtex .breff .breft .msbt .msbf .bmg, crash dumps .dmp/.mdmp
     (game memory) and .log files
  M  + LZ10/LZ11 header followed by a U8 archive, IMET banner (opening.bnr, magic at 0x40), MOC5 movie, REFF/REFT,
     MsgStdBn/MsgFlwBn, WIBN save banner, MDMP crash dump
  D  SHA-1 equal to any file (>= 32 bytes) of the player's extracted disc folder (--disc DIR; build_dist.py passes
     work/pc-game when it exists) - catches every exact copy (home.csv, RVZELDANP.str, ...) whatever its name
  I  any image (.png .bmp .jpg .gif .dds .tga .webp) outside vendored _deps/ that is not a SkyBreak branding file
     (a game screenshot is game-derived; branding: skybreak-logo-512.png, skybreak-logo-transparant.png)
     REL22: or a button-icon stencil: a PNG in .../outputs/pc-tools/glyphs/stencils/ passes only when that folder's
     recipe.json declares the Kenney "Input Prompts" CC0 art and lists the file with the same SHA-256 (the stencils are made
     from the Kenney pack alone by make_glyph_stencils.py, audit-hooked; the game-fitted glyph sets are made on the player's PC)
  A  a nested archive (.zip .7z .rar .cab .tar .gz .xz .bz2 .zst .jar, or PK/7z/Rar/MSCF magic) outside vendored _deps/:
     its contents would not be scanned
  G  the content check now runs on every file, not only on known source extensions

Writes LEGAL-SCAN.txt (counts per extension + every hit) and exits 0 only when there are 0 hits.

  python legal_scan.py <package-dir> [--hashes config/SOUE01] [--disc DIR] [--report FILE] [--allow-exe REL ...]
"""
import argparse
import hashlib
import re
import sys
from collections import Counter
from pathlib import Path

DENY_EXT = {'.dol', '.rel', '.arc', '.szs', '.brres', '.brfnt', '.brlyt', '.brlan', '.thp', '.tpl', '.bti', '.brsar',
            '.brstm', '.u8', '.iso', '.rvz', '.wbfs', '.gcz', '.wia', '.ciso', '.nkit', '.wad', '.sav', '.pdb', '.obj',
            '.lib', '.ilk',
            # REVINSTALL19
            '.lz', '.bnr', '.brtex', '.breff', '.breft', '.msbt', '.msbf', '.bmg', '.dmp', '.mdmp', '.log'}
DENY_NAMES = {'boot.bin', 'bi2.bin', 'apploader.img', 'fst.bin', 'h3.bin', 'cert.bin', 'tmd.bin', 'ticket.bin'}
MAGICS = [(b'\x55\xAA\x38\x2D', 'U8 archive'), (b'Yaz0', 'Yaz0'), (b'RARC', 'RARC'), (b'bres', 'BRRES'),
          (b'THP\x00', 'THP video'), (b'RFNT', 'BRFNT'), (b'RFNA', 'BRFNA'), (b'RFNU', 'BRFNU'),
          (b'\x00\x20\xAF\x30', 'TPL'), (b'RSAR', 'BRSAR'), (b'RSTM', 'BRSTM'), (b'RLYT', 'BRLYT'), (b'RLAN', 'BRLAN'),
          (b'RVZ\x01', 'RVZ image'), (b'WIA\x01', 'WIA image'), (b'WBFS', 'WBFS image'), (b'CISO', 'CISO image'),
          # REVINSTALL19
          (b'MOC5', 'Mobiclip movie'), (b'REFF', 'BREFF'), (b'REFT', 'BREFT'), (b'MsgStdBn', 'MSBT'), (b'MsgFlwBn', 'MSBF'),
          (b'WIBN', 'Wii save banner'), (b'MDMP', 'crash dump')]
U8_MAGIC = b'\x55\xAA\x38\x2D'
ARCHIVE_EXT = {'.zip', '.7z', '.rar', '.cab', '.tar', '.gz', '.tgz', '.xz', '.bz2', '.zst', '.jar'}  # REVINSTALL19
ARCHIVE_MAGIC = (b'PK\x03\x04', b'7z\xbc\xaf\x27\x1c', b'Rar!', b'MSCF')  # REVINSTALL19
BRAND_IMAGES = {'skybreak-logo-512.png', 'skybreak-logo-transparant.png'}  # REVINSTALL19
STENCIL_DIR = '/outputs/pc-tools/glyphs/stencils/'  # REL22
_recipes = {}


def stencil_ok(p, rel):
    """REL22: a Kenney stencil listed (name + SHA-256) in the recipe of its own folder, which declares the CC0 Kenney art."""
    if STENCIL_DIR not in '/' + rel or p.suffix.lower() != '.png':
        return False
    d = p.parent
    if d not in _recipes:
        try:
            import json
            r = json.loads((d / 'recipe.json').read_text(encoding='utf-8'))
            ok = 'Kenney' in r.get('art', '') and 'CC0' in r.get('art', '') and isinstance(r.get('stencils'), dict)
            _recipes[d] = r['stencils'] if ok else {}
        except (OSError, ValueError, KeyError):
            _recipes[d] = {}
    want = _recipes[d].get(p.name)
    return bool(want) and hashlib.sha256(p.read_bytes()).hexdigest() == want
GEN_NAME = re.compile(r'^(output_\d+\.cpp|main_output\.cpp|rel_\d+\.cpp|rel_aot_table\.cpp)$', re.I)
GEN_CONTENT = re.compile(rb'//\s*0x8[0-9A-Fa-f]{7}:\s*[0-9A-Fa-f]{8}|\bloc_8[0-9A-Fa-f]{7}\b')
HEX_LIT = re.compile(rb'0x[0-9A-Fa-f]{2}\b')
TEXT_EXT = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.inl', '.inc', '.txt', '.py', '.toml', '.md'}
IMAGE_EXT = {'.png', '.dds', '.tga', '.bmp', '.jpg', '.jpeg', '.webp', '.gif'}
BIG_OK = {'totaldb.dsy', 'freeze_symbols.txt'}
MAX_SIZE = 8 * 1024 * 1024
# fixed hashes of the disc files the project verifies (also in ui_disc.h / pkg10_release.py)
MAIN_DOL_SHA1 = '450a6806f46d59dcf8278db08e06f94865a4b18a'
RELS_ARC_SHA1 = 'a9d58dfea7071fad1c710bf3040529ac274e77f9'


def load_hashes(cfg_dir):
    hashes = {MAIN_DOL_SHA1: 'main.dol', RELS_ARC_SHA1: 'rels.arc'}
    if not cfg_dir:
        return hashes
    cfg = Path(cfg_dir)
    sha = cfg / 'build.sha1'
    if sha.is_file():
        for line in sha.read_text(encoding='utf-8', errors='replace').splitlines():
            parts = line.split()
            if len(parts) >= 2 and len(parts[0]) == 40:
                hashes[parts[0].lower()] = Path(parts[1]).name
    yml = cfg / 'config.yml'
    if yml.is_file():
        for m in re.finditer(r'hash:\s*([0-9a-fA-F]{40})', yml.read_text(encoding='utf-8', errors='replace')):
            hashes.setdefault(m.group(1).lower(), 'module in config.yml')
    return hashes


class DiscIndex:
    """REVINSTALL19: SHA-1 of every file of the player's extracted disc folder, hashed lazily per file size."""
    MIN_SIZE = 32

    def __init__(self, disc_dir):
        self.by_size, self.cache, self.files = {}, {}, 0
        if disc_dir and Path(disc_dir).is_dir():
            for p in Path(disc_dir).rglob('*'):
                if p.is_file():
                    s = p.stat().st_size
                    if s >= self.MIN_SIZE:
                        self.by_size.setdefault(s, []).append(p)
                        self.files += 1

    def match(self, size, sha1):
        for p in self.by_size.get(size, ()):
            if p not in self.cache:
                self.cache[p] = hashlib.sha1(p.read_bytes()).hexdigest()
            if self.cache[p] == sha1:
                return p.name
        return None


def dol_like(head):
    if len(head) < 0x100:
        return False
    be = lambda o: int.from_bytes(head[o:o + 4], 'big')
    text0_off, text0_addr, entry = be(0x00), be(0x48), be(0xE0)
    return text0_off == 0x100 and 0x80000000 <= text0_addr < 0x81800000 and 0x80000000 <= entry < 0x81800000


def scan(root, hashes, allow_exe, disc=None):
    root = Path(root)
    hits, ext_count, n = [], Counter(), 0
    for p in sorted(root.rglob('*')):
        if not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        if rel in ('LEGAL-SCAN.txt',):
            continue
        n += 1
        ext = p.suffix.lower()
        ext_count[ext or '(geen)'] += 1
        name = p.name.lower()
        size = p.stat().st_size
        parts_low = [x.lower() for x in p.relative_to(root).parts[:-1]]

        def hit(code, why):
            hits.append(f'{code} {rel}: {why}')

        if ext in DENY_EXT:
            hit('E', f'verboden extensie {ext}')
        if name in DENY_NAMES:
            hit('E', 'schijfsysteembestand')
        if ext == '.exe' and rel not in allow_exe:
            hit('X', 'exe die niet op de toegestane lijst staat')
        if ext in IMAGE_EXT and 'textures' in parts_low:
            hit('T', 'afbeelding onder een textures/-map')
        vendored = '/_deps/' in '/' + rel  # REVINSTALL19: upstream open-source trees (SDL2, toml++, ImGui)
        if ext in IMAGE_EXT and not vendored and name not in BRAND_IMAGES and not stencil_ok(p, rel):  # REL22: + stencils
            hit('I', 'afbeelding die geen SkyBreak-merkbestand of Kenney-sjabloon uit het recept is (schermafdruk van het spel?)')
        if size > MAX_SIZE and p.name not in BIG_OK:
            hit('S', f'groter dan 8 MB ({size} bytes)')
        if GEN_NAME.match(p.name):
            hit('G', 'naam van gegenereerde spelcode')
        with open(p, 'rb') as f:
            head = f.read(0x200)
        for magic, what in MAGICS:
            if head.startswith(magic):
                hit('M', f'magic {what}')
        if len(head) >= 0x20 and (head[0x18:0x1C] == b'\x5D\x1C\x9E\xA3' or head[0x1C:0x20] == b'\xC2\x33\x9F\x3D'):
            hit('M', 'Wii/GameCube-schijfkop')
        if dol_like(head):
            hit('M', 'DOL-kop')
        # REVINSTALL19: LZ10/LZ11-compressed U8 (the disc's *.arc.LZ), banner (IMET at 0x40), nested archives
        if head[:1] in (b'\x10', b'\x11') and U8_MAGIC in head[4:12]:
            hit('M', 'LZ-gecomprimeerd U8-archief')
        if head[0x40:0x44] == b'IMET':
            hit('M', 'IMET-banner')
        if not vendored and (ext in ARCHIVE_EXT or head.startswith(ARCHIVE_MAGIC)):
            hit('A', 'archief in het pakket (inhoud wordt niet gescand)')
        data = p.read_bytes()
        h = hashlib.sha1(data).hexdigest()
        if h in hashes:
            hit('H', f'SHA-1 gelijk aan {hashes[h]}')
        if disc is not None and size >= DiscIndex.MIN_SIZE:
            same = disc.match(size, h)
            if same:
                hit('D', f'SHA-1 gelijk aan schijfbestand {same}')
        if size < 64 * 1024 * 1024:  # REVINSTALL19: G content on every file (was: known source extensions only)
            g = len(GEN_CONTENT.findall(data))
            if g >= 20:
                hit('G', f'{g} gehercompileerde instructieregels/labels')
        if ext in TEXT_EXT and size < 64 * 1024 * 1024:
            # B: our own/NWiiRecomp code only; vendored upstream libraries (_deps: SDL2 ships compiled shader
            # bytecode tables) are third-party open source and are listed in DERDEN-LICENTIES.txt
            if ext not in ('.txt', '.md', '.py', '.toml') and '/_deps/' not in '/' + rel:
                lits = len(HEX_LIT.findall(data))
                if lits > 20000:
                    hit('B', f'{lits} byte-literals (ingebedde data?)')
    return n, ext_count, hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('package')
    ap.add_argument('--hashes', help='config/SOUE01 folder with build.sha1 + config.yml')
    ap.add_argument('--report', help='write the report here (default <package>/LEGAL-SCAN.txt)')
    ap.add_argument('--allow-exe', nargs='*', default=None)
    ap.add_argument('--disc', help='REVINSTALL19: extracted disc folder; refuse any exact copy of one of its files (rule D)')
    a = ap.parse_args()
    root = Path(a.package)
    if not root.is_dir():
        print(f'LEGAL FAIL: {root} is not a folder')
        return 2
    # the package has one top folder (SkyBreak-<ver>/) when scanning an unpacked zip
    base = root
    subs = [d for d in root.iterdir()]
    if len(subs) == 1 and subs[0].is_dir() and subs[0].name.lower().startswith('skybreak-'):
        base = subs[0]
    allow = set(a.allow_exe) if a.allow_exe is not None else {'SkyBreak-Setup.exe', 'app/SkyBreak.exe'}
    if base != root:
        allow |= {f'{base.name}/{x}' for x in allow}
    hashes = load_hashes(a.hashes)
    disc = DiscIndex(a.disc) if a.disc else None
    n, ext_count, hits = scan(root, hashes, allow, disc)
    lines = [f'INSTALL19 legal scan of {root}', f'files: {n}; known game hashes: {len(hashes)}; disc files indexed: '
             + (str(disc.files) if disc else 'none (no --disc)'),
             'per extension: ' + ', '.join(f'{k} {v}' for k, v in sorted(ext_count.items(), key=lambda kv: -kv[1]))]
    lines += [f'hits: {len(hits)}'] + hits
    lines.append('RESULT ' + ('PASS (0 game-derived files)' if not hits else f'FAIL ({len(hits)} hits)'))
    report = Path(a.report) if a.report else root / 'LEGAL-SCAN.txt'
    report.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines[:3] + lines[3:3 + 40] + lines[-1:]))
    return 0 if not hits else 1


if __name__ == '__main__':
    sys.exit(main())
