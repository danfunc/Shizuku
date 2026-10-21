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
assert 'csrrw sp, mscratch, sp' in asm
# t2 破壊防止 (重大指摘 1): t2 退避前に li/addi などで t2 を上書きしてはならない
# C6 専用実験 flag (重大指摘 A)
assert '#if defined(SHIZUKU_EXP_C6_STAGE1)' in asm
assert '#define SHIZUKU_EXP_C6_STAGE1 1' in vectors
c5_vectors = (root / 'chips/esp32_c5/c5_vectors.S').read_text()
assert 'SHIZUKU_EXP_C6_STAGE1' not in c5_vectors
# 非 C6 (既定挙動) が共有 rv32_trap.S の #else 側に保持されていること (重大指摘 A)
else_block = asm.split('#else')[1]
assert 'addi sp, sp, -RV32_FRAME_BYTES' in else_block
assert 'csrrw t0, mscratch, zero' in else_block
assert 'addi sp, sp, RV32_FRAME_BYTES' in else_block
# C5 パスのアセンブリに C6 stage1 固有命令 (csrrw sp, mscratch, sp) が含まれないことを検証
assert 'csrrw sp, mscratch, sp' not in else_block

entry_code = asm.split('shizuku_rv32_restore:')[0]
assert 'li t2' not in entry_code and 'addi t2' not in entry_code
assert entry_code.count('sw t2, RV32_F_T2(sp)') == 2
assert 'lw t0, RV32_F_RESERVED1(a0)' in entry_code
assert 'la t0, shizuku_c6_dispatch_stack' in entry_code
assert 'sub t1, a0, t0' in entry_code
assert 'bltu t1, t3, 1f' in entry_code
assert 'la sp, shizuku_c6_dispatch_stack + 4096' in entry_code
assert (entry_code.index('la t0, shizuku_c6_dispatch_stack') <
        entry_code.index('bltu t1, t3, 1f') <
        entry_code.index('la sp, shizuku_c6_dispatch_stack + 4096') <
        entry_code.index('call shizuku_rv32_trap_dispatch'))
assert 'lw sp, RV32_F_RESERVED0(sp)' in asm
# C6 Stage 1: normalize_frame sets active SP (reserved0) to snapshot = &frame + sizeof(exception_frame_t)
assert 'frame.reserved0 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&frame) + sizeof(exception_frame_t));' in header
# 重大ブロッカー G: trusted exception frame retention
# M-mode trap はスレッドスタック上にフレームを作り RESERVED1=0 を記録、
# U-mode trap は trusted stack cursor からフレームを確保し RESERVED1=cursor を記録。
assert 'sw zero, RV32_F_RESERVED1(sp)' in entry_code
assert 'addi t0, sp, RV32_FRAME_BYTES\n  sw t0, RV32_F_RESERVED1(sp)' in entry_code
# 復帰時は復帰対象フレーム自身の RESERVED1 から mscratch を復元 (dispatch 直後の固定復元を廃止)
restore_code = asm.split('shizuku_rv32_restore:')[1].split('.size')[0]
assert 'lw t0, RV32_F_RESERVED1(sp)\n  csrw mscratch, t0' in restore_code
assert 'lw t0, RV32_F_T0(sp)\n  lw sp, RV32_F_RESERVED0(sp)\n  mret' in restore_code

# ABI 整合性検査:
# 1. mscratch 復元が最終 T0 復元より前にあること
# 2. 最終 T0 復元が SP 復元および mret より前にあること
# 3. 最終 T0 復元と mret の間に t0 を書き換える命令が存在しないこと
idx_mscratch = restore_code.index('csrw mscratch, t0')
idx_t0 = restore_code.index('lw t0, RV32_F_T0(sp)')
idx_sp = restore_code.index('lw sp, RV32_F_RESERVED0(sp)')
idx_mret = restore_code.index('mret')
assert idx_mscratch < idx_t0 < idx_sp < idx_mret
between_t0_mret = restore_code[idx_t0 + len('lw t0, RV32_F_T0(sp)'):idx_mret]
assert 't0' not in between_t0_mret

