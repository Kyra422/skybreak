"""Disk GX textures: RGBA8 decoder and Dolphin 2606a replacement names.

No downloads or GPU needed. NumPy is optional and used for decoding speed.
Naming follows Dolphin TextureInfo::CalculateTextureName, including its C14X2
byte-swap quirk. See work/hdtex/codec-notes.md for source and compatibility notes.
"""
from __future__ import annotations

import struct

try:
    import numpy as np
except ImportError:
    np = None

FORMAT_NAMES = {0: "I4", 1: "I8", 2: "IA4", 3: "IA8", 4: "RGB565",
                5: "RGB5A3", 6: "RGBA8", 8: "C4", 9: "C8", 10: "C14X2", 14: "CMPR"}
# Format -> block width, height, storage bytes. Entire blocks exist at edges.
FORMATS = {0: (8, 8, 32), 1: (8, 4, 32), 2: (8, 4, 32), 3: (4, 4, 32),
           4: (4, 4, 32), 5: (4, 4, 32), 6: (4, 4, 64), 8: (8, 8, 32),
           9: (8, 4, 32), 10: (4, 4, 32), 14: (8, 8, 32)}
_MASK = (1 << 64) - 1
_P1, _P2, _P3, _P4, _P5 = (11400714785074694791, 14029467366897019727,
                           1609587929392839161, 9650029242287828579, 2870177450012600261)


def _rotl(value: int, count: int) -> int:
    return ((value << count) | (value >> (64 - count))) & _MASK


def _round(acc: int, lane: int) -> int:
    return (_rotl((acc + lane * _P2) & _MASK, 31) * _P1) & _MASK


def xxh64(data: bytes, seed: int = 0) -> int:
    """Reference scalar XXH64, returning its unsigned 64-bit value."""
    data = memoryview(data).cast("B")
    n, pos = len(data), 0
    seed &= _MASK
    if n >= 32:
        v1, v2, v3, v4 = (seed + _P1 + _P2) & _MASK, (seed + _P2) & _MASK, seed, (seed - _P1) & _MASK
        while pos <= n - 32:
            a, b, c, d = struct.unpack_from("<QQQQ", data, pos)
            v1, v2, v3, v4 = _round(v1, a), _round(v2, b), _round(v3, c), _round(v4, d)
            pos += 32
        acc = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & _MASK
        for value in (v1, v2, v3, v4):
            acc = ((acc ^ _round(0, value)) * _P1 + _P4) & _MASK
    else:
        acc = (seed + _P5) & _MASK
    acc = (acc + n) & _MASK
    while pos <= n - 8:
        acc = (_rotl(acc ^ _round(0, struct.unpack_from("<Q", data, pos)[0]), 27) * _P1 + _P4) & _MASK
        pos += 8
    if pos <= n - 4:
        acc ^= (struct.unpack_from("<I", data, pos)[0] * _P1) & _MASK
        acc = (_rotl(acc, 23) * _P2 + _P3) & _MASK
        pos += 4
    while pos < n:
        acc ^= (data[pos] * _P5) & _MASK
        acc = (_rotl(acc, 11) * _P1) & _MASK
        pos += 1
    acc ^= acc >> 33
    acc = (acc * _P2) & _MASK
    acc ^= acc >> 29
    acc = (acc * _P3) & _MASK
    return acc ^ (acc >> 32)


