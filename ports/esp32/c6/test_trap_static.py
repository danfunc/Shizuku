#!/usr/bin/env python3
"""Source ABI invariants; this does not claim execution on a C6."""
import re
from pathlib import Path
root = Path(__file__).absolute().parents[3]
layout = (root / 'ports/esp32/c6/frame_layout.h').read_text()
asm = (root / 'ports/esp32/c6/rv32_trap.S').read_text()
header = (root / 'internal_headers/shizuku/archs/rv32_c6.hpp').read_text()
offsets = dict((k, int(v)) for k, v in re.findall(r'#define C6_F_(\w+) (\d+)', layout))
assert sorted(offsets.values()) == list(range(0, 144, 4))
for name in offsets:
    reg = name.lower()
    assert f'offsetof(exception_frame_t, {reg}) == C6_F_{name}' in header
    if reg in {'mepc', 'mstatus', 'mcause', 'mtval', 'reserved0', 'reserved1'}:
        continue
    assert f'sw {reg}, C6_F_{name}(sp)' in asm
    assert f'lw {reg}, C6_F_{name}(sp)' in asm
for csr in ('mepc', 'mstatus', 'mcause', 'mtval'):
    assert f'csrr t0, {csr}\n  sw t0, C6_F_{csr.upper()}(sp)' in asm
assert 'csrw mepc, t0' in asm and 'csrw mstatus, t0' in asm and 'mret' in asm
assert '.balign 256' in asm and '.option norvc' in asm and '.rept 32' in asm
assert 'csrrw t0, mscratch, zero\n  beqz t0, 1f\n  mv sp, t0' in asm
assert 'sw t0, 0(sp)' in asm and 'lw t0, 0(sp)\n' in asm
assert asm.index('call shizuku_c6_trap_dispatch') < asm.index('csrw mscratch, t0')
assert 'mv sp, a0\n  j shizuku_c6_restore' in asm
# Entry marker must not destroy ring.magic before retention validation.
diag = (root / 'firmware/esp32c6/main/early_diag.c').read_text()
begin = diag.split('void shizuku_c6_early_diag_begin(void)')[1].split('void shizuku_c6_early_diag_writer_result')[0]
assert 'shizuku_c6_early_diag_log.magic = SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC' not in begin
assert begin.index('shizuku_c6_early_diag_progress = SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC') < begin.index('esp_rom_get_reset_reason')
print('PASS: frame offsets, full saves/restores, CSR capture, vector slots, mscratch nesting, ring retention source invariants')
