"""PKG10 (roadmap M7.1/M7.2): helpers for Bouw-SkywardSword.ps1 (bring-your-own-disc build).

Subcommands (all paths may be relative to the current directory):
  verify-game GAME            check an extracted DATA partition: game id SOUE01, disc revision 0,
                              sys/main.dol and files/rels.arc SHA-1. Prints JSON; exit 3 = wrong disc.
  extract-rels GAME OUT --decomp-config config.yml
                              unpack files/rels.arc (+ the loose files/rels/*.rel) into OUT and check
                              every module against the decomp's SHA-1 list. Exit 3 = mismatch.
  make-recomp-config ...      write the generator config for a release build from the dev profile
                              (outputs/pc-profile/recomp_config.toml): every path key is replaced,
                              written relative to the new config file's folder.
  make-settings ...           write the player's settings file (instellingen.toml) from the runtime
                              sections of the dev profile ([graphics], [input], [hle], ...), without the
                              generator-only keys/sections, plus [nand] root and screenshot_dir.
  sync-tree SRC DST           make DST equal to SRC, copying only changed files (keeps Ninja builds
                              incremental) and deleting files that are no longer generated.

Reads only the user's own extracted game files; never writes into them.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from pathlib import Path

GAME_ID = 'SOUE01'
DISC_REVISION = 0
MAIN_DOL_SHA1 = '450a6806f46d59dcf8278db08e06f94865a4b18a'
RELS_ARC_SHA1 = 'a9d58dfea7071fad1c710bf3040529ac274e77f9'
REGIONS = {'E': 'Amerikaanse (NTSC-U)', 'P': 'Europese (PAL)', 'J': 'Japanse', 'K': 'Koreaanse'}

# Top-level keys of the dev profile that only the generator reads.
GENERATOR_KEYS = {'project_name', 'input_game_dir', 'output_dir', 'runtime_source_dir', 'sdk_sigdb',
                  'symbols_csv', 'symbol_prefix', 'split_output', 'instructions_per_file'}
GENERATOR_SECTIONS = {'hle_hooks', 'rels', 'rel_aot'}
TECHNICAL_SECTIONS = ('system', 'game', 'hle')


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fail(message, code=3):
    print(json.dumps({'ok': False, 'melding': message}, ensure_ascii=False))
    sys.exit(code)


# ----------------------------------------------------------------------------------------- verify
def cmd_verify_game(a):
    game = Path(a.game)
    boot = game / 'sys' / 'boot.bin'
    dol = game / 'sys' / 'main.dol'
    arc = game / 'files' / 'rels.arc'
    if not boot.is_file() or not dol.is_file():
        fail(f'Geen uitgepakte Wii-schijf in {game}: sys/boot.bin of sys/main.dol ontbreekt.')
    head = boot.read_bytes()[:8]
    gid = head[:6].decode('ascii', 'replace')
    rev = head[7]
    if gid != GAME_ID:
        if gid[:3] == 'SOU' and gid[3:4] in REGIONS:
            fail(f'Dit is de {REGIONS[gid[3]]} versie ({gid}). Alleen de Amerikaanse versie 1.00 '
                 f'(SOUE01, revisie 0) wordt ondersteund.')
        fail(f'Dit is geen Skyward Sword-schijf (spelcode {gid!r}). Nodig: SOUE01, revisie 0.')
    if rev != DISC_REVISION:
        fail(f'Dit is SOUE01 revisie {rev}. Alleen revisie 0 (versie 1.00) wordt ondersteund.')
    dol_sha1 = sha1_file(dol)
    if dol_sha1 != MAIN_DOL_SHA1:
        fail(f'sys/main.dol heeft SHA-1 {dol_sha1}, verwacht {MAIN_DOL_SHA1}. De schijf is beschadigd '
             f'of aangepast (mod/patch).')
    if not arc.is_file():
        fail('files/rels.arc ontbreekt: de uitgepakte schijf is onvolledig.')
    arc_sha1 = sha1_file(arc)
    if arc_sha1 != RELS_ARC_SHA1:
        fail(f'files/rels.arc heeft SHA-1 {arc_sha1}, verwacht {RELS_ARC_SHA1}. De schijf is beschadigd '
             f'of aangepast.')
    print(json.dumps({'ok': True, 'game_id': gid, 'revision': rev, 'main_dol_sha1': dol_sha1,
                      'rels_arc_sha1': arc_sha1}))


# ----------------------------------------------------------------------------------------- RELs
def cmd_extract_rels(a):
    tools = Path(a.relfile_dir) if a.relfile_dir else (
        Path(__file__).resolve().parents[2] / 'nwiirecomp' / 'nWiiRecomp' / 'tools')
    sys.path.insert(0, str(tools))
    from relfile import u8_extract, Rel  # noqa: E402  (NWiiRecomp's REL/U8 reader)
    game = Path(a.game)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    cfg = Path(a.decomp_config).read_text(encoding='utf-8')
    known = dict(re.findall(r'rels\.arc:rels/(\S+)\.rel\n.*\n.*\n\s+hash: ([0-9a-f]+)', cfg))
    files = {Path(p).stem: data for p, data in u8_extract((game / 'files' / 'rels.arc').read_bytes()).items()
             if p.endswith('.rel')}
    loose = game / 'files' / 'rels'
    if loose.is_dir():
        for p in sorted(loose.glob('*.rel')):
            files[p.stem] = p.read_bytes()
    ok = bad = unknown = 0
    bad_names = []
    for name, data in sorted(files.items()):
        digest = Rel(data, name).sha1()
        if name in known:
            if known[name] == digest:
                ok += 1
            else:
                bad += 1
                bad_names.append(name)
        else:
            unknown += 1
        target = out / (name + '.rel')
        if not target.is_file() or target.read_bytes() != data:
            target.write_bytes(data)
    for stale in out.glob('*.rel'):
        if stale.stem not in files:
            stale.unlink()
    result = {'ok': bad == 0 and ok > 0, 'modules': len(files), 'hash_ok': ok, 'hash_bad': bad,
              'hash_unknown': unknown, 'bad': bad_names[:10]}
    print(json.dumps(result))
    if not result['ok']:
        sys.exit(3)


# --------------------------------------------------------------------------------- TOML helpers
def toml_str(path_text):
    return '"' + path_text.replace('\\', '/').replace('"', '\\"') + '"'


def rel_to(target, base_dir):
    """target relative to base_dir with forward slashes; absolute when on another drive."""
    target = Path(target).resolve()
    try:
        return Path(os.path.relpath(target, Path(base_dir).resolve())).as_posix()
    except ValueError:
        return target.as_posix()


def split_sections(text):
    """[(header or None, [lines])]; header is the table name ('graphics', 'rel_aot' for [[rel_aot]])."""
    sections = [(None, [])]
    for line in text.splitlines():
        m = re.match(r'^\s*\[\[?\s*([A-Za-z0-9_.\-"]+)\s*\]\]?\s*(#.*)?$', line)
        if m:
            sections.append((m.group(1).strip('"'), [line]))
        else:
            sections[-1][1].append(line)
    return sections


def set_key(lines, key, value_toml, top=False):
    """Replace `key = ...` in a section's lines (first match) or append it."""
    pat = re.compile(r'^\s*' + re.escape(key) + r'\s*=')
    for i, line in enumerate(lines):
        if pat.match(line):
            lines[i] = f'{key} = {value_toml}'
            return lines
    # append after the last non-blank line of the section
    k = len(lines)
    while k > (0 if top else 1) and not lines[k - 1].strip():
        k -= 1
    lines.insert(k, f'{key} = {value_toml}')
    return lines


