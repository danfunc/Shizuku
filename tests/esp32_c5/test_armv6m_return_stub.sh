#!/bin/sh
set -eu

python3 - "${TEST_SRCDIR}/${TEST_WORKSPACE}/arch/armv6m/armv6m_ctx.S" \
          "${TEST_SRCDIR}/${TEST_WORKSPACE}/arch/armv8m/armv8m_ctx.S" <<'PY'
import sys
from pathlib import Path

expected = [
    "movs r3, #0xA5",
    "lsls r3, r3, #8",
    "adds r3, #0x5A",
    "mov r12, r3",
    "movs r3, #0",
    "movs r0, #2",
    "svc 0",
]

for source in map(Path, sys.argv[1:]):
    assembly = source.read_text()
    label = "shizuku_" + source.stem.replace("_ctx", "") + "_return_stub:"
    body = assembly.split(label, 1)[1].split("\n", 1)[1]
    instructions = []
    for line in body.splitlines():
        line = line.split("//", 1)[0].strip()
        if line and not line.startswith((".", "1:")):
            instruction = " ".join(line.split())
            instructions.append(instruction)
            if instruction == "svc 0":
                break
    assert instructions[:2] == ["mov r2, r0", "movs r1, #1"], (
        f"{source}: return value/count registers changed: {instructions}"
    )
    assert any(instructions[i:i + len(expected)] == expected
               for i in range(len(instructions) - len(expected) + 1)), (
        f"{source}: marker must be copied to r12 and r3 cleared before SVC; "
        f"got {instructions}"
    )
PY
