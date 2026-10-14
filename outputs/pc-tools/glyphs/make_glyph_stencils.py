"""REL22: the DEVELOPER half of the shippable button icons ("stencils"). Runs on the developer PC (Pillow + NumPy + the Kenney pack).

Why: make_glyph_sets.py builds the glyph sets from Kenney "Input Prompts" (CC0) BUT it also reads the game's own button
textures (decoded from the disc): footprint() fits each icon into the bounding box of the original picture, and build_font()
keeps the original picture-font sheet (4x upscaled) for every glyph it does not replace. Those outputs therefore contain disc
pixels / disc-derived layout and may NOT be shipped. This script splits the work:

  here (shippable, no disc input):  the recoloured Kenney picture of every texture/font cell, exactly the 8-bit crop that
                                    make_glyph_sets.fit() would resize ("stencil"), plus a recipe (names, sizes, which stencil
                                    goes where, fit parameters). Kenney art + recolouring + file names only.
  glyph_local.py (player's PC):     decodes the game's own button textures from the PLAYER'S disc, computes the footprints and
                                    composes the sets there, bit-identical to make_glyph_sets.py (stdlib only).

Disc isolation is enforced, not just promised: an audit hook records every file this process opens and the run FAILS if any of
them lies under work/pc-game, work/hdtex or any other non-allowed folder (allowed: the Kenney pack, the two name tables
work/ctrl18/*.tsv - names, sizes and Dolphin names only, their 'png' column is never read -, Python itself, the output folder).

Usage (Codex Python with Pillow + NumPy):
  python -I outputs/pc-tools/glyphs/make_glyph_stencils.py            (re)write outputs/pc-tools/glyphs/stencils/
  python -I outputs/pc-tools/glyphs/make_glyph_stencils.py --check    rebuild in memory, compare with the committed folder
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
OUT_DEFAULT = os.path.join(HERE, 'stencils')
RECIPE_VERSION = 1

_opened = []


def _audit(event, args):
    if event == 'open' and args and isinstance(args[0], (str, bytes, os.PathLike)):
        p = args[0]
        if isinstance(p, bytes):
            p = p.decode('utf-8', 'replace')
        _opened.append(os.path.abspath(os.fspath(p)))


sys.addaudithook(_audit)
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402
import make_glyph_sets as M  # noqa: E402


def allowed(p):
    lp = os.path.normcase(os.path.abspath(p))
    ok_dirs = [M.KENNEY, os.path.dirname(sys.executable), sys.prefix, sys.base_prefix, HERE, os.path.join(HERE, '..', 'hdtex')]
    if any(lp.startswith(os.path.normcase(os.path.abspath(d)) + os.sep) or lp == os.path.normcase(os.path.abspath(d)) for d in ok_dirs):
        return True
    if lp in (os.path.normcase(M.GLYPH_TSV), os.path.normcase(M.TEX2D_TSV)):
        return True
    return False


def png_bytes(img):
    """8-bit RGBA PNG, filter 0 on every row (glyph_local.py decodes that without per-byte loops), zlib level 9."""
    from glyph_local import png_encode
    w, h = img.size
    return png_encode(w, h, img.convert('RGBA').tobytes())


def glow_lut():
    """make_glyph_sets.glow()/dilate(): alpha byte -> float32 /255 -> *255 -> uint8 (truncation), as NumPy does it."""
    v = np.arange(256, dtype=np.uint8)
    a = v.astype(np.float32) / 255.0
    a = (np.clip(a, 0, 1) * 255).astype(np.uint8)
    return [int(x) for x in a]


def sources(glyph):
    """Where each original lives on the disc: archive + internal path + image index (NAMES only; the 'png' column is not used),
    chosen exactly like make_glyph_sets.load_tables() (first matching row)."""
    out = {}
    for r in M.read_tsv(M.TEX2D_TSV):
        name = r['texname']
        if r['archive'] == 'Font/picture_00.brfnt':
            key = 'picture_00.' + name
        elif name.endswith('_0'):
            key = name[:-2]
        else:
            continue
        if key in out:
            continue
        if key not in glyph and not key.startswith('picture_00.'):
            continue
        w, h = (int(x) for x in r['size'].split('x'))
        fmt = {'I4': 0, 'I8': 1, 'IA4': 2, 'IA8': 3, 'RGB565': 4, 'RGB5A3': 5, 'RGBA8': 6, 'C4': 8, 'C8': 9, 'C14X2': 10,
               'CMPR': 14}[r['fmt'].split('/')[0]]
        e = {'archive': r['archive'], 'internal': r['internal'], 'w': w, 'h': h, 'fmt': fmt,
             'dolphin': r['dolphin_names'].split()[0][:-4]}
        if key.startswith('picture_00.'):
            e.update(kind='brfnt', sheet=int(name.split('_')[1]))
        else:
            e.update(kind='tpl', image=0)
        out[key] = e
    return out


def build(kenney):
    glyph = {r['texture']: r for r in M.read_tsv(M.GLYPH_TSV)}
    k = M.Kenney(kenney)
    stencils = {}  # file name -> png bytes

    def stencil(arr, sb):
        x0, y0, x1, y1 = sb
        data = png_bytes(M.to_pil(arr[y0:y1, x0:x1]))
        name = 's_' + hashlib.sha256(data).hexdigest()[:16] + '.png'
        stencils[name] = data
        return name

    sets = {}
    for set_name, (fam, variant) in M.SETS.items():
        R = M.roles(fam, variant)
        group_box = {}
        for grp in M.GROUPS:  # as make_glyph_sets.build_set
            specs = [R[M.TEXTURES[t][0]] for t in grp]
            if specs[0][0] in ('keep', 'blank'):
                continue
            pics = [M.picture(k, s) for s in specs]
            ub = M.union([M.abs_box(p) for p in pics])
            for t, p in zip(grp, pics):
                arr = M.on_box(p, ub)
                group_box[t] = (arr, (0, 0, arr.shape[1], arr.shape[0]))
        tex_out = []
        for tex, (role, mod) in M.TEXTURES.items():
            row = glyph[tex]
            w, h = (int(v) for v in row['size'].split('x'))
            e = {'tex': tex, 'name': row['dolphin_name'], 'w': w, 'h': h}
            spec = None if role == 'BLANK' else R[role]
            if spec is not None and spec[0] == 'keep':
                continue
            if role == 'BLANK' or spec[0] == 'blank':
                e['op'] = 'blank'
            else:
                if tex in group_box:
                    pic, sb = group_box[tex]
                    if mod == 'dark':
                        pic = pic.copy()
                        pic[..., :3] *= 0.78
                else:
                    pic = M.picture(k, spec, mod if mod != 'glow' else '')[0]
                    sb = M.bbox(pic[..., 3] / 255.0)
                e.update(op='fit', stencil=stencil(pic, sb), min_frac=0.62 if role in ('B', 'ONE', 'TWO') else 0.0,
                         square=not tex.startswith('th_remoCon') and not tex.startswith('th_nunchaku_'), glow=mod == 'glow')
            tex_out.append(e)
        # picture font (as make_glyph_sets.build_font)
        pics = {}
        for grp in M.FONT_GROUPS + [[i] for i in M.FONT_ROLES if not any(i in g for g in M.FONT_GROUPS)]:
            members = [i for i in grp if i in M.FONT_ROLES and R[M.FONT_ROLES[i]][0] not in ('keep', 'blank')]
            if not members:
                continue
            by_kind = {}
            for i in members:
                spec = R[M.FONT_ROLES[i]]
                by_kind.setdefault(spec[0] + (spec[1] if spec[0] in ('btn',) else ''), []).append(i)
            for ids in by_kind.values():
                ps = {i: M.picture(k, R[M.FONT_ROLES[i]]) for i in ids}
                ub = M.union([M.abs_box(p) for p in ps.values()])
                for i, p in ps.items():
                    arr = M.on_box(p, ub)
                    pics[i] = (arr, (0, 0, arr.shape[1], arr.shape[0]))
        font_out = []
        for s, hsh in enumerate(M.FONT_SHEETS):
            cells = []
            for c in range(9):
                i = s * 9 + c
                if i in pics:
                    cells.append({'c': c, 'i': i, 'gw': M.FONT_W[i], 'stencil': stencil(*pics[i])})
            if cells:
                font_out.append({'sheet': s, 'name': 'tex1_128x128_%s_2.png' % hsh, 'cells': cells})
        n = len(tex_out) + len(font_out)
        ini = ('[glyphs]\nset = %s\ndevice = %s\npreset = %s\ngenerator = outputs/pc-tools/glyphs/make_glyph_sets.py v%d\n'
               'art = Kenney "Input Prompts" 1.5, CC0 1.0 (www.kenney.nl), recoloured\ntextures = %d\n'
               % (set_name, {'xb': 'xbox', 'ps': 'playstation', 'sw': 'switch', 'kb': 'keyboard+mouse'}[fam],
                  'Switch HD / Toetsenbord + muis' if variant == 'hd' else 'Klassiek / Wii (beweging)', M.VERSION, n))
        sets[set_name] = {'textures': tex_out, 'font': font_out, 'ini': ini}
    src = sources(glyph)
    recipe = {
        'version': RECIPE_VERSION,
        'what': 'SkyBreak button icons: recoloured Kenney "Input Prompts" 1.5 pictures (CC0 1.0) + where they go. No game data.',
        'art': 'Kenney "Input Prompts" 1.5, CC0 1.0 (www.kenney.nl), recoloured by make_glyph_sets.py',
        'scale': M.SCALE,
        'font_cell': {'x0': 1, 'dx': 39, 'y0': 1, 'dy': 40, 'clear_w': 38, 'h': 39},
        'glow_lut': glow_lut(),
        'sources': src,
        'sets': sets,
        'stencils': {n: hashlib.sha256(d).hexdigest() for n, d in sorted(stencils.items())},
    }
    return recipe, stencils


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=OUT_DEFAULT)
    ap.add_argument('--kenney', default=M.KENNEY)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    recipe, stencils = build(a.kenney)
    rj = json.dumps(recipe, indent=1, sort_keys=True, ensure_ascii=True) + '\n'
    bad_opens = sorted({p for p in _opened if not allowed(p) and not os.path.normcase(p).startswith(os.path.normcase(os.path.abspath(a.out)))})
    disc_opens = [p for p in bad_opens if any(x in os.path.normcase(p) for x in (os.path.normcase(os.path.join(ROOT, 'work', 'pc-game')),
                                                                                  os.path.normcase(os.path.join(ROOT, 'work', 'hdtex'))))]
    print('AUDIT opened %d files; outside the allowed set: %d; under work/pc-game or work/hdtex: %d'
          % (len(set(_opened)), len(bad_opens), len(disc_opens)))
    for p in bad_opens[:10]:
        print('AUDIT not allowed:', p)
    if bad_opens:
        sys.exit('REFUSED: the stencil build opened files outside the Kenney pack / name tables')
    if a.check:
        bad = 0
        old = {}
        if os.path.isfile(os.path.join(a.out, 'recipe.json')):
            with open(os.path.join(a.out, 'recipe.json'), encoding='utf-8') as f:
                old_text = f.read()
            if old_text != rj:
                print('CHECK recipe.json differs')
                bad += 1
        else:
            print('CHECK recipe.json missing')
            bad += 1
        for n, d in stencils.items():
            p = os.path.join(a.out, n)
            if not os.path.isfile(p) or open(p, 'rb').read() != d:
                print('CHECK stencil differs/missing', n)
                bad += 1
        extra = [f for f in os.listdir(a.out) if f.endswith('.png') and f not in stencils] if os.path.isdir(a.out) else []
        if extra:
            print('CHECK extra files', extra[:5])
            bad += 1
        print('CHECK', 'OK' if not bad else '%d differences' % bad, '- %d stencils, %d sets' % (len(stencils), len(recipe['sets'])))
        sys.exit(1 if bad else 0)
    os.makedirs(a.out, exist_ok=True)
    for f in os.listdir(a.out):
        if f.startswith('s_') and f.endswith('.png') and f not in stencils:
            os.remove(os.path.join(a.out, f))
    for n, d in stencils.items():
        with open(os.path.join(a.out, n), 'wb') as f:
            f.write(d)
    with open(os.path.join(a.out, 'recipe.json'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(rj)
    lic = os.path.join(M.KENNEY, 'License.txt')
    with open(lic, encoding='utf-8', errors='replace') as f:
        text = f.read()
    if 'CC0' not in text and 'Creative Commons Zero' not in text:
        sys.exit('Kenney licence text does not say CC0: ' + lic)
    with open(os.path.join(a.out, 'KENNEY-LICENSE.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('The pictures s_*.png in this folder are recoloured crops of Kenney "Input Prompts" 1.5 (www.kenney.nl),\n'
                'made by outputs/pc-tools/glyphs/make_glyph_stencils.py. Original licence of the pack follows.\n\n' + text)
    total = sum(len(d) for d in stencils.values())
    print('wrote %d stencils (%d KB) + recipe.json (%d KB) to %s' % (len(stencils), total // 1024, len(rj) // 1024, a.out))


if __name__ == '__main__':
    main()
