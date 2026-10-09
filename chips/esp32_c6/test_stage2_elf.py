#!/usr/bin/env python3
"""Fresh final ELF checks, to be run by the upper layer after Bazel/IDF build."""
import subprocess
import sys
from pathlib import Path
elf, tc = sys.argv[1:]
def run(tool, *args):
    return subprocess.check_output([str(Path(tc) / ('riscv32-esp-elf-' + tool)), *args, elf], text=True)
symbols = {}
for line in run('nm', '-n').splitlines():
    parts = line.split()
    if len(parts) == 3:
        symbols[parts[2]] = int(parts[0], 16)
for name, size in [('shizuku_c6_early_diag_log', 376), ('shizuku_c6_early_diag_progress', 4)]:
    addr = symbols[name]
    assert symbols['_noinit_start'] <= addr and addr + size <= symbols['_noinit_end'], name
    assert addr + size <= symbols['_bss_start'] or addr >= symbols['_bss_end'], name
vector = symbols['shizuku_c6_vectors']
assert vector % 256 == 0
vector_disasm = run('objdump', '-d', '--start-address=' + hex(vector), '--stop-address=' + hex(vector + 128))
assert vector_disasm.count('<shizuku_c6_trap_entry>') == 32, vector_disasm
trap = run('objdump', '-d', '--disassemble=shizuku_c6_trap_entry')
assert 'mret' in trap and 'mscratch' in trap and 'mepc' in trap and 'mstatus' in trap
assert '<shizuku_c6_trap_dispatch>' in trap
for name in ['shizuku_c6_thread0_diag', 'shizuku_c6_thread1_diag', 'shizuku_c6_timer_diag', 'shizuku_c6_switch_diag', 'shizuku_c6_trap_diag', 'shizuku_c6_irq_depth', 'shizuku_c6_trap_depth']:
    assert name in symbols
print('PASS: final vector alignment/32 entries, CSR restoration/dispatch, noinit outside BSS, JTAG symbols')
