"""REL22-CERT: mutated copy of hle/devices/es_device.cpp for the negative control of es-cert-test (C3 C4 C5 C7 must fail):
non-zero signature + public key bytes (a "signed" certificate), a retail-looking device id instead of the placeholder, and no
length check on the GetDeviceCert output vector."""
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[3]
src = root / 'outputs/nwiirecomp/nWiiRuntime/src/hle/devices/es_device.cpp'
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
MUTATIONS = [
    ('    std::array<uint8_t, DEVICE_CERT_SIZE> cert{};  // everything not written below stays zero',
     '    std::array<uint8_t, DEVICE_CERT_SIZE> cert{};  cert[0x004] = 0x5A; cert[0x108] = 0x01;  // MUTANT: signed + key'),
    ('constexpr uint32_t PLACEHOLDER_DEVICE_ID = 0x00000000;',
     'constexpr uint32_t PLACEHOLDER_DEVICE_ID = 0x04123456;  // MUTANT: retail-looking device id'),
    ('if (!has_vectors(req, 0, 1) || req.ioctlv_vecs[0].len < DEVICE_CERT_SIZE) return ES_EINVAL;',
     'if (!has_vectors(req, 0, 1)) return ES_EINVAL;  // MUTANT: no length check'),
]
t = src.read_text(encoding='utf-8')
for old, new in MUTATIONS:
    if t.count(old) != 1:
        sys.exit(f'mutation target not found in {src.name}: {old!r}')
    t = t.replace(old, new)
(out / src.name).write_text(t, encoding='utf-8')
print(f'wrote mutant to {out}')
