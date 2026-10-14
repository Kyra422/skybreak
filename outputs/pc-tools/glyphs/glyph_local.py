"""REL22: make the SkyBreak button icons (glyph sets) on the PLAYER'S PC, from their own disc. Python standard library only.

The package ships only stencils/ (recoloured Kenney "Input Prompts" pictures, CC0, made by make_glyph_stencils.py) and
stencils/recipe.json (names + where each picture goes). The parts that come from the game - the size and position of the game's
own button pictures, and the picture-font sheet whose other glyphs stay as they are - are read here from the player's own
extracted disc (<game>/files/...: Layout archives and Font/picture_00.brfnt) and never leave this PC.

The result is bit-identical to outputs/pc-tools/glyphs/make_glyph_sets.py (Pillow + NumPy on the developer PC): this file
re-implements exactly the Pillow operations that script uses (RGBA<->RGBa, LANCZOS resize, alpha_composite, MaxFilter,
GaussianBlur) with Pillow's integer arithmetic. Tested in outputs/pc-tests/rel22/test_glyph_local.py.

Usage (called by Bouw-SkywardSword.ps1 step 8):
  python glyph_local.py --game <extracted disc root (sys/, files/)> --out <install>/textures/glyphs/SOUE01 [--jobs N]
  --force   rebuild even when the stamp says the sets are current
Exit 0 = sets written (or already current); 1 = failed (the old sets, if any, are left untouched; the game then shows the
original Wii pictures). Writes a new folder next to --out and swaps it in at the end.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import hashlib
import json
import math
import os
import shutil
import struct
import sys
import time
import zlib
from operator import mul

HERE = os.path.dirname(os.path.abspath(__file__))
STENCILS = os.path.join(HERE, 'stencils')
sys.path.insert(0, os.path.join(HERE, '..', 'hdtex'))
import gx_texture as GX  # noqa: E402  (our GX decoder + Dolphin texture names, stdlib only)

TOOL_VERSION = 1
MUTATE = os.environ.get('SKYBREAK_GLYPH_MUTATE', '')  # tests only: 'bilinear' / 'no-footprint' must break the bit-exact check


# ------------------------------------------------------------------------------------------------ PNG (8-bit RGBA)
def png_read(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG: ' + path)
    pos, idat, w = 8, [], 0
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
            if (depth, ctype, inter) != (8, 6, 0):
                raise ValueError('only 8-bit RGBA non-interlaced PNG: ' + path)
        elif typ == b'IDAT':
            idat.append(body)
        elif typ == b'IEND':
            break
        pos += 12 + n
    raw = zlib.decompress(b''.join(idat))
    stride = w * 4
    if all(raw[y * (stride + 1)] == 0 for y in range(h)):  # filter 0 everywhere (make_glyph_stencils writes that): no loops
        return w, h, bytearray(b''.join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h)))
    out = bytearray(stride * h)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:
            for x in range(4, stride):
                line[x] = (line[x] + line[x - 4]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                line[x] = (line[x] + ((line[x - 4] if x >= 4 else 0) + prev[x]) // 2) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - 4] if x >= 4 else 0
                b = prev[x]
                c = prev[x - 4] if x >= 4 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, out


def png_encode(w, h, rgba, level=9):
    """8-bit RGBA PNG, filter 0 on every row."""
    stride = w * 4
    raw = b''.join(b'\x00' + bytes(rgba[y * stride:(y + 1) * stride]) for y in range(h))

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, level)) + chunk(b'IEND', b''))


def png_write(path, w, h, rgba):
    with open(path, 'wb') as f:
        f.write(png_encode(w, h, rgba, 6))


# ------------------------------------------------------------------------------------------------ disc containers
def _u16(d, p):
    return struct.unpack_from('>H', d, p)[0]


def _u32(d, p):
    return struct.unpack_from('>I', d, p)[0]


def u8_find(data, internal):
    """The file whose U8 path ends with `internal` (layout archives keep their pictures under arc/timg/...)."""
    if data[:4] != b'\x55\xaa\x38\x2d':
        raise ValueError('not a U8 archive')
    root, header_size = _u32(data, 4), _u32(data, 8)
    count = _u32(data, root + 8)
    names, names_end = root + count * 12, root + header_size
    stack = [(count, '')]
    want = internal.lower().strip('/')
    for i in range(1, count):
        while stack and i >= stack[-1][0]:
            stack.pop()
        pos = root + i * 12
        packed = _u32(data, pos)
        np_ = names + (packed & 0xffffff)
        end = data.index(b'\0', np_, names_end)
        path = (stack[-1][1] + '/' + data[np_:end].decode('ascii', 'replace')).lstrip('/')
        if packed >> 24 == 1:
            stack.append((_u32(data, pos + 8), path))
        else:
            low = path.lower()
            if low == want or low.endswith('/' + want):
                start, size = _u32(data, pos + 4), _u32(data, pos + 8)
                return data[start:start + size]
    raise ValueError('not in the archive: ' + internal)


def tpl_image(data, index):
    if data[:4] != b'\x00\x20\xaf\x30':
        raise ValueError('not a TPL')
    count, table = _u32(data, 4), _u32(data, 8)
    if index >= count:
        raise ValueError('TPL image %d of %d' % (index, count))
    p, pal = _u32(data, table + index * 8), _u32(data, table + index * 8 + 4)
    h, w, fmt, do = _u16(data, p), _u16(data, p + 2), _u32(data, p + 4), _u32(data, p + 8)
    palette, pf = b'', 0
    if pal:
        n, pf, po = _u16(data, pal), _u32(data, pal + 4), _u32(data, pal + 8)
        palette = data[po:po + n * 2]
    return w, h, fmt, data[do:do + GX.base_level_size(w, h, fmt)], palette, pf


def brfnt_sheet(data, sheet):
    if data[:4] not in (b'RFNT', b'RFNU') or _u16(data, 4) != 0xfeff:
        raise ValueError('not a big-endian RFNT font')
    pos, count = _u16(data, 12), _u16(data, 14)
    for _ in range(count):
        size = _u32(data, pos + 4)
        if data[pos:pos + 4] == b'TGLP':
            q = pos + 8
            sheet_size, sheets, fmt = _u32(data, q + 4), _u16(data, q + 8), _u16(data, q + 10)
            w, h, start = _u16(data, q + 16), _u16(data, q + 18), _u32(data, q + 20)
            if sheet >= sheets:
                raise ValueError('font sheet %d of %d' % (sheet, sheets))
            return w, h, fmt, data[start + sheet * sheet_size:start + (sheet + 1) * sheet_size]
        pos += size
    raise ValueError('no TGLP block')


def load_originals(game, sources):
    """{texture: (w, h, rgba)} decoded from the player's own disc; each one must carry the expected Dolphin name."""
    files = os.path.join(game, 'files')
    cache, out = {}, {}
    for tex, s in sources.items():
        arc = os.path.join(files, *s['archive'].split('/'))
        if arc not in cache:
            with open(arc, 'rb') as f:
                cache[arc] = f.read()
        data = cache[arc]
        if s['kind'] == 'brfnt':
            w, h, fmt, base = brfnt_sheet(data if data[:4] in (b'RFNT', b'RFNU') else u8_find(data, s['internal']), s['sheet'])
            palette, pf = b'', 0
        else:
            w, h, fmt, base, palette, pf = tpl_image(u8_find(data, s['internal']), s['image'])
        name = GX.dolphin_name(w, h, fmt, base, palette)
        if (w, h, fmt) != (s['w'], s['h'], s['fmt']) or name != s['dolphin']:
            raise ValueError('%s in %s is not the expected picture (%s, expected %s): not a SOUE01 rev 0 disc?'
                             % (tex, s['archive'], name, s['dolphin']))
        out[tex] = (w, h, bytearray(GX.decode_rgba(w, h, fmt, base, palette, pf)))
    return out


