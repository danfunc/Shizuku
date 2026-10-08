#!/usr/bin/env python3
"""Offline RAM-code identity check. Never opens a device or runs a debugger."""
import argparse
import base64
import hashlib
import json
import re
import struct
from pathlib import Path


def elf_segments(path):
    data = Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x01\x01':
        raise ValueError('expected little-endian ELF32')
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    if header[2] != 243:
        raise ValueError('expected RISC-V ELF')
    segments = []
    for i in range(header[10]):
        kind, offset, addr, _, size, _, _, _ = struct.unpack_from('<IIIIIIII', data, header[5] + i * header[9])
        if kind == 1 and size:
            chunk = data[offset:offset + size]
            if len(chunk) != size:
                raise ValueError('truncated ELF segment')
            segments.append((addr, chunk))
    return segments


def stub_segments(path):
    stub = json.loads(Path(path).read_text())
    return [(stub[k + '_start'], base64.b64decode(stub[k])) for k in ('text', 'data') if k in stub]


def words_at(segments, addr, count=8):
    for start, data in segments:
        offset = addr - start
        if offset >= 0 and offset + count * 4 <= len(data):
            return list(struct.unpack_from('<' + 'I' * count, data, offset))
    return None


def read_dump(text):
    words = {}
    for line in text.splitlines():
        match = re.match(r'^\s*(?:0x)?([0-9a-fA-F]{8}):\s*((?:[0-9a-fA-F]{8}(?:\s+|$))+)', line)
        if not match:
            continue
        addr = int(match[1], 16)
        for i, word in enumerate(match[2].split()):
            key, value = addr + i * 4, int(word, 16)
            if key in words and words[key] != value:
                raise ValueError('conflicting snapshots; compare one invocation at a time')
            words[key] = value
    return words


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', required=True)
    parser.add_argument('--expected-sha256', required=True)
    parser.add_argument('--bootloader')
    parser.add_argument('--stub-json', action='append', default=[])
    parser.add_argument('--address', action='append', type=lambda s: int(s, 0))
    parser.add_argument('--dump', help='one OpenOCD mdw stdout/stderr snapshot')
    args = parser.parse_args()
    digest = hashlib.sha256(Path(args.elf).read_bytes()).hexdigest()
    if not re.fullmatch('[0-9a-f]{64}', args.expected_sha256) or digest != args.expected_sha256:
        raise ValueError('full flashed ELF SHA256 mismatch')
    candidates = [('app', args.elf, elf_segments(args.elf))]
    if args.bootloader:
        candidates.append(('bootloader', args.bootloader, elf_segments(args.bootloader)))
    for i, path in enumerate(args.stub_json):
        candidates.append((f'stub-{i}', path, stub_segments(path)))
    addresses = args.address or [0x40800000, 0x408003f0]
    if any(addr & 3 for addr in addresses):
        raise ValueError('mdw fingerprint addresses must be word aligned')
    for name, path, segments in candidates:
        print(name, 'file=', path, 'sha256=', hashlib.sha256(Path(path).read_bytes()).hexdigest())
        for addr in addresses:
            expected = words_at(segments, addr)
            print(f'  0x{addr:08x}:', ' '.join(f'{word:08x}' for word in expected) if expected else 'not file-backed by this image')
    for addr in addresses:
        print(f'checked code-{addr:08x} {{echo [mdw 0x{addr:08x} 8]}}')
    if args.dump:
        observed = read_dump(Path(args.dump).read_text())
        needed = [addr + i * 4 for addr in addresses for i in range(8)]
        if any(addr not in observed for addr in needed):
            raise ValueError('incomplete code dump; identity unknown')
        matches = []
        for name, _, segments in candidates:
            expected = [words_at(segments, addr) for addr in addresses]
            if all(window is not None for window in expected) and all(
                [observed[addr + i * 4] for i in range(8)] == window
                for addr, window in zip(addresses, expected)
            ):
                matches.append(name)
        print('CODE_IDENTITY=', ','.join(matches) if matches else 'unknown/mixed/unlisted')
        return 0 if len(matches) == 1 else 1
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, OSError, struct.error) as exc:
        print('ERROR:', exc)
        raise SystemExit(2)
