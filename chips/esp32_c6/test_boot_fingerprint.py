#!/usr/bin/env python3
import importlib.util
import struct
from pathlib import Path

path = Path(__file__).absolute().parents[2] / 'tools/esp32/boot_fingerprint.py'
spec = importlib.util.spec_from_file_location('boot_fingerprint', path)
f = importlib.util.module_from_spec(spec)
spec.loader.exec_module(f)
# Two different loaded programs can share a PC. Compare bytes, not addr2line.
app = [(0x40800000, struct.pack('<8I', *range(8)))]
stub = [(0x40800000, struct.pack('<8I', *range(8, 16)))]
assert f.words_at(app, 0x40800000) == list(range(8))
assert f.words_at(stub, 0x40800000) != f.words_at(app, 0x40800000)
assert f.words_at(app, 0x407ffffc) is None
assert f.words_at(app, 0x40800004) is None  # partial window is not an identity
parsed = f.read_dump('0x40800000: 00000000 00000001 00000002 00000003\n40800010: 00000004 00000005 00000006 00000007\n')
assert [parsed[0x40800000 + i * 4] for i in range(8)] == list(range(8))
try:
    f.read_dump('40800000: 00000000\n40800000: 00000001\n')
    assert False, 'conflicting snapshots must fail'
except ValueError:
    pass
print('PASS: shared-PC distinction, unavailable/partial windows, mdw parsing, conflicting snapshots')
