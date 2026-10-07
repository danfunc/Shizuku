#!/usr/bin/env python3
"""Verify each archive member occurrence contributes a strong symbol to map and ELF."""
import subprocess
import sys
import tempfile
from pathlib import Path


def run(args, *, cwd=None):
    return subprocess.run(args, cwd=cwd, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def strong_symbols(nm, obj):
    symbols = set()
    # Exclude COMMON (mergeable) and absolute symbols; lowercase/weak and W/V are not strong definitions.
    for args in (["-g", "--defined-only"], ["-C", "-g", "--defined-only"]):
        for line in run([nm, *args, str(obj)]).splitlines():
            fields = line.split(None, 2)
            if len(fields) != 3:
                continue
            _address, kind, name = fields
            if len(kind) == 1 and kind in "BDGIRST" and name:
                symbols.add(name)
    return symbols


def check_archive_members(archive, elf, map_file, ar, nm):
    all_members = run([ar, "t", str(archive)]).splitlines()
    members = [name for name in all_members if name not in {"/", "//", "__.SYMDEF", "__.SYMDEF SORTED", "__.GOSYMDEF"}]
    elf_symbols = strong_symbols(nm, elf)
    map_text = Path(map_file).read_text(errors="replace")
    missing = []
    occurrences = {}
    with tempfile.TemporaryDirectory(prefix="shizuku-archive-check-") as scratch:
        for index, member in enumerate(members, 1):
            occurrences[member] = occurrences.get(member, 0) + 1
            member_dir = Path(scratch) / str(index)
            member_dir.mkdir()
            run([ar, "xN", str(occurrences[member]), str(Path(archive).resolve()), member], cwd=member_dir)
            obj = member_dir / member
            if not obj.is_file():
                missing.append(f"{index}:{member}:extract-failed")
                continue
            candidates = strong_symbols(nm, obj)
            matched = next((sym for sym in sorted(candidates) if sym in elf_symbols and sym in map_text), None)
            if matched is None:
                missing.append(f"{index}:{member}:no-strong-symbol-in-map-and-ELF")
    return members, missing


def main():
    if len(sys.argv) != 6:
        print("usage: check_archive_members.py ARCHIVE ELF MAP AR NM", file=sys.stderr)
        return 2
    archive, elf, map_file, ar, nm = sys.argv[1:]
    try:
        members, missing = check_archive_members(archive, elf, map_file, ar, nm)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"C4 ERROR: archive inspection failed ({exc})", file=sys.stderr)
        return 2
    if missing:
        print("C4 FAIL: " + ", ".join(missing))
        return 1
    print(f"C4 PASS: identified {len(members)} archive member occurrences by strong symbols in map and ELF")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
