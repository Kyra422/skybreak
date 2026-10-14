"""REL (OSModule v1-3) + U8 archive helpers for the REL AOT generator (RELAOT, M2.7).

Read-only: parses original .rel bytes; never touches game memory.
"""
import struct, hashlib
from pathlib import Path

R_NAMES = {0: 'NONE', 1: 'ADDR32', 2: 'ADDR24', 3: 'ADDR16', 4: 'ADDR16_LO', 5: 'ADDR16_HI',
           6: 'ADDR16_HA', 7: 'ADDR14', 8: 'ADDR14_BRTAKEN', 9: 'ADDR14_BRNTAKEN', 10: 'REL24',
           11: 'REL14', 12: 'REL14_BRTAKEN', 13: 'REL14_BRNTAKEN', 201: 'DOLPHIN_NOP',
           202: 'DOLPHIN_SECTION', 203: 'DOLPHIN_END', 204: 'DOLPHIN_MRKREF'}


def u8_extract(arc_bytes):
    """Return {path: bytes} for every file in a U8 archive."""
    b = arc_bytes
    magic, root_off, hdr_size, data_off = struct.unpack_from('>IIII', b, 0)
    assert magic == 0x55AA382D, hex(magic)
    def node(i):
        o = root_off + 12 * i
        w0, doff, size = struct.unpack_from('>III', b, o)
        return w0 >> 24, w0 & 0xFFFFFF, doff, size
    n_nodes = node(0)[3]
    str_base = root_off + 12 * n_nodes
    def name(off):
        e = b.index(b'\0', str_base + off)
        return b[str_base + off:e].decode('ascii')
    out = {}
    stack = [(n_nodes, '')]  # (end index, prefix)
    i = 1
    while i < n_nodes:
        while stack and i >= stack[-1][0]:
            stack.pop()
        typ, noff, doff, size = node(i)
        nm = name(noff)
        prefix = stack[-1][1] if stack else ''
        if typ == 1:
            stack.append((size, prefix + nm + '/'))
        else:
            out[prefix + nm] = bytes(b[doff:doff + size])
        i += 1
    return out


class Rel:
    def __init__(self, data, name=''):
        self.data = data
        self.name = name
        (self.id, _n, _p, self.num_sections, self.sec_info_off, self.name_off, self.name_size,
         self.version, self.bss_size, self.rel_off, self.imp_off, self.imp_size) = struct.unpack_from('>12I', data, 0)
        (self.prolog_sec, self.epilog_sec, self.unres_sec, self.bss_sec) = struct.unpack_from('>4B', data, 0x30)
        self.prolog, self.epilog, self.unresolved = struct.unpack_from('>3I', data, 0x34)
        self.align = self.bss_align = self.fix_size = 0
        if self.version >= 2:
            self.align, self.bss_align = struct.unpack_from('>II', data, 0x40)
        if self.version >= 3:
            self.fix_size, = struct.unpack_from('>I', data, 0x48)
        self.sections = []  # (file_offset, size, exec)
        for s in range(self.num_sections):
            off, size = struct.unpack_from('>II', data, self.sec_info_off + 8 * s)
            self.sections.append((off & ~1, size, bool(off & 1)))
        self.imports = []  # (module_id, offset)
        for k in range(self.imp_size // 8):
            self.imports.append(struct.unpack_from('>II', data, self.imp_off + 8 * k))
        # relocations: list of (target_module, section, sec_offset, type, target_section, addend)
        self.relocs = []
        for mod, off in self.imports:
            sec = None; pos = 0; o = off
            while True:
                d_off, typ, tsec, addend = struct.unpack_from('>HBBI', data, o); o += 8
                if typ == 203:
                    break
                if typ == 202:
                    sec = tsec; pos = 0; continue
                pos += d_off
                if typ in (0, 201):
                    continue
                self.relocs.append((mod, sec, pos, typ, tsec, addend))

    def sec_bytes(self, s):
        off, size, _ = self.sections[s]
        return self.data[off:off + size] if off else b''

    def text_sections(self):
        return [s for s, (off, size, ex) in enumerate(self.sections) if ex and off and size]

    def sha1(self):
        return hashlib.sha1(self.data).hexdigest()


def load_symbols(path):
    """decomp symbols.txt -> list of (name, section, addr, size, type)"""
    out = []
    for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        line = line.strip()
        if not line or '=' not in line:
            continue
        nm, rest = line.split('=', 1)
        nm = nm.strip(); rest = rest.strip()
        loc, _, attrs = rest.partition(';')
        sec, _, addr = loc.strip().partition(':')
        try:
            a = int(addr, 16)
        except ValueError:
            continue
        typ = size = None
        for tok in attrs.split():
            if tok.startswith('type:'): typ = tok[5:]
            if tok.startswith('size:'): size = int(tok[5:], 16)
        out.append((nm, sec, a, size or 0, typ))
    return out