# ------------------------------------------------------------------------------------------------ Pillow-exact image operations
# Images are (w, h, bytearray RGBA). See Pillow: Convert.c (rgba2rgbA / rgbA2rgba), Resample.c (LANCZOS, 8bpc, PRECISION_BITS
# 22), AlphaComposite.c, Filter.c (ImagingExpand + RankFilter), BoxBlur.c (ImagingGaussianBlur, 3 passes).
_PRE = [bytes(((c * a + 128) + ((c * a + 128) >> 8)) >> 8 for c in range(256)) for a in range(256)]  # MULDIV255
_UNPRE = [bytes(range(256)) if a in (0, 255) else bytes(min(255, (255 * c) // a) for c in range(256)) for a in range(256)]
_CLIP = [0] * 1024 + list(range(256)) + [255] * 1024
_PREC = 22
_HALF = 1 << (_PREC - 1)


def premultiply(img):
    w, h, px = img
    out = bytearray(px)
    a = px[3::4]
    for ch in range(3):
        out[ch::4] = bytes(_PRE[av][cv] for av, cv in zip(a, px[ch::4]))
    return w, h, out


def unpremultiply(img):
    w, h, px = img
    out = bytearray(px)
    a = px[3::4]
    for ch in range(3):
        out[ch::4] = bytes(_UNPRE[av][cv] for av, cv in zip(a, px[ch::4]))
    return w, h, out


def _sinc(x):
    if x == 0.0:
        return 1.0
    x = x * math.pi
    return math.sin(x) / x


def _lanczos(x):
    if -3.0 <= x < 3.0:
        return _sinc(x) * _sinc(x / 3)
    return 0.0


def _bilinear(x):  # tests only (mutant)
    x = abs(x)
    return 1.0 - x if x < 1.0 else 0.0


def _coeffs(in_size, out_size):
    filt, sup = (_bilinear, 1.0) if MUTATE == 'bilinear' else (_lanczos, 3.0)
    scale = float(in_size) / out_size
    filterscale = scale if scale > 1.0 else 1.0
    support = sup * filterscale
    bounds, kk = [], []
    for xx in range(out_size):
        center = (xx + 0.5) * scale
        ss = 1.0 / filterscale
        xmin = int(center - support + 0.5)
        if xmin < 0:
            xmin = 0
        xmax = int(center + support + 0.5)
        if xmax > in_size:
            xmax = in_size
        xmax -= xmin
        ws = [filt((x + xmin - center + 0.5) * ss) for x in range(xmax)]
        tot = 0.0  # Pillow adds the weights one by one in a double
        for v in ws:
            tot += v
        if tot != 0.0:
            ws = [v / tot for v in ws]
        kk.append([int(-0.5 + v * (1 << _PREC)) if v < 0 else int(0.5 + v * (1 << _PREC)) for v in ws])
        bounds.append((xmin, xmax))
    return bounds, kk


def resize(img, nw, nh):
    """Pillow Image.resize((nw, nh), LANCZOS) on an RGBa (premultiplied) 8-bit image."""
    w, h, px = img
    if (nw, nh) == (w, h):
        return w, h, bytearray(px)
    bh, kh = _coeffs(w, nw)
    bv, kv = _coeffs(h, nh)
    planes = [list(px[c::4]) for c in range(4)]
    clip = _CLIP
    if nw != w:
        y0, y1 = bv[0][0], bv[-1][0] + bv[-1][1]
        bv = [(a - y0, n) for a, n in bv]
        new = []
        for P in planes:
            out = []
            app = out.append
            for y in range(y0, y1):
                row = P[y * w:(y + 1) * w]
                for (xmin, n), k in zip(bh, kh):
                    app(clip[((sum(map(mul, row[xmin:xmin + n], k)) + _HALF) >> _PREC) + 1024])
            new.append(out)
        planes, w, h = new, nw, y1 - y0
    if nh != h:
        new = []
        for P in planes:
            out = [0] * (nh * w)
            for x in range(w):
                col = P[x::w]
                for yy, ((ymin, n), k) in enumerate(zip(bv, kv)):
                    out[yy * w + x] = clip[((sum(map(mul, col[ymin:ymin + n], k)) + _HALF) >> _PREC) + 1024]
            new.append(out)
        planes, h = new, nh
    res = bytearray(w * h * 4)
    for c in range(4):
        res[c::4] = bytes(planes[c])
    return w, h, res


def _f(v):  # AlphaComposite.c with dst = (0,0,0,0): out channel for source channel v (src alpha > 0), PRECISION_BITS 7
    t = v * (255 * 128) + (0x80 << 7)
    return (((t >> 8) + t) >> 8) >> 7


_COMP_C = bytes(_f(v) for v in range(256))
_COMP_A = bytes(((((a * 255 + 0x80) >> 8) + a * 255 + 0x80) >> 8) for a in range(256))


def composite_onto_clear(src):
    """Image.alpha_composite(transparent, src): alpha 0 -> (0,0,0,0), else the integer blend with a clear background."""
    w, h, px = src
    out = bytearray(px.translate(_COMP_C))
    out[3::4] = bytes(px[3::4]).translate(_COMP_A)
    mask = bytes(px[3::4]).translate(bytes([0] + [255] * 255))
    m4 = bytes(b for v in mask for b in (v, v, v, v))
    out = bytearray((int.from_bytes(out, 'big') & int.from_bytes(m4, 'big')).to_bytes(len(out), 'big'))
    return w, h, out


def place(canvas, small, dx, dy, region=None):
    """canvas.alpha_composite(small, (dx, dy)) for a canvas that is clear where `small` lands (Pillow crops at the edges);
    region (x0, y0, x1, y1) limits the written area (the font cell)."""
    cw, ch, cpx = canvas
    sw, sh, spx = composite_onto_clear(small)
    x0, y0, x1, y1 = region or (0, 0, cw, ch)
    for y in range(sh):
        ty = dy + y
        if ty < y0 or ty >= y1:
            continue
        a, b = max(dx, x0), min(dx + sw, x1)
        if a >= b:
            continue
        cpx[(ty * cw + a) * 4:(ty * cw + b) * 4] = spx[(y * sw + a - dx) * 4:(y * sw + b - dx) * 4]


def clear(canvas, x0, y0, x1, y1):
    cw, ch, cpx = canvas
    for y in range(max(0, y0), min(ch, y1)):
        cpx[(y * cw + max(0, x0)) * 4:(y * cw + min(cw, x1)) * 4] = bytes((min(cw, x1) - max(0, x0)) * 4)


def bbox_alpha(px, w, h, thr_byte):
    """make_glyph_sets.bbox(alpha/255, thr) on an 8-bit alpha: first/last column and row with alpha >= thr_byte."""
    a = px[3::4]
    xs0, ys0, xs1, ys1 = w, h, -1, -1
    for y in range(h):
        row = a[y * w:(y + 1) * w]
        hit = [x for x, v in enumerate(row) if v >= thr_byte]
        if hit:
            ys0, ys1 = min(ys0, y), y
            xs0, xs1 = min(xs0, hit[0]), max(xs1, hit[-1])
    if xs1 < 0:
        return None
    return xs0, ys0, xs1 + 1, ys1 + 1


def footprint(orig, scale, min_frac=0.0, square=True):
    """make_glyph_sets.footprint(): box (output pixels) of the original picture's visible part (alpha > 0.15)."""
    w, h, px = orig
    b = bbox_alpha(px, w, h, 39) or (0, 0, w, h)  # float32(a)/255 > 0.15  <=>  a >= 39
    if MUTATE == 'no-footprint':
        b = (0, 0, w, h)
    cx, cy = (b[0] + b[2]) / 2.0, (b[1] + b[3]) / 2.0
    bw, bh = b[2] - b[0], b[3] - b[1]
    if square:
        bw = bh = max(bw, bh)
    bw = max(bw, min_frac * min(w, h))
    bh = max(bh, min_frac * min(w, h))
    bw, bh = min(bw, w), min(bh, h)
    cx = min(max(cx, bw / 2.0), w - bw / 2.0)
    cy = min(max(cy, bh / 2.0), h - bh / 2.0)
    return tuple(int(round(v * scale)) for v in (cx - bw / 2.0, cy - bh / 2.0, cx + bw / 2.0, cy + bh / 2.0))


def fit_size(sw, sh, box):
    bw, bh = box[2] - box[0], box[3] - box[1]
    s = min(bw / sw, bh / sh)
    nw, nh = max(1, int(round(sw * s))), max(1, int(round(sh * s)))
    cx, cy = (box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0
    return nw, nh, int(round(cx - nw / 2.0)), int(round(cy - nh / 2.0))


def _f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def _box_radius(radius, passes=3):
    """BoxBlur.c _gaussian_blur_radius() in C float (each operation rounded to float32, MSVC x64)."""
    r = _f32(radius)
    sigma2 = _f32(_f32(r * r) / passes)
    L = _f32(math.sqrt(12.0 * sigma2 + 1.0))
    l = _f32(math.floor((L - 1.0) / 2.0))
    a = _f32(_f32(_f32(2 * l) + 1) * _f32(_f32(l * _f32(l + 1)) - _f32(3 * sigma2)))
    a = _f32(a / _f32(6 * _f32(sigma2 - _f32(_f32(l + 1) * _f32(l + 1)))))
    return _f32(l + a)


def _box_line(line, radius, ww, fw):
    lastx = len(line) - 1
    edge_a, edge_b = min(radius + 1, len(line)), max(len(line) - radius - 1, 0)
    acc = line[0] * (radius + 1)
    for x in range(edge_a - 1):
        acc += line[x]
    acc += line[lastx] * (radius - edge_a + 1)
    out = [0] * len(line)

    def save(x, bulk):
        out[x] = ((bulk + (1 << 23)) >> 24) & 255
    if edge_a <= edge_b:
        for x in range(edge_a):
            acc += line[x + radius] - line[0]
            save(x, acc * ww + (line[0] + line[x + radius + 1]) * fw)
        for x in range(edge_a, edge_b):
            acc += line[x + radius] - line[x - radius - 1]
            save(x, acc * ww + (line[x - radius - 1] + line[x + radius + 1]) * fw)
        for x in range(edge_b, lastx + 1):
            acc += line[lastx] - line[x - radius - 1]
            save(x, acc * ww + (line[x - radius - 1] + line[lastx]) * fw)
    else:
        for x in range(edge_b):
            acc += line[x + radius] - line[0]
            save(x, acc * ww + (line[0] + line[x + radius + 1]) * fw)
        for x in range(edge_b, edge_a):
            acc += line[lastx] - line[0]
            save(x, acc * ww + (line[0] + line[lastx]) * fw)
        for x in range(edge_a, lastx + 1):
            acc += line[lastx] - line[x - radius - 1]
            save(x, acc * ww + (line[x - radius - 1] + line[lastx]) * fw)
    return out


def gaussian_blur_l(plane, w, h, radius):
    """ImageFilter.GaussianBlur(radius) on an 'L' image (3 box passes per direction)."""
    fr = _box_radius(radius)
    ri = int(fr)
    ww = int(_f32(16777216.0 / _f32(_f32(fr * 2) + 1)))
    fw = ((1 << 24) - (ri * 2 + 1) * ww) // 2
    rows = [plane[y * w:(y + 1) * w] for y in range(h)]
    if fr != 0:
        for _ in range(3):
            rows = [_box_line(r, ri, ww, fw) for r in rows]
    cols = [[rows[y][x] for y in range(h)] for x in range(w)]
    if fr != 0:
        for _ in range(3):
            cols = [_box_line(c, ri, ww, fw) for c in cols]
    return [cols[x][y] for y in range(h) for x in range(w)]


def max_filter(plane, w, h, size):
    """ImageFilter.MaxFilter(size): edge pixels replicated (ImagingExpand) = max over the in-image window."""
    r = size // 2
    rows = []
    for y in range(h):
        row = plane[y * w:(y + 1) * w]
        rows.append([max(row[max(0, x - r):x + r + 1]) for x in range(w)])
    out = [0] * (w * h)
    for x in range(w):
        col = [rows[y][x] for y in range(h)]
        for y in range(h):
            out[y * w + x] = max(col[max(0, y - r):y + r + 1])
    return out


def glow(img, lut):
    """make_glyph_sets.glow(): intensity = blurred, dilated alpha (R=G=B=A)."""
    w, h, px = img
    r = max(2, w // 24)
    a = [lut[v] for v in px[3::4]]
    a = [lut[v] for v in max_filter(a, w, h, r * 2 + 1)]
    b = gaussian_blur_l(a, w, h, r * 0.6)
    return w, h, bytearray(v for v in b for _ in range(4))


# ------------------------------------------------------------------------------------------------ work items (process pool)
_stencil_cache = {}


def _stencil(name):
    if name not in _stencil_cache:
        _stencil_cache[name] = premultiply(png_read(os.path.join(STENCILS, name)))
    return _stencil_cache[name]


def job_resize(args):
    kind, a, nw, nh = args
    src = _stencil(a) if kind == 'stencil' else premultiply(a)  # 'sheet': the original font sheet, upscaled 4x
    return unpremultiply(resize(src, nw, nh))


# ------------------------------------------------------------------------------------------------ main
def stamp_of(recipe_bytes):
    return {'tool': TOOL_VERSION, 'recipe_sha256': hashlib.sha256(recipe_bytes).hexdigest(),
            'glyph_local_sha256': hashlib.sha256(open(os.path.abspath(__file__), 'rb').read()).hexdigest()}


def current(out, stamp, recipe):
    p = os.path.join(out, '.skybreak-glyphs.json')
    try:
        with open(p, encoding='utf-8') as f:
            old = json.load(f)
    except (OSError, ValueError):
        return False
    if any(old.get(k) != v for k, v in stamp.items()):
        return False
    for set_name, s in recipe['sets'].items():
        for e in s['textures'] + s['font']:
            if not os.path.isfile(os.path.join(out, set_name, e['name'])):
                return False
    return True


def build(game, out, jobs, log=print):
    with open(os.path.join(STENCILS, 'recipe.json'), 'rb') as f:
        recipe_bytes = f.read()
    recipe = json.loads(recipe_bytes)
    for n, sha in recipe['stencils'].items():  # the package's own stencils, unchanged
        with open(os.path.join(STENCILS, n), 'rb') as f:
            if hashlib.sha256(f.read()).hexdigest() != sha:
                raise ValueError('stencil changed or damaged: ' + n)
    scale, lut, fc = recipe['scale'], recipe['glow_lut'], recipe['font_cell']
    t0 = time.monotonic()
    orig = load_originals(game, recipe['sources'])
    log('originals: %d pictures from your disc decoded and checked (%.1f s)' % (len(orig), time.monotonic() - t0))

    # 1. every resize that is needed (deduplicated) + the three font sheets
    plan, tasks = [], {}
    for set_name, s in recipe['sets'].items():
        for e in s['textures']:
            if e['op'] != 'fit':
                continue
            sw, sh = png_size(e['stencil'])
            box = footprint(orig[e['tex']], scale, e['min_frac'], e['square'])
            nw, nh, dx, dy = fit_size(sw, sh, box)
            tasks[('stencil', e['stencil'], nw, nh)] = None
            plan.append((set_name, e, (nw, nh, dx, dy)))
        for fs in s['font']:
            ow, oh, opx = orig['picture_00.sheet_%d' % fs['sheet']]
            for cell in fs['cells']:
                c, gw = cell['c'], cell['gw']
                x0, y0 = fc['x0'] + (c % 3) * fc['dx'], fc['y0'] + (c // 3) * fc['dy']
                sub = cut(orig['picture_00.sheet_%d' % fs['sheet']], x0, y0, gw, fc['h'])
                b = bbox_alpha(sub[2], sub[0], sub[1], 39) or (0, 0, gw, fc['h'])
                cy = y0 + (b[1] + b[3]) / 2.0
                hh = max(30.0, b[3] - b[1])
                box = (int((x0 + 0.75) * scale), int((cy - hh / 2.0) * scale), int((x0 + gw - 0.75) * scale), int((cy + hh / 2.0) * scale))
                sw, sh = png_size(cell['stencil'])
                nw, nh, dx, dy = fit_size(sw, sh, box)
                cell['_fit'] = (nw, nh, dx, dy, x0, y0)
                tasks[('stencil', cell['stencil'], nw, nh)] = None
    sheets = sorted({fs['sheet'] for s in recipe['sets'].values() for fs in s['font']})
    t1 = time.monotonic()
    work = [('stencil', k[1], k[2], k[3]) for k in tasks] + [('sheet', orig['picture_00.sheet_%d' % sh], orig['picture_00.sheet_%d' % sh][0] * scale,
                                                              orig['picture_00.sheet_%d' % sh][1] * scale) for sh in sheets]
    results = {}
    if jobs > 1:
        with cf.ProcessPoolExecutor(max_workers=jobs) as ex:
            done = list(ex.map(job_resize, work, chunksize=max(1, len(work) // (jobs * 4))))
    else:
        done = [job_resize(w) for w in work]
    sheet_imgs = []
    for wk, img in zip(work, done):
        if wk[0] == 'stencil':
            results[(wk[1], wk[2], wk[3])] = img
        else:
            sheet_imgs.append(img)
    sheet_up = dict(zip(sheets, sheet_imgs))
    log('resized: %d pictures + %d font sheets in %.1f s (%d processes)' % (len(tasks), len(sheets), time.monotonic() - t1, jobs))

    # 2. compose and write into a fresh folder, then swap it in
    tmp = out + '.nieuw'
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    count = 0
    for set_name, s in recipe['sets'].items():
        d = os.path.join(tmp, set_name)
        os.makedirs(d)
        written = []
        fits = {id(e): f for sn, e, f in plan if sn == set_name}
        for e in s['textures']:
            cw, ch = e['w'] * scale, e['h'] * scale
            canvas = (cw, ch, bytearray(cw * ch * 4))
            if e['op'] == 'fit':
                nw, nh, dx, dy = fits[id(e)]
                place(canvas, results[(e['stencil'], nw, nh)], dx, dy)
                if e.get('glow'):
                    canvas = glow(canvas, lut)
            png_write(os.path.join(d, e['name']), *canvas)
            written.append(e['name'])
        for fs in s['font']:
            base = sheet_up[fs['sheet']]
            base = (base[0], base[1], bytearray(base[2]))
            for cell in fs['cells']:
                nw, nh, dx, dy, x0, y0 = cell['_fit']
                region = (x0 * scale, y0 * scale, (x0 + 38) * scale, (y0 + fc['h']) * scale)
                clear(base, *region)
                # fit() draws the icon on a clear canvas the size of the sheet; that canvas' cell region is then
                # alpha-composited onto the cleared cell: the same blend twice
                canvas = (base[0], base[1], bytearray(base[0] * base[1] * 4))
                place(canvas, results[(cell['stencil'], nw, nh)], dx, dy, region)
                piece = cut(canvas, region[0], region[1], region[2] - region[0], region[3] - region[1])
                place(base, piece, region[0], region[1], region)
            png_write(os.path.join(d, fs['name']), *base)
            written.append(fs['name'])
        for name in written:  # the "_m" twin as a hard link, like make_glyph_sets.py
            tag = name.split('_')[1]
            twin = name.replace('tex1_%s_' % tag, 'tex1_%s_m_' % tag, 1)
            try:
                os.link(os.path.join(d, name), os.path.join(d, twin))
            except OSError:
                shutil.copy2(os.path.join(d, name), os.path.join(d, twin))
        with open(os.path.join(d, 'glyphs.ini'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(s['ini'])
        count += len(written)
    with open(os.path.join(tmp, '.skybreak-glyphs.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(dict(stamp_of(recipe_bytes), made=time.strftime('%Y-%m-%d %H:%M:%S'), pictures=count), f, indent=1)
    with open(os.path.join(os.path.dirname(out), 'LEESMIJ-knopiconen.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('Knopiconen voor SkyBreak (instelling [graphics] glyph_set, launcher > Besturing)\n\n'
                'Gemaakt op deze pc door glyph_local.py: tekeningen uit "Input Prompts" 1.5 van Kenney (www.kenney.nl, CC0 1.0),\n'
                'passend gemaakt op de knopplaatjes van JOUW eigen schijf. Daardoor horen ze bij jouw installatie: niet delen.\n')
    old = out + '.oud'
    shutil.rmtree(old, ignore_errors=True)
    if os.path.isdir(out):
        os.rename(out, old)
    os.rename(tmp, out)
    shutil.rmtree(old, ignore_errors=True)
    return count


_png_sizes = {}


def png_size(name):
    if name not in _png_sizes:
        with open(os.path.join(STENCILS, name), 'rb') as f:
            _png_sizes[name] = struct.unpack('>II', f.read(24)[16:24])
    return _png_sizes[name]


def cut(img, x0, y0, w, h):
    iw, ih, px = img
    out = bytearray(w * h * 4)
    for y in range(h):
        sy = y0 + y
        if 0 <= sy < ih:
            a, b = max(0, x0), min(iw, x0 + w)
            if a < b:
                out[(y * w + a - x0) * 4:(y * w + b - x0) * 4] = px[(sy * iw + a) * 4:(sy * iw + b) * 4]
    return w, h, out


def main():
    ap = argparse.ArgumentParser(description='SkyBreak button icons from your own disc')
    ap.add_argument('--game', required=True, help='extracted disc root (with sys/ and files/)')
    ap.add_argument('--out', required=True, help='<install>/textures/glyphs/SOUE01')
    ap.add_argument('--jobs', type=int, default=0)
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    with open(os.path.join(STENCILS, 'recipe.json'), 'rb') as f:
        stamp = stamp_of(f.read())
    with open(os.path.join(STENCILS, 'recipe.json'), 'rb') as f:
        recipe = json.loads(f.read())
    if not a.force and current(out, stamp, recipe):
        print(json.dumps({'status': 'actueel', 'out': out}))
        return 0
    jobs = a.jobs or max(1, min(8, (os.cpu_count() or 2) - 1))
    t0 = time.monotonic()
    try:
        n = build(os.path.abspath(a.game), out, jobs, log=lambda m: print(m, flush=True))
    except Exception as exc:  # the old sets stay; the game shows the Wii pictures when there are none
        shutil.rmtree(out + '.nieuw', ignore_errors=True)
        print(json.dumps({'status': 'fout', 'melding': '%s: %s' % (type(exc).__name__, exc)}))
        return 1
    print(json.dumps({'status': 'gemaakt', 'pictures': n, 'seconds': round(time.monotonic() - t0, 1), 'out': out}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