def base_level_size(width: int, height: int, fmt: int) -> int:
    if width <= 0 or height <= 0:
        raise ValueError("Texture dimensions must be positive")
    if fmt not in FORMATS:
        raise ValueError(f"Unsupported GX format {fmt}")
    bw, bh, size = FORMATS[fmt]
    return ((width + bw - 1) // bw) * ((height + bh - 1) // bh) * size


def _base(width: int, height: int, fmt: int, data: bytes) -> bytes:
    size = base_level_size(width, height, fmt)
    if len(data) < size:
        raise ValueError(f"Truncated {FORMAT_NAMES[fmt]} base level: {len(data)} < {size}")
    return data[:size]


def palette_hash_range(fmt: int, base: bytes) -> tuple[int, int] | None:
    """Return [first,last+1) palette ENTRIES hashed by Dolphin 2606a.

    Padding texels participate. C14X2 intentionally mirrors byte-only swap16
    in TextureInfo.cpp; decoding uses the proper complete 14-bit index.
    """
    if fmt == 8:
        lo = min(min(v & 15, v >> 4) for v in base)
        hi = max(max(v & 15, v >> 4) for v in base)
    elif fmt == 9:
        lo, hi = min(base), max(base)
    elif fmt == 10:
        values = [((base[i] << 8) & 0x3fff) for i in range(0, len(base), 2)]
        lo, hi = min(values), max(values)
    else:
        return None
    return lo, hi + 1


def dolphin_name(width: int, height: int, fmt: int, data: bytes,
                 palette: bytes = b"", mipmapped: bool = False) -> str:
    """Exact tex1 stem (without .png); caller supplies runtime mip-filter flag.

    Only the padded base level is hashed. A short disk palette is accepted when
    all bytes in Dolphin's hash range exist; absent entries are never invented.
    Palette encoding does not form part of Dolphin's filename.
    """
    base = _base(width, height, fmt, data)
    name = f"tex1_{width}x{height}{'_m' if mipmapped else ''}_{xxh64(base):016x}"
    region = palette_hash_range(fmt, base)
    if region is not None:
        start, end = region
        if len(palette) < end * 2:
            raise ValueError(f"Palette has {len(palette)//2} entries, Dolphin name needs entries {start}..{end-1}")
        name += f"_{xxh64(palette[start*2:end*2]):016x}"
    return name + f"_{fmt}"


def _rgb565(v: int) -> tuple[int, int, int, int]:
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255


def _rgb5a3(v: int) -> tuple[int, int, int, int]:
    if v & 0x8000:
        r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
        return (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255
    a = (v >> 12) & 7
    return ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (a << 5) | (a << 2) | (a >> 1)


def _palette_rgba(palette: bytes, fmt: int) -> list[tuple[int, int, int, int]]:
    if fmt not in (0, 1, 2) or len(palette) % 2:
        raise ValueError("Palette must contain complete IA8(0), RGB565(1), or RGB5A3(2) entries")
    if fmt == 0:
        return [(palette[i + 1],) * 3 + (palette[i],) for i in range(0, len(palette), 2)]
    decode = _rgb565 if fmt == 1 else _rgb5a3
    return [decode((palette[i] << 8) | palette[i + 1]) for i in range(0, len(palette), 2)]


def _cmpr_block(data: bytes) -> list[tuple[int, int, int, int]]:
    c0, c1 = struct.unpack_from(">HH", data)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    if c0 > c1:
        p2 = tuple((a * 5 + b * 3) >> 3 for a, b in zip(p0[:3], p1[:3])) + (255,)
        p3 = tuple((a * 3 + b * 5) >> 3 for a, b in zip(p0[:3], p1[:3])) + (255,)
    else:
        avg = tuple((a + b) // 2 for a, b in zip(p0[:3], p1[:3]))
        p2, p3 = avg + (255,), avg + (0,)
    colors = (p0, p1, p2, p3)
    return [colors[(data[4 + y] >> (6 - 2 * x)) & 3] for y in range(4) for x in range(4)]


def _decode_scalar(width: int, height: int, fmt: int, data: bytes,
                   palette: bytes, palette_format: int) -> bytes:
    bw, bh, block_size = FORMATS[fmt]
    colors = _palette_rgba(palette, palette_format) if fmt in (8, 9, 10) else []
    out = bytearray(width * height * 4)
    pos = 0
    for by in range(0, height, bh):
        for bx in range(0, width, bw):
            block = data[pos:pos + block_size]
            pos += block_size
            sub = [_cmpr_block(block[i:i + 8]) for i in range(0, 32, 8)] if fmt == 14 else None
            for y in range(min(bh, height - by)):
                for x in range(min(bw, width - bx)):
                    i = y * bw + x
                    if fmt in (0, 8):
                        v = (block[i // 2] >> (4 if i % 2 == 0 else 0)) & 15
                        color = (v * 17,) * 4 if fmt == 0 else colors[v]
                    elif fmt == 1:
                        color = (block[i],) * 4
                    elif fmt == 2:
                        color = ((block[i] & 15) * 17,) * 3 + ((block[i] >> 4) * 17,)
                    elif fmt == 3:
                        color = (block[2 * i + 1],) * 3 + (block[2 * i],)
                    elif fmt in (4, 5, 10):
                        v = (block[2 * i] << 8) | block[2 * i + 1]
                        color = _rgb565(v) if fmt == 4 else _rgb5a3(v) if fmt == 5 else colors[v & 0x3fff]
                    elif fmt == 6:
                        color = (block[2*i+1], block[32+2*i], block[33+2*i], block[2*i])
                    elif fmt == 9:
                        color = colors[block[i]]
                    else:
                        color = sub[(y // 4) * 2 + x // 4][(y % 4) * 4 + x % 4]
                    dest = ((by + y) * width + bx + x) * 4
                    out[dest:dest + 4] = bytes(color)
    return bytes(out)


def _decode_numpy(width: int, height: int, fmt: int, data: bytes,
                  palette: bytes, palette_format: int) -> bytes:
    bw, bh, block_size = FORMATS[fmt]
    b = np.frombuffer(data, dtype=np.uint8).reshape(-1, block_size)
    count = len(b)
    if fmt == 14:
        # Four 4x4 sub-blocks per 8x8 macroblock, ordered TL, TR, BL, BR.
        c = b.reshape(-1, 8).astype(np.uint32)
        v0, v1 = c[:, 0] * 256 + c[:, 1], c[:, 2] * 256 + c[:, 3]
        def rgb(v):
            r, g, blue = (v >> 11) & 31, (v >> 5) & 63, v & 31
            return np.stack(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (blue << 3) | (blue >> 2)), -1)
        a, z = rgb(v0), rgb(v1)
        colors = np.full((len(c), 4, 4), 255, dtype=np.uint32)
        colors[:, 0, :3], colors[:, 1, :3] = a, z
        opaque = v0 > v1
        colors[:, 2, :3] = np.where(opaque[:, None], (a * 5 + z * 3) >> 3, (a + z) // 2)
        colors[:, 3, :3] = np.where(opaque[:, None], (a * 3 + z * 5) >> 3, (a + z) // 2)
        colors[:, 3, 3] = np.where(opaque, 255, 0)
        indices = ((c[:, 4:, None] >> np.array([6, 4, 2, 0], np.uint32)) & 3).reshape(-1, 16)
        rgba = colors[np.arange(len(c))[:, None], indices]
        rgba = rgba.reshape(count, 2, 2, 4, 4, 4).transpose(0, 1, 3, 2, 4, 5).reshape(count, 64, 4)
    else:
        rgba = np.empty((count, bw * bh, 4), dtype=np.uint8)
        if fmt in (0, 8):
            v = np.stack((b >> 4, b & 15), -1).reshape(count, -1)
        elif fmt in (3, 4, 5, 10):
            v = b[:, 0::2].astype(np.uint16) * 256 + b[:, 1::2]
        else:
            v = b
        if fmt in (0, 1):
            rgba[:] = (v * 17 if fmt == 0 else v)[:, :, None]
        elif fmt in (2, 3):
            rgba[:, :, :3] = ((v & 15) * 17 if fmt == 2 else v & 255)[:, :, None]
            rgba[:, :, 3] = (v >> 4) * 17 if fmt == 2 else v >> 8
        elif fmt in (4, 5):
            if fmt == 4:
                r, g, blue = (v >> 11) & 31, (v >> 5) & 63, v & 31
                rgba[:, :, 0] = (r << 3) | (r >> 2)
                rgba[:, :, 1] = (g << 2) | (g >> 4)
                rgba[:, :, 2] = (blue << 3) | (blue >> 2)
                rgba[:, :, 3] = 255
            else:
                opaque = (v & 0x8000) != 0
                for channel, shift in enumerate((10, 5, 0)):
                    small = (v >> shift) & 31
                    rgba[:, :, channel] = np.where(opaque, (small << 3) | (small >> 2), ((v >> (8 - 4 * channel)) & 15) * 17)
                alpha = (v >> 12) & 7
                rgba[:, :, 3] = np.where(opaque, 255, (alpha << 5) | (alpha << 2) | (alpha >> 1))
        elif fmt == 6:
            rgba[:, :, 0], rgba[:, :, 3] = b[:, 1:32:2], b[:, 0:32:2]
            rgba[:, :, 1], rgba[:, :, 2] = b[:, 32::2], b[:, 33::2]
        else:
            colors = np.array(_palette_rgba(palette, palette_format), dtype=np.uint8)
            indices = v & 0x3fff if fmt == 10 else v
            rgba[:] = colors[indices]
    tile_rows, tile_cols = (height + bh - 1) // bh, (width + bw - 1) // bw
    planar = rgba.reshape(tile_rows, tile_cols, bh, bw, 4).transpose(0, 2, 1, 3, 4)
    return planar.reshape(tile_rows * bh, tile_cols * bw, 4)[:height, :width].astype(np.uint8).tobytes()


def decode_rgba(width: int, height: int, fmt: int, data: bytes,
                palette: bytes = b"", palette_format: int = 0) -> bytes:
    """Decode the base level to top-to-bottom, tightly packed RGBA8 bytes.

    I4/I8 replicate intensity into alpha too, as GX does. IA8 bytes are A,I.
    CMPR retains RGB in alpha-zero texels and uses GX's 5:3 interpolation.
    """
    base = _base(width, height, fmt, data)
    if fmt in (8, 9, 10) and not palette:
        raise ValueError("Indexed texture requires a palette")
    try:
        decoder = _decode_numpy if np is not None else _decode_scalar
        return decoder(width, height, fmt, base, palette, palette_format)
    except IndexError as exc:
        raise ValueError("Texture indexes an absent palette entry") from exc