# ----------------------------------------------------------------------- generator config (M7.2)
def cmd_make_recomp_config(a):
    out = Path(a.out)
    base = out.resolve().parent
    text = Path(a.profile).read_text(encoding='utf-8')
    secs = split_sections(text)
    top = secs[0][1]
    top[:] = [l for l in top if not l.lstrip().startswith('#')]  # dev-profile path comments
    for key, value in (('input_game_dir', a.game), ('output_dir', a.export),
                       ('runtime_source_dir', a.runtime), ('sdk_sigdb', a.sigdb),
                       ('symbols_csv', a.symbols_csv)):
        set_key(top, key, toml_str(rel_to(value, base)), top=True)
    found_rels = False
    for name, lines in secs[1:]:
        if name == 'rels':
            found_rels = True
            for key, value in (('rel_dir', a.rel_dir), ('symbols_dir', a.rel_symbols_dir),
                               ('output_dir', Path(a.export) / 'rels'),
                               ('functions_h', Path(a.export) / 'functions.h')):
                set_key(lines, key, toml_str(rel_to(value, base)))
    if not found_rels:
        fail('Het profiel heeft geen [rels]-sectie.', 2)
    header = ['# PKG10: generated by Bouw-SkywardSword.ps1 from outputs/pc-profile/recomp_config.toml.',
              '# Paths are relative to this file (nwiirecomp resolves them against its folder).']
    body = []
    for name, lines in secs:
        body.extend(lines)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('\n'.join(header + body) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'ok': True, 'config': str(out)}))


