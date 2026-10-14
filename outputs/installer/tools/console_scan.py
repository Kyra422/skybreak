"""REL22-CERT console-data scan: refuses a SkyBreak package that contains data unique to one real Wii or one player.

legal_scan.py keeps game-derived files out; this scan keeps console identities and personal data out (until 11 Oct 2026
es_device.cpp embedded the Nintendo-signed device certificate of one real Wii, and every source zip carried it).

  C  a Nintendo-signed certificate: an IOS cert (signature type 0x0001000x, issuer "Root-CA00000001..." at the offset that
     type implies) whose signature is not all zero - as raw bytes in any file, or as a "0x.." byte array in a source file.
     The runtime's generated placeholder (zero signature, NG00000000) passes.
  K  console keys: keys.bin / otp.bin / seeprom.bin / nand.bin / keys.txt, or a BootMii "BackupMii" header
  Y  console system files: SYSCONF, setting.txt, uid.sys, cert.sys, nwc24msg.cfg, iplsave.bin, content.map, or the SYSCONF
     ("SCv0") / WC24 config ("WcCf") magic at the start of a file
  N  NAND save data: a file under a title/<8 hex>/<8 hex>/ or shared2/ folder, or wiiking2.sav / banner.bin (SS save)
  W  a MAC address with a Nintendo OUI written out in a text file (Wii, Wii Remote, Wi-Fi adapter)

Vendored open-source trees (_deps/) are scanned too. Exits 0 only when there are 0 hits.

  python console_scan.py <folder-or-zip> [...]
"""
import re
import sys
import zipfile
from pathlib import Path

ISSUER = b'Root-CA00000001'
# signature type -> (signature length, padding) of an IOS certificate / signed blob header (Dolphin IOS::ES SignatureType)
SIG_TYPES = {0x00010000: (0x200, 0x3C), 0x00010001: (0x100, 0x3C), 0x00010002: (0x3C, 0x40)}
KEY_NAMES = {'keys.bin', 'otp.bin', 'seeprom.bin', 'nand.bin', 'keys.txt'}
SYS_NAMES = {'sysconf', 'setting.txt', 'uid.sys', 'cert.sys', 'nwc24msg.cfg', 'iplsave.bin', 'content.map'}
SAVE_NAMES = {'wiiking2.sav', 'banner.bin'}
TITLE_DIR = re.compile(r'(^|/)title/[0-9a-f]{8}/[0-9a-f]{8}/', re.I)
TEXT_EXT = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.inl', '.inc', '.txt', '.py', '.toml', '.md', '.json',
            '.ps1', '.cmd', '.bat', '.sh', '.ini', '.cfg', '.csv', '.xml', '.yml', '.yaml', '.html', '.rc', '.cmake'}
HEX_BYTE = re.compile(rb'0x([0-9A-Fa-f]{2})\b')
MAC = re.compile(rb'(?<![0-9A-Fa-f:-])([0-9A-Fa-f]{2})([:-])([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2[0-9A-Fa-f]{2}\2[0-9A-Fa-f]{2}\2[0-9A-Fa-f]{2}(?![0-9A-Fa-f:-])')
# IEEE OUIs registered to Nintendo Co., Ltd. (Wii consoles, Wii Remotes, Wii LAN/Wi-Fi modules)
NINTENDO_OUI = {
    '0009BF', '001656', '0017AB', '00191D', '0019FD', '001AE9', '001B7A', '001BEA', '001CBE', '001DBC', '001E35', '001EA9',
    '001F32', '001FC5', '002147', '0021BD', '00224C', '0022AA', '0022D7', '002331', '0023CC', '00241E', '002444', '0024F3',
    '0025A0', '002659', '002709', '34AF2C', '40F407', '58BDA3', '8CCDE8', '9CE635', 'A45C27', 'A4C0E1', 'B8AE6E', 'CC9E00',
    'CCFB65', 'D86BF7', 'E00C7F', 'E0E751', 'E84ECE'}
MAX_BYTES = 512 * 1024 * 1024


def signed_certs(data):
    """(offset, name, sig_type) of every IOS certificate in data whose signature is not all zero."""
    out, i = [], data.find(ISSUER)
    while i >= 0:
        for st, (sig_len, pad) in SIG_TYPES.items():
            start = i - 4 - sig_len - pad
            if start >= 0 and int.from_bytes(data[start:start + 4], 'big') == st:
                sig = data[start + 4:start + 4 + sig_len]
                if any(sig):
                    name = data[i + 0x44:i + 0x84].split(b'\0', 1)[0].decode('ascii', 'replace')
                    out.append((start, name, st))
        i = data.find(ISSUER, i + 1)
    return out


def scan_bytes(rel, data, hits):
    rel = rel.replace('\\', '/')
    name = rel.rsplit('/', 1)[-1].lower()
    ext = ('.' + name.rsplit('.', 1)[-1]) if '.' in name else ''

    def hit(code, why):
        hits.append(f'{code} {rel}: {why}')

    for off, cname, st in signed_certs(data):
        hit('C', f'door Nintendo ondertekend certificaat {cname!r} (type {st:08x}) op 0x{off:x}')
    if ext in TEXT_EXT and len(data) < 64 * 1024 * 1024:
        hexbytes = bytes(int(m, 16) for m in HEX_BYTE.findall(data))
        if ISSUER in hexbytes:
            for off, cname, st in signed_certs(hexbytes):
                hit('C', f'door Nintendo ondertekend certificaat {cname!r} (type {st:08x}) als 0x..-bytetabel (byte {off})')
    if name in KEY_NAMES or b'BackupMii' in data[:0x100]:
        hit('K', 'consolesleutels (keys.bin/OTP/SEEPROM/NAND-dump)')
    if name in SYS_NAMES or data[:4] in (b'SCv0', b'WcCf'):
        hit('Y', 'systeembestand van een console (SYSCONF/setting.txt/uid.sys/WC24)')
    if TITLE_DIR.search(rel) or '/shared2/' in '/' + rel.lower() or name in SAVE_NAMES:
        hit('N', 'NAND-/savegegevens')
    if ext in TEXT_EXT:
        for m in MAC.finditer(data):
            oui = (m.group(1) + m.group(3) + m.group(4)).decode().upper()
            if oui in NINTENDO_OUI:
                hit('W', f'MAC-adres met Nintendo-OUI {m.group(0).decode()}')


def scan(target):
    """Scan a folder (every file) or a .zip (every member); returns (files, hits)."""
    target = Path(target)
    hits, n = [], 0
    if target.is_file() and zipfile.is_zipfile(target):
        with zipfile.ZipFile(target) as z:
            for info in z.infolist():
                if info.is_dir() or info.file_size > MAX_BYTES:
                    continue
                n += 1
                scan_bytes(info.filename, z.read(info), hits)
        return n, hits
    for p in sorted(target.rglob('*')):
        if p.is_file() and p.stat().st_size <= MAX_BYTES:
            n += 1
            scan_bytes(p.relative_to(target).as_posix(), p.read_bytes(), hits)
    return n, hits


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    total = 0
    for t in argv:
        n, hits = scan(t)
        total += len(hits)
        print(f'console scan of {t}: {n} files, {len(hits)} hits')
        for h in hits[:200]:
            print('  ' + h)
    print('RESULT ' + ('PASS (0 console-unique files)' if total == 0 else f'FAIL ({total} hits)'))
    return 0 if total == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
