#!/usr/bin/env python3
"""Derive an MPU-aligned copy-to-RAM script from the pinned pico-sdk script.

Fail closed if its structure changes. Startup, load addresses, U-cost sections,
heap and stack layout remain owned by the SDK. No textual bulk substitution.
"""
import pathlib
import sys


def render(source):
    lines = source.splitlines(keepends=True)
    anchors = {
        "__ram_text_end__ = .;": "        . = ALIGN(32);\n",
        "*(.rodata*)": "        . = ALIGN(32);\n        __shizuku_rodata_start__ = .;\n",
    }
    for anchor in (*anchors, "*(.srodata*)", ".text : {", "__ram_text_start__ = .;"):
        if sum(line.strip() == anchor for line in lines) != 1:
            raise ValueError("SDK linker structure changed: " + anchor)
    if "*(.time_critical*)" not in source or "> RAM AT> FLASH" not in source:
        raise ValueError("not the expected SDK copy_to_ram linker script")
    output = []
    for line in lines:
        key = line.strip()
        if key == ".text : {":
            # Align the output section itself: LOADADDR(.text) must still refer
            # to exactly the byte copied into __ram_text_start__ by SDK CRT0.
            output.append("    .text : ALIGN(32) {\n")
            continue
        if key in anchors:
            output.append(anchors[key])
        output.append(line)
        if key == "*(.srodata*)":
            output.append("        . = ALIGN(32);\n        __shizuku_rodata_end__ = .;\n")
    return "".join(output)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: make_rp_linker.py <sdk-copy-to-ram.ld> <output.ld>")
    try:
        result = render(pathlib.Path(sys.argv[1]).read_text())
    except (OSError, ValueError) as error:
        sys.exit(str(error))
    pathlib.Path(sys.argv[2]).write_text(result)
