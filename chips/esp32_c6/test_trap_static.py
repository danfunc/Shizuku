#!/usr/bin/env python3
"""Source ABI invariants; this does not claim execution on a C6."""
import re
from pathlib import Path
root = Path(__file__).absolute().parents[2]
layout = (root / 'arch/riscv/rv32_frame.h').read_text()
# Generic RV32 save/restore/CSR/mret/first-return/return stub (arch side).
asm = (root / 'arch/riscv/rv32_trap.S').read_text()
# C6-only 256B vector table with 32 slots that enter the generic entry (chip side).
vectors = (root / 'chips/esp32_c6/c6_vectors.S').read_text()
header = (root / 'arch/riscv/rv32.hpp').read_text()
offsets = dict((k, int(v)) for k, v in re.findall(r'#define RV32_F_(\w+) (\d+)', layout))
assert sorted(offsets.values()) == list(range(0, 144, 4))
assert '#define RV32_FRAME_BYTES 144' in layout
assert '#define RV32_MSTATUS_MPP 0x1800' in layout and '#define RV32_MSTATUS_MPIE 0x80' in layout
for name in offsets:
    reg = name.lower()
    assert f'offsetof(exception_frame_t, {reg}) == RV32_F_{name}' in header
    if reg in {'mepc', 'mstatus', 'mcause', 'mtval', 'reserved0', 'reserved1'}:
        continue
    assert f'sw {reg}, RV32_F_{name}(sp)' in asm
    assert f'lw {reg}, RV32_F_{name}(sp)' in asm
for csr in ('mepc', 'mstatus', 'mcause', 'mtval'):
    assert f'csrr t0, {csr}\n  sw t0, RV32_F_{csr.upper()}(sp)' in asm
assert 'csrw mepc, t0' in asm and 'csrw mstatus, t0' in asm and 'mret' in asm
# Vector table: 256B aligned, norvc, 32 slots, every slot enters the generic entry.
assert '.balign 256' in vectors and '.option norvc' in vectors and '.rept 32' in vectors
assert '  j shizuku_rv32_trap_entry\n.endr' in vectors
assert 'shizuku_rv32_trap_entry:' in asm and '.option norvc' in asm
assert 'csrrw t0, mscratch, zero\n  beqz t0, 1f\n  mv sp, t0' in asm
assert 'sw t0, 0(sp)' in asm and 'lw t0, 0(sp)\n' in asm
assert asm.index('call shizuku_rv32_trap_dispatch') < asm.index('csrw mscratch, t0')
assert 'mv sp, a0\n  j shizuku_rv32_restore' in asm
assert 'shizuku_rv32_restore:' in asm and 'call shizuku_rv32_return_failed' in asm
assert 'shizuku_rv32_return_stub:' in asm and 'shizuku_rv32_first_return:' in asm
assert 'mv a2, a0' in asm and 'ecall' in asm
# Entry marker must not destroy ring.magic before retention validation.
diag = (root / 'modules/esp_idf_support/firmware/main/early_diag.c').read_text()
begin = diag.split('void shizuku_c6_early_diag_begin(void)')[1].split('void shizuku_c6_early_diag_writer_result')[0]
assert 'shizuku_c6_early_diag_log.magic = SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC' not in begin
assert begin.index('shizuku_c6_early_diag_progress = SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC') < begin.index('esp_rom_get_reset_reason')

trap_src = (root / 'chips/esp32_c6/trap.cpp').read_text()
assert '__attribute__((weak))' not in trap_src
assert 'shizuku_esp_ble_' not in trap_src
assert 'shizuku_freertos_' not in trap_src
assert 'SHIZUKU_IRQ_DELAY_US' not in trap_src
assert 'shizuku_rv32_trap_dispatch' in trap_src and 'shizuku_rv32_return_failed' in trap_src
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
assert 'c6_idf_ble_freertos_policy' in boot_src
assert 'set_irq_dispatch_hooks' in boot_src
assert 'set_irq_dispatch_hooks' in api_src
assert 'make_irq_dispatch_hooks' in api_src

print('PASS: frame offsets, full saves/restores, CSR capture, vector slots, mscratch nesting, ring retention, timer/thread1 invariants')
