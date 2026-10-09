#!/usr/bin/env python3
"""Source ABI invariants; this does not claim execution on a C6."""
import re
from pathlib import Path
root = Path(__file__).absolute().parents[2]
layout = (root / 'chips/esp32_c6/frame_layout.h').read_text()
asm = (root / 'chips/esp32_c6/rv32_trap.S').read_text()
header = (root / 'chips/esp32_c6/rv32_c6.hpp').read_text()
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
diag = (root / 'modules/esp_idf_support/firmware/main/early_diag.c').read_text()
begin = diag.split('void shizuku_c6_early_diag_begin(void)')[1].split('void shizuku_c6_early_diag_writer_result')[0]
assert 'shizuku_c6_early_diag_log.magic = SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC' not in begin
assert begin.index('shizuku_c6_early_diag_progress = SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC') < begin.index('esp_rom_get_reset_reason')

trap_src = (root / 'chips/esp32_c6/trap.cpp').read_text()
board_src = (root / 'boards/esp32_c6/board_esp32_c6.cpp').read_text()
api_src = (root / 'chips/esp32_c6/trap_api.hpp').read_text()
assert 'void esp32_c6::init(uint32_t core)' in board_src
assert 'install_traps();' in board_src and 'interrupt_init();' in board_src
assert 'void esp32_c6::init' not in trap_src  # board 実装は chips に残さない
assert 'void install_traps();' in api_src and '[[noreturn]] void stop(uint32_t status);' in api_src
assert 'kernel.timer_expired()' in trap_src
assert 'run_pending_switch()' in trap_src
assert 'reg_write(c6::SYSTIMER_INT_CLR, c6::TARGET0_INT_BIT)' in trap_src
assert 'shizuku_c6_timer_diag' in trap_src

boot_src = (root / 'modules/esp_idf_support/c6/early_boot.cpp').read_text()
assert 'shizuku_c6_thread1_diag' in boot_src
assert 'records[2]' in boot_src
assert 'thread1' in boot_src

print('PASS: frame offsets, full saves/restores, CSR capture, vector slots, mscratch nesting, ring retention, timer/thread1 invariants')