# C5 / 汎用 #else 復帰経路の不変条件検査:
else_restore = else_block.split('shizuku_rv32_restore:')[1].split('.size')[0]
assert 'lw t0, RV32_F_T0(sp)' in else_restore
assert 'addi sp, sp, RV32_FRAME_BYTES\n  mret' in else_restore
assert 'RV32_F_RESERVED1' not in else_restore
assert 'RV32_F_RESERVED0' not in else_restore
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
assert 'shizuku_c6_dispatch_stack' in trap_src
assert 'shizuku_c6_is_dispatch_stack_address' in trap_src
pmp_src = (root / 'chips/esp32_c6/pmp_c6.hpp').read_text()
assert 'registered_owner != 0 &&' in pmp_src
assert 'current_obj == registered_owner' in pmp_src
assert 'shizuku_c6_is_authorized_u_object(g_c6_stage1_u_bounds.owner_id, cur_obj)' in trap_src
assert 'g_c6_probe_caller_stack_fault_pc' in trap_src
assert 'g_c6_probe_combo_0x802_pc' in trap_src
assert 'g_c6_probe_combo_pmp_pc' in trap_src
assert 'f.reserved1 = 0' in trap_src
assert 'f.reserved1 = kstack_top' in trap_src
assert 'f.ra == arch::return_stub()' in trap_src and 'f.ra = trampoline' in trap_src
safety_src = (root / 'source/selftest/c6_stage1_safety.cpp').read_text()
assert 'multi-trap combo' in safety_src
assert 'caller SP exactly retained across multi-trap invocation' in safety_src

# Regression assertions for Stage1 probe 0x61 assembly contracts:
# 1. .Lprobe_ecall uses GET_CURRENT_OBJECT=5
ecall_probe = safety_src.split('.Lprobe_ecall:')[1].split('.Lprobe_unpriv:')[0]
assert 'li a0, 5' in ecall_probe
assert 'ecall' in ecall_probe

# 2. combo region uses GET_CURRENT_OBJECT=5, checks a0 error, and combines a1
combo_probe = safety_src.split('.Lprobe_multi_trap_combo:')[1].split('.size u_mode_probe_worker')[0]
assert 'li a0, 5' in combo_probe
assert 'ecall' in combo_probe
assert 'bnez a0' in combo_probe
assert 'or a0, a1, t0' in combo_probe

# 3. return trampoline still uses EXIT_METHOD=2
trampoline = safety_src.split('c6_stage1_u_return_trampoline:')[1].split('.size c6_stage1_u_return_trampoline')[0]
assert 'li a0, 2' in trampoline
assert 'li a4, 0xA55A' in trampoline
assert 'ecall' in trampoline

# 4. Lane A r6 safety contracts:
# - named slot generated object ID
assert 'OBJECT_STAGE1_PROBE = object_id::c6_stage1_probe;' in safety_src
# - single-argument CALL_METHOD ABI preserved with data pool pointer sourcing
assert 'la t1, u_mode_stage1_data_pool' in safety_src
# - precondition gate: dependent tests aborted if create_object fails
assert 'if (!create_ok) {' in safety_src
# - exact fault assertions: mcause, mepc, mtval
assert 'stage1: PMP fault exact mcause is 5 (Load Access Fault)' in safety_src
assert 'stage1: caller stack fault exact mcause is 5 (Load Access Fault)' in safety_src
assert 'stage1: multi-trap combo incremented PMP fault counter by 1' in safety_src
assert 'stage1: multi-trap combo exact fault mcause is 5' in safety_src
assert 'stage1: multi-trap combo exact fault mepc is combo probe insn' in safety_src
assert 'stage1: multi-trap combo exact fault mtval is kstack address' in safety_src
# - aligned stack snapshot & separate unaligned rejection
assert 'kstack_valid_frame = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack) + 3600u;' in safety_src
assert 'stage1: M handler stack rejects unaligned frame (fail-closed 0)' in safety_src
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