# ------------------------------------------------------------------------- player settings (M7.3)
SETTINGS_HEADER = """\
# Skyward Sword PC - instellingen
# ================================
# Dit bestand mag je zelf aanpassen (bijvoorbeeld met Kladblok). Regels die met # beginnen zijn uitleg.
# Wijzigingen gelden vanaf de volgende start van Speel.cmd.
# - Relatieve paden gelden vanaf de map van dit bestand; %APPDATA% en andere %NAAM%-variabelen worden ingevuld.
# - De fabrieksinstellingen staan in instellingen.standaard.toml (die wordt bij elke bouw vernieuwd;
#   dit bestand niet). Kopieer dat bestand over dit bestand om alles terug te zetten.
"""

NAND_BLOCK = """\
[nand]
# root: map met je opgeslagen spel (de virtuele Wii-NAND). Bewaar deze map om je voortgang te houden.
root = {root}
# widescreen: true = 16:9, false = 4:3 (de Wii-instelling van de virtuele console).
# widescreen = true
"""


def cmd_make_settings(a):
    text = Path(a.profile).read_text(encoding='utf-8')
    secs = split_sections(text)
    user, technical = [], []
    have_nand = False
    for name, lines in secs[1:]:
        if name in GENERATOR_SECTIONS:
            continue
        lines = list(lines)
        while lines and not lines[-1].strip():
            lines.pop()
        if name == 'graphics':
            # drop the dev comment about the default screenshot folder, set the packaged one
            lines = [l for l in lines if not re.match(r'^\s*#\s*screenshot_dir', l)]
            lines = [l for l in lines if not re.match(r'^\s*screenshot_dir\s*=', l)]
            lines.append('# screenshot_dir: map voor F12-screenshots (volle resolutie, BMP).')
            lines.append(f'screenshot_dir = {toml_str(a.screenshot_dir)}')
        if name == 'input' and not any(re.match(r'^\s*#\s*mode', l) for l in lines):
            lines.insert(1, '# mode: 6 = toetsenbord/muis en gamepad tegelijk (aanbevolen). De toetsen staan in LEESMIJ.txt.')
        if name == 'nand':
            have_nand = True
            set_key(lines, 'root', toml_str(a.nand_root))
        (technical if name in TECHNICAL_SECTIONS else user).append('\n'.join(lines))
    if not have_nand:
        user.append(NAND_BLOCK.format(root=toml_str(a.nand_root)).rstrip())
    parts = [SETTINGS_HEADER.rstrip()] + user + [
        '# ----------------------------------------------------------------------------------------\n'
        '# Technisch: hieronder niets wijzigen (spel-ID en adressen die de runtime nodig heeft).\n'
        '# ----------------------------------------------------------------------------------------'] + technical
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('\n\n'.join(parts) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'ok': True, 'settings': str(out), 'sections': [n for n, _ in secs[1:]
                                                                      if n not in GENERATOR_SECTIONS]}))


# ------------------------------------------------------------------------------------- sync-tree
def cmd_sync_tree(a):
    src, dst = Path(a.src), Path(a.dst)
    copied = deleted = same = 0
    wanted = set()
    for s in src.rglob('*'):
        if not s.is_file():
            continue
        rel = s.relative_to(src)
        wanted.add(rel)
        d = dst / rel
        if d.is_file() and d.stat().st_size == s.stat().st_size and d.read_bytes() == s.read_bytes():
            same += 1
            continue
        d.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(s, d)  # fresh mtime -> Ninja rebuilds exactly the changed files
        copied += 1
    if dst.is_dir():
        for d in sorted(dst.rglob('*'), reverse=True):
            rel = d.relative_to(dst)
            if d.is_file() and rel not in wanted:
                d.unlink()
                deleted += 1
            elif d.is_dir() and not any(d.iterdir()):
                d.rmdir()
    print(json.dumps({'ok': True, 'copied': copied, 'unchanged': same, 'deleted': deleted}))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('verify-game'); s.add_argument('game'); s.set_defaults(fn=cmd_verify_game)
    s = sub.add_parser('extract-rels'); s.add_argument('game'); s.add_argument('out')
    s.add_argument('--decomp-config', required=True); s.add_argument('--relfile-dir')
    s.set_defaults(fn=cmd_extract_rels)
    s = sub.add_parser('make-recomp-config')
    for k in ('profile', 'out', 'game', 'export', 'runtime', 'sigdb', 'symbols-csv', 'rel-dir',
              'rel-symbols-dir'):
        s.add_argument('--' + k, required=True)
    s.set_defaults(fn=cmd_make_recomp_config)
    s = sub.add_parser('make-settings')
    for k in ('profile', 'out', 'nand-root', 'screenshot-dir'):
        s.add_argument('--' + k, required=True)
    s.set_defaults(fn=cmd_make_settings)
    s = sub.add_parser('sync-tree'); s.add_argument('src'); s.add_argument('dst'); s.set_defaults(fn=cmd_sync_tree)
    a = p.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
