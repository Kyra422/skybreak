"""CTRL18-UI (T7): generate the controller glyph sets (button pictures for the HUD, menus and the picture font).

Source art: Kenney "Input Prompts" 1.5 (CC0, https://kenney.nl/assets/input-prompts), unpacked in
work/tools/kenney-input-prompts/extracted. The shapes come from Kenney's icon FONTS (crisp at any size), the colours,
the dark symbol fill, the highlighted arm/key and the thin dark rim are added here.

Output: outputs/pc-prototype/textures/glyphs/SOUE01/<set>/tex1_<w>x<h>_<hash>_<fmt>.png (+ the "_m" twin as a hard
link), Dolphin names of the ORIGINAL textures (work/ctrl18/glyph-textures.tsv), 4x the original size, RGBA, straight
alpha, fitted into the footprint of the original picture. The runtime loader (hle/gx/hd_textures.cpp, [graphics]
glyph_set) searches the active set folder before every other pack folder.

Sets (folder = <device>-<preset family>):
  xbox-hd, ps-hd, switch-hd, kbm          preset "Switch HD (knoppen)" / "Toetsenbord + muis": the PHYSICAL button that
                                          performs the action, face buttons BY POSITION like a Switch (Switch A = east:
                                          Xbox B / PS Circle). Wii B (items) = ZR / RT / R2 / R, and so on.
  xbox-classic, ps-classic, switch-classic, kbm-classic
                                          presets "Klassiek" and "Wii (beweging/gyro)": the old fixed mapping
                                          (input_bindings.h defaults; SDL button "a" = south on Xbox/PS).
  (wii = no folder: the original Wii pictures)

Usage (Codex Python with Pillow + NumPy):
  python -I outputs/pc-tools/glyphs/make_glyph_sets.py [--sets a,b] [--out DIR] [--preview DIR] [--check]
--check verifies an existing output (names, sizes, alpha present) without writing (used by the tests).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
from collections import deque

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
KENNEY = os.path.join(ROOT, 'work', 'tools', 'kenney-input-prompts', 'extracted')
GLYPH_TSV = os.path.join(ROOT, 'work', 'ctrl18', 'glyph-textures.tsv')
TEX2D_TSV = os.path.join(ROOT, 'work', 'ctrl18', '2d-textures.tsv')
OUT_DEFAULT = os.path.join(ROOT, 'outputs', 'pc-prototype', 'textures', 'glyphs', 'SOUE01')
VERSION = 1
SCALE = 4

FAMILIES = {
    'xb': ('Xbox Series', 'kenney_input_xbox_series'),
    'ps': ('PlayStation Series', 'kenney_input_playstation_series'),
    'sw': ('Nintendo Switch', 'kenney_input_nintendo_switch'),
    'kb': ('Keyboard & Mouse', 'kenney_input_keyboard_&_mouse'),
}
WHITE = (255, 255, 255)
DARK = (40, 36, 32)       # symbol fill (letters cut out of the Kenney shapes)
RIM = (24, 21, 18)        # thin dark rim for contrast on bright scenes
HILITE = (231, 50, 70)    # Kenney's highlight red (active D-pad arm / key / mouse button)
XBOX = {'a': (125, 183, 0), 'b': (239, 78, 41), 'x': (0, 159, 235), 'y': (254, 181, 4)}
PSC = {'cross': (124, 102, 232), 'circle': (255, 102, 102), 'square': (255, 105, 248), 'triangle': (64, 226, 160)}

# ------------------------------------------------------------------------------------------------ role -> picture
# Picture specs:  ('btn', fam, kenney_name, style)   style: 'w' white shape + dark symbol, 'xb:<k>' coloured Xbox disc
#                                                    + white letter, 'ps:<k>' white disc + coloured PlayStation symbol
#                 ('dpad', fam, hi)  ('arrows', hi)  ('wasd', hi)  ('mouse', hi)  ('stick', fam, side, dir)
#                 ('blank',)  ('keep',)
PAD_PREFIX = {'xb': 'xbox', 'ps': 'playstation', 'sw': 'switch'}


def face(fam, pos):
    """Face button at a physical position (south/east/west/north)."""
    if fam == 'xb':
        k = {'south': 'a', 'east': 'b', 'west': 'x', 'north': 'y'}[pos]
        return ('btn', 'xb', 'xbox_button_' + k, 'xb:' + k)
    if fam == 'ps':
        k = {'south': 'cross', 'east': 'circle', 'west': 'square', 'north': 'triangle'}[pos]
        return ('btn', 'ps', 'playstation_button_' + k, 'ps:' + k)
    k = {'south': 'b', 'east': 'a', 'west': 'y', 'north': 'x'}[pos]  # Nintendo: labels at their positions
    return ('btn', 'sw', 'switch_button_' + k, 'w')


def shoulder(fam, which):  # which: lb rb lt rt
    if fam == 'xb':
        return ('btn', 'xb', 'xbox_' + which, 'w')
    if fam == 'ps':
        return ('btn', 'ps', 'playstation_trigger_' + {'lb': 'l1', 'rb': 'r1', 'lt': 'l2', 'rt': 'r2'}[which], 'w')
    return ('btn', 'sw', 'switch_button_' + {'lb': 'l', 'rb': 'r', 'lt': 'zl', 'rt': 'zr'}[which], 'w')


def menu_btn(fam, which):  # which: back (View/Create/-) or start (Menu/Options/+)
    if fam == 'xb':
        return ('btn', 'xb', 'xbox_button_' + ('view' if which == 'back' else 'menu'), 'w')
    if fam == 'ps':
        return ('btn', 'ps', 'playstation5_button_' + ('create' if which == 'back' else 'options'), 'w')
    return ('btn', 'sw', 'switch_button_' + ('minus' if which == 'back' else 'plus'), 'w')


def key(k):
    return ('btn', 'kb', 'keyboard_' + k, 'w')


def roles(fam, variant):
    """Wii role -> picture for one set. Roles: A B ONE TWO PLUS MINUS C Z STICK(dir) STICK_SIDE DPAD(hi) for HUD pieces,
    and the font-only roles DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT DPAD_ALL DPAD_NONE POINTER, plus MOTION (remote
    pictures) and NUN (Nunchuk pictures)."""
    r = {}
    if fam == 'kb':
        if variant == 'hd':  # design section 4 + decision D4 (Esc = gear)
            r.update(A=key('e'), B=key('r'), MINUS=key('tab'), PLUS=key('m'), ONE=key('escape'), TWO=key('h'),
                     C=key('c'), Z=key('q'), HUD_DPAD=key('t'), DPAD_DOWN=key('t'), DPAD_UP=key('v'),
                     DPAD_LEFT=('arrows', 'left'), DPAD_RIGHT=('arrows', 'right'), DPAD_ALL=('arrows', 'all'),
                     DPAD_NONE=('arrows', None), POINTER=('keep',), MOTION=('mouse', 'left'), NUN=('mouse', 'right'))
        else:  # input_bindings.h keyboard defaults
            r.update(A=('mouse', 'left'), B=('mouse', 'right'), MINUS=key('backspace'), PLUS=key('return'),
                     ONE=key('1'), TWO=key('2'), C=key('c'), Z=key('z'), HUD_DPAD=('arrows', 'down'),
                     DPAD_DOWN=('arrows', 'down'), DPAD_UP=('arrows', 'up'), DPAD_LEFT=('arrows', 'left'),
                     DPAD_RIGHT=('arrows', 'right'), DPAD_ALL=('arrows', 'all'), DPAD_NONE=('arrows', None),
                     POINTER=('keep',), MOTION=('keep',), NUN=('keep',))
        for d in (None, 'up', 'down', 'left', 'right'):
            r['STICK_' + (d or 'n')] = ('wasd', d)
        r['STICK_SIDE'] = ('wasd', None)
        return r
    if variant == 'hd':  # design section 3.3, face buttons by position (user decision)
        r.update(A=face(fam, 'east'), B=shoulder(fam, 'rt'), MINUS=shoulder(fam, 'rb'), PLUS=menu_btn(fam, 'back'),
                 ONE=menu_btn(fam, 'start'), TWO=('dpad', fam, 'right'), C=('dpad', fam, 'up'),
                 Z=shoulder(fam, 'lt'), DPAD_UP=face(fam, 'north'),
                 POINTER=('stick', fam, 'r', None), MOTION=('stick', fam, 'r', None),
                 NUN=('btn', fam, {'xb': 'xbox_ls', 'ps': 'playstation_button_l3', 'sw': 'switch_stick_l_press'}[fam], 'w'))
    else:  # input_bindings.h gamepad defaults (SDL a/b/x/y)
        r.update(A=face(fam, 'south') if fam != 'sw' else face(fam, 'east'),
                 B=face(fam, 'east') if fam != 'sw' else face(fam, 'south'),
                 ONE=face(fam, 'west') if fam != 'sw' else face(fam, 'north'),
                 TWO=face(fam, 'north') if fam != 'sw' else face(fam, 'west'),
                 PLUS=menu_btn(fam, 'start'), MINUS=menu_btn(fam, 'back'), C=shoulder(fam, 'lb'),
                 Z=shoulder(fam, 'lt'), DPAD_UP=('dpad', fam, 'up'), POINTER=('stick', fam, 'r', None),
                 MOTION=('keep',), NUN=('keep',))
    r.update(HUD_DPAD=('dpad', fam, 'down'), DPAD_DOWN=('dpad', fam, 'down'), DPAD_LEFT=('dpad', fam, 'left'),
             DPAD_RIGHT=('dpad', fam, 'right'), DPAD_ALL=('dpad', fam, 'all'), DPAD_NONE=('dpad', fam, None))
    for d in (None, 'up', 'down', 'left', 'right'):
        r['STICK_' + (d or 'n')] = ('stick', fam, 'l', d)
    r['STICK_SIDE'] = r['STICK_n']  # the HUD 'move' stick: top view (Kenney's side views read as 'press L3')
    return r


# texture -> (role, modifier). modifier: '' | 'dark' (pressed/dark variants) | 'glow' (I4/I8 glow silhouette)
# Group = textures that must share one transform (animation frames that cross-fade).
TEXTURES = {
    'tr_aBtn_00': ('A', ''), 'tr_aBtn_01': ('A', 'dark'), 'tr_bBtn_01': ('B', ''), 'tr_bBtn_03': ('B', 'dark'),
    'tr_cBtn_00': ('C', ''), 'tr_cBtn_01': ('C', 'dark'), 'tr_zBtn_00': ('Z', ''), 'th_zBtn_01': ('Z', 'dark'),
    'tr_1Btn_00': ('ONE', ''), 'tr_2Btn_00': ('TWO', ''), 'tr_minusBtn_00': ('MINUS', ''),
    'tr_plusBtn_00': ('PLUS', ''), 'tr_nunchakuBtn_00': ('STICK_SIDE', ''),
    'th_nunchakuBtn_00': ('STICK_n', ''), 'th_nunchakuBtn_01': ('STICK_up', ''),
    'th_nunchakuBtn_02': ('STICK_right', ''), 'th_nunchakuBtn_03': ('STICK_left', ''),
    'th_nunchakuBtn_04': ('STICK_down', ''),
    # D-pad: the base shows the lit DOWN arm (Fi: every HUD/popup use is "down"); the lit-arm overlays go blank.
    'tr_crossBtnBasic_00': ('HUD_DPAD', ''), 'tr_crossBtnBasicT_00': ('BLANK', ''),
    'tr_crossBtnBasicL_00': ('BLANK', ''), 'tr_crossBtnBasicD_00': ('BLANK', ''),
    # HUD silhouettes of the Wii Remote / Nunchuk: transparent (no Wii controller is used)
    'tr_remoConBg_01': ('BLANK', ''), 'tr_nunBg_00': ('BLANK', ''), 'tr_nunBgArrow_00': ('BLANK', ''),
    # motion pictograms: remote -> right stick / mouse, Nunchuk -> L3 / right mouse button (HD sets only)
    'th_remoCon_02': ('MOTION', ''), 'th_remoCon_03': ('MOTION', ''), 'th_remoCon_04': ('MOTION', ''),
    'th_remoCon_05': ('MOTION', ''), 'th_remoCon_06': ('MOTION', ''), 'th_remoCon_07': ('MOTION', ''),
    'th_remoCon_08': ('MOTION', ''), 'th_remoCon_09': ('MOTION', ''), 'th_remoCon_10': ('MOTION', ''),
    'th_remoCon_11': ('MOTION', ''), 'th_remoCon_13': ('MOTION', ''), 'th_remoCon_15': ('MOTION', ''),
    'th_remoCon_16': ('MOTION', ''), 'th_remoCon_17': ('MOTION', ''), 'th_remoCon_18': ('MOTION', ''),
    'th_remoCon_19': ('MOTION', ''), 'th_remoCon_20': ('MOTION', ''), 'th_remoConS_00': ('MOTION', 'glow'),
    'th_nunchaku_00': ('NUN', ''), 'th_nunchaku_01': ('NUN', ''), 'th_nunchaku_02': ('NUN', ''),
    'th_nunchaku_03': ('NUN', ''), 'th_nunchakuS_00': ('NUN', 'glow'),
}
GROUPS = [['th_nunchakuBtn_00', 'th_nunchakuBtn_01', 'th_nunchakuBtn_02', 'th_nunchakuBtn_03', 'th_nunchakuBtn_04']]

# picture_00.brfnt: char -> (glyph index, role). Cells not listed keep the original (arrows, X mark, hole).
FONT_W = [36, 28, 27, 27, 29, 29, 34, 36, 35, 35, 35, 36, 35, 36, 36, 36, 36, 36, 36, 32, 32, 36, 36, 26, 26, 38]
FONT_SHEETS = ['e9829023c61afe11', '6e7cc5c89669d856', '7616852e3e5e9a30']
FONT_ROLES = {0: 'A', 1: 'B', 2: 'MINUS', 3: 'PLUS', 4: 'ONE', 5: 'TWO', 6: 'C', 7: 'Z', 8: 'STICK_n',
              9: 'STICK_up', 10: 'STICK_down', 11: 'STICK_left', 12: 'STICK_right', 13: 'DPAD_ALL',
              14: 'DPAD_NONE', 15: 'DPAD_UP', 16: 'DPAD_DOWN', 17: 'DPAD_LEFT', 18: 'DPAD_RIGHT', 23: 'POINTER'}
FONT_GROUPS = [[8, 9, 10, 11, 12], [13, 14, 15, 16, 17, 18]]

SETS = {
    'xbox-hd': ('xb', 'hd'), 'ps-hd': ('ps', 'hd'), 'switch-hd': ('sw', 'hd'), 'kbm': ('kb', 'hd'),
    'xbox-classic': ('xb', 'classic'), 'ps-classic': ('ps', 'classic'), 'switch-classic': ('sw', 'classic'),
    'kbm-classic': ('kb', 'classic'),
}


# ------------------------------------------------------------------------------------------------ data
def read_tsv(path):
    rows = [l.rstrip('\n').split('\t') for l in open(path, encoding='utf-8')]
    head = rows[0]
    return [dict(zip(head, r)) for r in rows[1:] if r and r[0]]


def load_tables():
    glyph = {r['texture']: r for r in read_tsv(GLYPH_TSV)}
    decoded = {}
    for r in read_tsv(TEX2D_TSV):
        name = r['texname']
        if r['archive'] == 'Font/picture_00.brfnt':
            decoded['picture_00.' + name] = r['png']
        elif name.endswith('_0') and name[:-2] not in decoded:
            decoded[name[:-2]] = r['png']
    return glyph, decoded


def load_original(decoded, tex):
    p = decoded[tex].replace('\\', '/')
    return np.array(Image.open(os.path.join(ROOT, p)).convert('RGBA'))


# ------------------------------------------------------------------------------------------------ Kenney fonts
class Kenney:
    EM = 640  # font size used to render shapes (glyphs are ~ EM/2 px tall)

    def __init__(self, base):
        self.base = base
        self.maps, self.fonts = {}, {}
        for fam, (folder, stem) in FAMILIES.items():
            m = {}
            for line in open(os.path.join(base, folder, 'Fonts', stem + '_map.txt'), encoding='utf-8'):
                if ':' in line:
                    k, v = line.split(':', 1)
                    m[k.strip()] = chr(int(v.strip()[2:], 16))
            self.maps[fam] = m
            self.fonts[fam] = ImageFont.truetype(os.path.join(base, folder, 'Fonts', stem + '.ttf'), self.EM)
        self._cache = {}

    def alpha(self, fam, name):
        """(coverage float 0..1 cropped to the glyph + padding, (x, y) of its top-left on the font's shared em canvas):
        every glyph of one font shares that coordinate system, so animation frames can share one transform."""
        k = (fam, name)
        if k not in self._cache:
            if name not in self.maps[fam]:
                raise KeyError('Kenney %s has no glyph %s' % (fam, name))
            c = Image.new('L', (int(self.EM * 1.5), int(self.EM * 1.5)), 0)
            ImageDraw.Draw(c).text((self.EM // 4, 0), self.maps[fam][name], font=self.fonts[fam], fill=255)
            a = np.asarray(c, dtype=np.float32) / 255.0
            b = bbox(a)
            pad = max(8, int(0.08 * max(b[2] - b[0], b[3] - b[1])))
            x0, y0 = max(0, b[0] - pad), max(0, b[1] - pad)
            x1, y1 = min(a.shape[1], b[2] + pad), min(a.shape[0], b[3] + pad)
            self._cache[k] = (a[y0:y1, x0:x1].copy(), (x0, y0))
        return self._cache[k]


def bbox(a, thr=0.02):
    ys, xs = np.nonzero(a > thr)
    if len(xs) == 0:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1


def union(boxes):
    boxes = [b for b in boxes if b]
    return (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes))


def holes_of(cov):
    """Enclosed transparent areas (letters/symbols cut out of a shape)."""
    solid = cov > 0.5
    h, w = solid.shape
    img = Image.fromarray(np.pad(~solid, 1, constant_values=True).astype(np.uint8) * 255, "L").copy()
    ImageDraw.floodfill(img, (0, 0), 128)
    outside = np.asarray(img)[1:-1, 1:-1] == 128
    return (~solid) & (~outside)


def dilate(mask_f, px):
    if px <= 0:
        return mask_f
    size = int(px) * 2 + 1
    im = Image.fromarray((np.clip(mask_f, 0, 1) * 255).astype(np.uint8), 'L').filter(ImageFilter.MaxFilter(size))
    return np.asarray(im, dtype=np.float32) / 255.0


def region_mask(shape, box, hi, kind):
    """Normalised regions of the highlighted part (D-pad arm, arrow key, mouse button)."""
    h, w = shape
    x0, y0, x1, y1 = box
    yy, xx = np.mgrid[0:h, 0:w]
    u = (xx - x0) / max(1, x1 - x0)
    v = (yy - y0) / max(1, y1 - y0)
    if hi is None:
        return np.zeros(shape, bool)
    if kind == 'dpad':
        parts = {'up': (v < 0.40) & (u > 0.28) & (u < 0.72), 'down': (v > 0.60) & (u > 0.28) & (u < 0.72),
                 'left': (u < 0.40) & (v > 0.28) & (v < 0.72), 'right': (u > 0.60) & (v > 0.28) & (v < 0.72)}
    elif kind == 'arrows':  # inverted T: up key on top, left/down/right below
        parts = {'up': (v < 0.48) & (u > 0.33) & (u < 0.67), 'left': (v > 0.52) & (u < 0.33),
                 'down': (v > 0.52) & (u > 0.33) & (u < 0.67), 'right': (v > 0.52) & (u > 0.67)}
    else:  # mouse buttons
        parts = {'left': (v < 0.40) & (u < 0.5), 'right': (v < 0.40) & (u >= 0.5)}
    if hi == 'all':
        m = np.zeros(shape, bool)
        for p in parts.values():
            m |= p
        return m
    return parts[hi]


def colourise(cov, shape_rgb, hole_rgb, hi_mask=None, hi_rgb=HILITE, hi_hole_rgb=WHITE, rim=True):
    """cov: shape coverage. Returns float RGBA (premultiplied-free, straight) at the same resolution."""
    h, w = cov.shape
    holes = holes_of(cov)
    hole_fill = dilate(holes.astype(np.float32), max(2, h // 160))
    rgb_shape = np.empty((h, w, 3), np.float32)
    rgb_shape[:] = shape_rgb
    rgb_hole = np.empty((h, w, 3), np.float32)
    rgb_hole[:] = hole_rgb
    if hi_mask is not None and hi_mask.any():
        rgb_shape[hi_mask] = hi_rgb
        rgb_hole[hi_mask] = hi_hole_rgb
    a_s = cov[..., None]
    a_h = hole_fill[..., None] * (1 - a_s)
    a = a_s + a_h
    rgb = (rgb_shape * a_s + rgb_hole * a_h) / np.maximum(a, 1e-6)
    if rim:
        r_px = max(2, int(round(max(h, w) * 0.022)))
        ring = dilate(a[..., 0], r_px)
        ring = np.asarray(Image.fromarray((ring * 255).astype(np.uint8), 'L').filter(ImageFilter.GaussianBlur(r_px * 0.35)),
                          dtype=np.float32) / 255.0
        a_r = (ring * 0.78)[..., None] * (1 - a)
        a2 = a + a_r
        rgb = (rgb * a + np.array(RIM, np.float32) * a_r) / np.maximum(a2, 1e-6)
        a = a2
    return np.dstack([rgb, a[..., 0] * 255.0]).astype(np.float32)


# ------------------------------------------------------------------------------------------------ pictures
def picture(k, spec, mod=''):
    """(high-resolution RGBA float image, (x, y) of its top-left on the family's shared em canvas)."""
    kind = spec[0]
    org = (0, 0)
    if kind == 'btn':
        _, fam, name, style = spec
        cov, org = k.alpha(fam, name)
        if style.startswith('xb:'):
            img = colourise(cov, XBOX[style[3:]], WHITE)
        elif style.startswith('ps:'):
            img = colourise(cov, WHITE, PSC[style[3:]])
        else:
            img = colourise(cov, WHITE, DARK)
    elif kind in ('dpad', 'arrows', 'mouse'):
        name = {'dpad': PAD_PREFIX.get(spec[1], '') + '_dpad', 'arrows': 'keyboard_arrows', 'mouse': 'mouse'}[kind]
        fam = spec[1] if kind == 'dpad' else 'kb'
        hi = spec[2] if kind == 'dpad' else spec[1]
        cov, org = k.alpha(fam, name)
        img = colourise(cov, WHITE, DARK, region_mask(cov.shape, bbox(cov), hi, kind), HILITE, WHITE)
    elif kind == 'stick':
        _, fam, side, d = spec
        cov, org = k.alpha(fam, PAD_PREFIX[fam] + '_stick_' + side + ('_' + d if d else ''))
        img = colourise(cov, WHITE, DARK)
    elif kind == 'wasd':
        img = wasd(k, spec[1])
    else:
        raise ValueError(spec)
    if mod == 'dark':
        img[..., :3] *= 0.78
    return img, org


def abs_box(pic):
    img, (ox, oy) = pic
    b = bbox(img[..., 3] / 255.0)
    return (b[0] + ox, b[1] + oy, b[2] + ox, b[3] + oy)


def on_box(pic, box):
    """The picture as an array covering `box` (shared em-canvas coordinates)."""
    img, (ox, oy) = pic
    out = np.zeros((box[3] - box[1], box[2] - box[0], 4), np.float32)
    x0, y0 = ox - box[0], oy - box[1]
    sx0, sy0 = max(0, -x0), max(0, -y0)
    dx0, dy0 = max(0, x0), max(0, y0)
    w = min(img.shape[1] - sx0, out.shape[1] - dx0)
    h = min(img.shape[0] - sy0, out.shape[0] - dy0)
    if w > 0 and h > 0:
        out[dy0:dy0 + h, dx0:dx0 + w] = img[sy0:sy0 + h, sx0:sx0 + w]
    return out


def wasd(k, hi):
    """W / A S D keycaps in an inverted T, the key of the stick direction highlighted."""
    keys = {'w': (1, 0), 'a': (0, 1), 's': (1, 1), 'd': (2, 1)}
    lit = {'up': 'w', 'left': 'a', 'down': 's', 'right': 'd'}.get(hi)
    tiles = {}
    for ch in keys:
        cov = k.alpha('kb', 'keyboard_' + ch)[0]
        b = bbox(cov)
        cov = cov[b[1]:b[3], b[0]:b[2]]
        tiles[ch] = colourise(cov, HILITE if ch == lit else WHITE, WHITE if ch == lit else DARK, rim=False)
    th, tw = tiles['w'].shape[:2]
    gap = tw // 14
    W, H = tw * 3 + gap * 2, th * 2 + gap
    out = np.zeros((H, W, 4), np.float32)
    for ch, (cx, cy) in keys.items():
        t = tiles[ch]
        y, x = cy * (th + gap), cx * (tw + gap)
        out[y:y + t.shape[0], x:x + t.shape[1]] = t
    pad = W // 20
    out = np.pad(out, ((pad, pad), (pad, pad), (0, 0)))
    a = out[..., 3] / 255.0
    r_px = max(2, int(round(max(out.shape[:2]) * 0.018)))
    ring = dilate(a, r_px) * 0.78 * (1 - a)
    rgb = (out[..., :3] * a[..., None] + np.array(RIM, np.float32) * ring[..., None]) / np.maximum(a + ring, 1e-6)[..., None]
    return np.dstack([rgb, (a + ring) * 255.0]).astype(np.float32)


def to_pil(img):
    return Image.fromarray(np.clip(img + 0.5, 0, 255).astype(np.uint8), 'RGBA')


def fit(img, src_box, dst_w, dst_h, box):
    """Scale the src_box part of img (aspect kept) into box (x0,y0,x1,y1, output pixels), centred, on a dst canvas."""
    x0, y0, x1, y1 = src_box
    crop = to_pil(img[y0:y1, x0:x1])
    bw, bh = box[2] - box[0], box[3] - box[1]
    s = min(bw / crop.width, bh / crop.height)
    nw, nh = max(1, int(round(crop.width * s))), max(1, int(round(crop.height * s)))
    small = crop.convert('RGBa').resize((nw, nh), Image.LANCZOS).convert('RGBA')
    canvas = Image.new('RGBA', (dst_w, dst_h), (0, 0, 0, 0))
    cx, cy = (box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0
    canvas.alpha_composite(small, (int(round(cx - nw / 2.0)), int(round(cy - nh / 2.0))))
    return canvas


def footprint(orig, min_frac=0.0, square=True):
    """Box (output pixels, 4x) of the original picture's visible part, squared around its centre."""
    h, w = orig.shape[:2]
    b = bbox(orig[..., 3].astype(np.float32) / 255.0, 0.15) or (0, 0, w, h)
    cx, cy = (b[0] + b[2]) / 2.0, (b[1] + b[3]) / 2.0
    bw, bh = b[2] - b[0], b[3] - b[1]
    if square:
        bw = bh = max(bw, bh)
    bw = max(bw, min_frac * min(w, h))
    bh = max(bh, min_frac * min(w, h))
    bw, bh = min(bw, w), min(bh, h)
    cx = min(max(cx, bw / 2.0), w - bw / 2.0)
    cy = min(max(cy, bh / 2.0), h - bh / 2.0)
    return tuple(int(round(v * SCALE)) for v in (cx - bw / 2.0, cy - bh / 2.0, cx + bw / 2.0, cy + bh / 2.0))


def glow(pic_rgba):
    """I4/I8 glow silhouette: intensity = alpha (R=G=B=A like the original intensity texture)."""
    a = np.asarray(pic_rgba, dtype=np.float32)[..., 3] / 255.0
    r = max(2, pic_rgba.width // 24)
    a = dilate(a, r)
    a = np.asarray(Image.fromarray((a * 255).astype(np.uint8), 'L').filter(ImageFilter.GaussianBlur(r * 0.6)))
    return Image.fromarray(np.dstack([a, a, a, a]).astype(np.uint8), 'RGBA')


# ------------------------------------------------------------------------------------------------ sets
def build_set(k, set_name, glyph, decoded, out_dir, log):
    fam, variant = SETS[set_name]
    R = roles(fam, variant)
    written = []
    os.makedirs(out_dir, exist_ok=True)
    # remove our own previous output (only tex1_ files) so a changed mapping leaves no stale picture
    for f in os.listdir(out_dir):
        if f.startswith('tex1_') and f.endswith('.png'):
            os.remove(os.path.join(out_dir, f))
    group_box = {}
    for grp in GROUPS:
        specs = [R[TEXTURES[t][0]] for t in grp]
        if specs[0][0] in ('keep', 'blank'):
            continue
        pics = [picture(k, s) for s in specs]
        ub = union([abs_box(p) for p in pics])
        for t, p in zip(grp, pics):
            arr = on_box(p, ub)
            group_box[t] = (arr, (0, 0, arr.shape[1], arr.shape[0]))
    for tex, (role, mod) in TEXTURES.items():
        row = glyph[tex]
        w, h = (int(v) for v in row['size'].split('x'))
        orig = load_original(decoded, tex)
        if role == 'BLANK':
            img = Image.new('RGBA', (w * SCALE, h * SCALE), (0, 0, 0, 0))
        else:
            spec = R[role]
            if spec[0] == 'keep':
                continue
            if spec[0] == 'blank':
                img = Image.new('RGBA', (w * SCALE, h * SCALE), (0, 0, 0, 0))
            else:
                if tex in group_box:
                    pic, sb = group_box[tex]
                    if mod == 'dark':
                        pic = pic.copy()
                        pic[..., :3] *= 0.78
                else:
                    pic = picture(k, spec, mod if mod != 'glow' else '')[0]
                    sb = bbox(pic[..., 3] / 255.0)
                box = footprint(orig, 0.62 if role in ('B', 'ONE', 'TWO') else 0.0,
                                square=not tex.startswith('th_remoCon') and not tex.startswith('th_nunchaku_'))
                img = fit(pic, sb, w * SCALE, h * SCALE, box)
                if mod == 'glow':
                    img = glow(img)
        name = row['dolphin_name']
        img.save(os.path.join(out_dir, name), optimize=True)
        written.append(name)
        log.append('%s\t%s\t%s\t%s' % (set_name, tex, role + ('/' + mod if mod else ''), name))
    written += build_font(k, R, decoded, out_dir, set_name, log)
    for name in written:  # the "_m" twin as a hard link (Dolphin packs ship both; the loader shares the entry)
        twin = name.replace('tex1_%s_' % name.split('_')[1], 'tex1_%s_m_' % name.split('_')[1], 1)
        src, dst = os.path.join(out_dir, name), os.path.join(out_dir, twin)
        try:
            os.link(src, dst)
        except OSError:
            import shutil
            shutil.copy2(src, dst)
    with open(os.path.join(out_dir, 'glyphs.ini'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('[glyphs]\nset = %s\ndevice = %s\npreset = %s\ngenerator = outputs/pc-tools/glyphs/make_glyph_sets.py v%d\n'
                'art = Kenney "Input Prompts" 1.5, CC0 1.0 (www.kenney.nl), recoloured\ntextures = %d\n'
                % (set_name, {'xb': 'xbox', 'ps': 'playstation', 'sw': 'switch', 'kb': 'keyboard+mouse'}[fam],
                   'Switch HD / Toetsenbord + muis' if variant == 'hd' else 'Klassiek / Wii (beweging)', VERSION,
                   len(written)))
    return written


def build_font(k, R, decoded, out_dir, set_name, log):
    written = []
    # per glyph index: the picture (shared transforms for the stick and D-pad groups)
    pics = {}
    for grp in FONT_GROUPS + [[i] for i in FONT_ROLES if not any(i in g for g in FONT_GROUPS)]:
        members = [i for i in grp if i in FONT_ROLES and R[FONT_ROLES[i]][0] not in ('keep', 'blank')]
        if not members:
            continue
        # D-pad group members can differ in kind (HD: up = a face button): share the transform per kind
        by_kind = {}
        for i in members:
            spec = R[FONT_ROLES[i]]
            by_kind.setdefault(spec[0] + (spec[1] if spec[0] in ('btn',) else ''), []).append(i)
        for ids in by_kind.values():
            ps = {i: picture(k, R[FONT_ROLES[i]]) for i in ids}
            ub = union([abs_box(p) for p in ps.values()])
            for i, p in ps.items():
                arr = on_box(p, ub)
                pics[i] = (arr, (0, 0, arr.shape[1], arr.shape[0]))
    for s, h in enumerate(FONT_SHEETS):
        orig = load_original(decoded, 'picture_00.sheet_%d' % s)
        base = Image.fromarray(orig, 'RGBA').convert('RGBa').resize((orig.shape[1] * SCALE, orig.shape[0] * SCALE),
                                                                      Image.LANCZOS).convert('RGBA')
        changed = False
        for c in range(9):
            i = s * 9 + c
            if i not in pics:
                continue
            x0, y0 = 1 + (c % 3) * 39, 1 + (c // 3) * 40
            gw = FONT_W[i]
            cell = orig[y0:y0 + 39, x0:x0 + gw]
            b = bbox(cell[..., 3].astype(np.float32) / 255.0, 0.15) or (0, 0, gw, 39)
            # icon box: the original glyph's vertical extent (min 30 px), full advance width minus 1 px each side
            cy = y0 + (b[1] + b[3]) / 2.0
            hh = max(30.0, b[3] - b[1])
            box = (int((x0 + 0.75) * SCALE), int((cy - hh / 2.0) * SCALE), int((x0 + gw - 0.75) * SCALE),
                   int((cy + hh / 2.0) * SCALE))
            pic, sb = pics[i]
            icon = fit(pic, sb, base.width, base.height, box)
            clear = Image.new('RGBA', (38 * SCALE, 39 * SCALE), (0, 0, 0, 0))
            base.paste(clear, (x0 * SCALE, y0 * SCALE))
            base.alpha_composite(icon.crop((x0 * SCALE, y0 * SCALE, (x0 + 38) * SCALE, (y0 + 39) * SCALE)),
                                 (x0 * SCALE, y0 * SCALE))
            changed = True
            log.append('%s\tpicture_00 0x%02X\t%s\ttex1_128x128_%s_2.png' % (set_name, 0x20 + i, FONT_ROLES[i], h))
        if changed:
            name = 'tex1_128x128_%s_2.png' % h
            base.save(os.path.join(out_dir, name), optimize=True)
            written.append(name)
    return written


def preview(out_dir, set_name, glyph, prev_dir):
    """Contact sheet: every picture of the set on a checkerboard, tinted like the HUD (C0 black, C1 warm white)."""
    names = sorted(f for f in os.listdir(out_dir) if f.startswith('tex1_') and '_m_' not in f)
    cell = 140
    cols = 8
    rows = (len(names) + cols - 1) // cols
    sheet = Image.new('RGBA', (cols * cell, rows * (cell + 14)), (0, 0, 0, 255))
    d = ImageDraw.Draw(sheet)
    by_name = {r['dolphin_name']: t for t, r in glyph.items()}
    for n, f in enumerate(names):
        im = Image.open(os.path.join(out_dir, f)).convert('RGBA')
        im.thumbnail((cell - 8, cell - 8), Image.LANCZOS)
        x, y = (n % cols) * cell, (n // cols) * (cell + 14)
        for yy in range(0, cell, 16):
            for xx in range(0, cell, 16):
                d.rectangle((x + xx, y + yy, x + xx + 15, y + yy + 15),
                            fill=(70, 110, 160) if ((xx + yy) // 16) % 2 else (95, 140, 190))
        sheet.alpha_composite(im, (x + (cell - im.width) // 2, y + (cell - im.height) // 2))
        d.text((x + 2, y + cell), by_name.get(f, f[5:30])[:22], fill=(255, 255, 0, 255))
    os.makedirs(prev_dir, exist_ok=True)
    sheet.save(os.path.join(prev_dir, set_name + '.png'))


def check(out_root, glyph):
    """Verify every set folder: expected names present, 4x size, some alpha, _m twin = same bytes."""
    import struct
    bad = 0
    sizes = {r['dolphin_name']: tuple(int(v) for v in r['size'].split('x')) for r in glyph.values()}
    for s in SETS:
        d = os.path.join(out_root, s)
        if not os.path.isdir(d):
            print('FAIL missing set', s)
            bad += 1
            continue
        files = [f for f in os.listdir(d) if f.startswith('tex1_') and '_m_' not in f]
        for f in files:
            with open(os.path.join(d, f), 'rb') as fh:
                hdr = fh.read(24)
            w, h = struct.unpack('>II', hdr[16:24])
            ow, oh = sizes.get(f, (128, 128))
            if (w, h) != (ow * SCALE, oh * SCALE):
                print('FAIL size', s, f, w, h)
                bad += 1
            twin = f.replace('tex1_%s_' % f.split('_')[1], 'tex1_%s_m_' % f.split('_')[1], 1)
            if not os.path.isfile(os.path.join(d, twin)):
                print('FAIL no _m twin', s, f)
                bad += 1
        print('set %-15s %3d pictures' % (s, len(files)))
    print('check:', 'OK' if bad == 0 else '%d problems' % bad)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sets', default=','.join(SETS))
    ap.add_argument('--out', default=OUT_DEFAULT)
    ap.add_argument('--kenney', default=KENNEY)
    ap.add_argument('--preview', default=os.path.join(ROOT, 'work', 'ctrl18', 'glyph-sets'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    glyph, decoded = load_tables()
    if a.check:
        sys.exit(1 if check(a.out, glyph) else 0)
    if not os.path.isfile(os.path.join(a.kenney, 'License.txt')) or 'CC0' not in open(os.path.join(a.kenney, 'License.txt'), encoding='utf-8', errors='replace').read():
        sys.exit('Kenney pack (CC0) not found at ' + a.kenney)
    k = Kenney(a.kenney)
    log = []
    for s in a.sets.split(','):
        n = build_set(k, s, glyph, decoded, os.path.join(a.out, s), log)
        preview(os.path.join(a.out, s), s, glyph, a.preview)
        print('%-15s %3d pictures -> %s' % (s, len(n), os.path.join(a.out, s)))
    os.makedirs(a.preview, exist_ok=True)
    with open(os.path.join(a.preview, 'mapping.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('set\ttexture\trole\tfile\n' + '\n'.join(log) + '\n')
    lic = os.path.join(a.out, '..', 'LEESMIJ-knopiconen.txt')
    with open(lic, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('Knopiconen (CTRL18) voor SkyBreak\n\n'
                'Elke map in SOUE01/ is een set knopplaatjes met Dolphin-namen (instelling [graphics] glyph_set,\n'
                'launcher > Besturing). De plaatjes zijn gemaakt met outputs/pc-tools/glyphs/make_glyph_sets.py uit\n'
                '"Input Prompts" 1.5 van Kenney (www.kenney.nl), licentie Creative Commons Zero (CC0 1.0):\n'
                'http://creativecommons.org/publicdomain/zero/1.0/ - vrij te gebruiken, naamsvermelding niet verplicht.\n')
    print('done; mapping:', os.path.join(a.preview, 'mapping.tsv'))


if __name__ == '__main__':
    main()
