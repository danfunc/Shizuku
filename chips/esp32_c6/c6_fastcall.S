#include "arch/riscv/rv32_frame.h"
#include "chips/esp32_c6/fastcall.h"

// ===========================================================================
//  ESP32-C6 Fastcall Dedicated IRAM Asm Path (Stage 1 Prototype)
// ===========================================================================
//  Hotpath execution in .iram1, bypassing normal C trap_dispatch and svc_dispatch.
//  Two traps per round-trip:
//    Trap 1: Caller -> Kernel -> Callee
//    Trap 2: Callee -> Return Stub -> Kernel -> Caller
// ===========================================================================

.section .data,"aw",@progbits
.balign 16
.global shizuku_c6_fastcall_descriptor
shizuku_c6_fastcall_descriptor:
  .word shizuku_c6_fastcall_leaf_callee // 0: entry_pc (fixed, immutable 1-method baseline)
  .word 0                               // 4: caller_sp
  .word 0                               // 8: caller_pc
  .word 0                               // 12: depth (0: idle, 1: active)
  .word 0                               // 16: call_count

.balign 4
.global shizuku_c6_fastcall_s_nonzero
shizuku_c6_fastcall_s_nonzero:
  .word 0

.balign 4
.global shizuku_c6_fastcall_gp_tp_nonzero
shizuku_c6_fastcall_gp_tp_nonzero:
  .word 0

.balign 4
.global shizuku_c6_fastcall_nonarg_nonzero
shizuku_c6_fastcall_nonarg_nonzero:
  .word 0

.global shizuku_c6_fastcall_probe_return
shizuku_c6_fastcall_probe_return: .word 0
.global shizuku_c6_fastcall_probe_scratch
shizuku_c6_fastcall_probe_scratch: .word 0
.global shizuku_c6_fastcall_probe_gp_before
shizuku_c6_fastcall_probe_gp_before: .word 0
.global shizuku_c6_fastcall_probe_gp_after
shizuku_c6_fastcall_probe_gp_after: .word 0
.global shizuku_c6_fastcall_probe_tp_before
shizuku_c6_fastcall_probe_tp_before: .word 0
.global shizuku_c6_fastcall_probe_tp_after
shizuku_c6_fastcall_probe_tp_after: .word 0
.global shizuku_c6_fastcall_probe_s_after
shizuku_c6_fastcall_probe_s_after: .space 48

.section .iram1,"ax",@progbits
.option push
.option norvc
.option norelax

// ---------------------------------------------------------------------------
// Leaf callee (single fixed 1-method callee for both selftest and bench)
// ---------------------------------------------------------------------------
.global shizuku_c6_fastcall_leaf_callee
.type shizuku_c6_fastcall_leaf_callee,@function
shizuku_c6_fastcall_leaf_callee:
#if SHIZUKU_C6_FASTCALL_EMPTY_BENCH
  // Minimal method payload for a dedicated bench-only image. Entry remains fixed.
  addi a0, a0, 1
  ret
#else
  // Callee entry check: save argument a0 to caller stub's empty frame slot 60(sp).
  // No non-argument registers are overwritten prior to reading their entry values.
  sw a0, 60(sp)

  // Accumulate bitwise OR of all non-argument registers using a0 as accumulator.
  // a0 is excluded from the zero check because it carries the function argument.
  // ra is excluded as a specification control-flow exception (fixed return_stub address).
  // Target registers (28 total): a1-a7, t0-t6, s0-s11, gp, tp.
  mv a0, a1
  or a0, a0, a2
  or a0, a0, a3
  or a0, a0, a4
  or a0, a0, a5
  or a0, a0, a6
  or a0, a0, a7
  or a0, a0, t0
  or a0, a0, t1
  or a0, a0, t2
  or a0, a0, t3
  or a0, a0, t4
  or a0, a0, t5
  or a0, a0, t6
  or a0, a0, s0
  or a0, a0, s1
  or a0, a0, s2
  or a0, a0, s3
  or a0, a0, s4
  or a0, a0, s5
  or a0, a0, s6
  or a0, a0, s7
  or a0, a0, s8
  or a0, a0, s9
  or a0, a0, s10
  or a0, a0, s11
  or a0, a0, gp
  or a0, a0, tp

  // All 28 non-argument registers have now been sampled without prior modification.
  // Store accumulated nonarg entry mask to fixed RAM record variables.
  lui t0, %hi(shizuku_c6_fastcall_nonarg_nonzero)
  addi t0, t0, %lo(shizuku_c6_fastcall_nonarg_nonzero)
  lw t1, 0(t0)
  or t1, t1, a0
  sw t1, 0(t0)

  lui t0, %hi(shizuku_c6_fastcall_s_nonzero)
  addi t0, t0, %lo(shizuku_c6_fastcall_s_nonzero)
  lw t1, 0(t0)
  or t1, t1, a0
  sw t1, 0(t0)

  lui t0, %hi(shizuku_c6_fastcall_gp_tp_nonzero)
  addi t0, t0, %lo(shizuku_c6_fastcall_gp_tp_nonzero)
  lw t1, 0(t0)
  or t1, t1, a0
  sw t1, 0(t0)

  // Restore original argument a0 from 60(sp)
  lw a0, 60(sp)

  // Deliberately dirty return-side registers to verify the selected return sanitizer.
  li t0, 0xDEAD0001
  li t1, 0xDEAD0002
  li t2, 0xDEAD0003
  li t3, 0xDEAD0004
  li t4, 0xDEAD0005
  li t5, 0xDEAD0006
  li t6, 0xDEAD0007
  li a1, 0xDEAD0011
  li a2, 0xDEAD0012
  li a3, 0xDEAD0013
  li a4, 0xDEAD0014
  li a5, 0xDEAD0015
  li a6, 0xDEAD0016
  li a7, 0xDEAD0017
  li gp, 0xDEAD0018
  li tp, 0xDEAD0019

  // 1-method operation: increment argument a0 by 1
  addi a0, a0, 1
  ret
#endif
.size shizuku_c6_fastcall_leaf_callee, .-shizuku_c6_fastcall_leaf_callee

// ---------------------------------------------------------------------------
// Return stub: callee returns here via `ret` (ra set by Trap 1)
// ---------------------------------------------------------------------------
.global shizuku_c6_fastcall_return_stub
.type shizuku_c6_fastcall_return_stub,@function
shizuku_c6_fastcall_return_stub:
  li a7, SHIZUKU_FASTCALL_TRAP_RETURN
  ecall
  // If return fails or falls through, halt with fatal error 0xE005
  call shizuku_rv32_return_failed
1: j 1b
.size shizuku_c6_fastcall_return_stub, .-shizuku_c6_fastcall_return_stub

// ---------------------------------------------------------------------------
// Caller stub: saves caller s0-s11 + gp + tp + ra, zeroes s0-s11, ecall, restores on return
// ---------------------------------------------------------------------------
.global shizuku_c6_fastcall_stub
.type shizuku_c6_fastcall_stub,@function
shizuku_c6_fastcall_stub:
  // Allocate frame for ra, gp, tp, and s0-s11 (15 words = 60 bytes -> 64 bytes aligned)
  addi sp, sp, -64
  sw ra, 0(sp)
  sw gp, 4(sp)
  sw tp, 8(sp)
  sw s0, 12(sp)
  sw s1, 16(sp)
  sw s2, 20(sp)
  sw s3, 24(sp)
  sw s4, 28(sp)
  sw s5, 32(sp)
  sw s6, 36(sp)
  sw s7, 40(sp)
  sw s8, 44(sp)
  sw s9, 48(sp)
  sw s10, 52(sp)
  sw s11, 56(sp)

  // Callee privacy invariant: zero s0-s11 before entering trap
  li s0, 0
  li s1, 0
  li s2, 0
  li s3, 0
  li s4, 0
  li s5, 0
  li s6, 0
  li s7, 0
  li s8, 0
  li s9, 0
  li s10, 0
  li s11, 0

  // Identify fastcall CALL trap
  li a7, SHIZUKU_FASTCALL_TRAP_CALL
  ecall

  // Execution resumes here after Trap 2 mret.
  // a0 contains the return value from callee.
#if SHIZUKU_C6_FASTCALL_CLEAR_RETURN_IN_CALLER
  // The caller stub is the trusted boundary that clears return-side scratch.
  li ra, 0
  li gp, 0
  li tp, 0
  li a1, 0
  li a2, 0
  li a3, 0
  li a4, 0
  li a5, 0
  li a6, 0
  li a7, 0
  li t0, 0
  li t1, 0
  li t2, 0
  li t3, 0
  li t4, 0
  li t5, 0
  li t6, 0
#endif
  // Restore caller's original callee-saved registers, gp, tp, and ra:
  lw ra, 0(sp)
  lw gp, 4(sp)
  lw tp, 8(sp)
  lw s0, 12(sp)
  lw s1, 16(sp)
  lw s2, 20(sp)
  lw s3, 24(sp)
  lw s4, 28(sp)
  lw s5, 32(sp)
  lw s6, 36(sp)
  lw s7, 40(sp)
  lw s8, 44(sp)
  lw s9, 48(sp)
  lw s10, 52(sp)
  lw s11, 56(sp)
  addi sp, sp, 64
  ret
.size shizuku_c6_fastcall_stub, .-shizuku_c6_fastcall_stub

// ABI-safe selftest harness. It owns its register setup and records observations
// in fixed RAM, avoiding compiler register allocation across a 30-register asm.
.global shizuku_c6_fastcall_probe
.type shizuku_c6_fastcall_probe,@function
shizuku_c6_fastcall_probe:
  addi sp, sp, -128
  sw ra, 0(sp)
  sw gp, 4(sp)
  sw tp, 8(sp)
  sw s0, 12(sp)
  sw s1, 16(sp)
  sw s2, 20(sp)
  sw s3, 24(sp)
  sw s4, 28(sp)
  sw s5, 32(sp)
  sw s6, 36(sp)
  sw s7, 40(sp)
  sw s8, 44(sp)
  sw s9, 48(sp)
  sw s10, 52(sp)
  sw s11, 56(sp)
  mv t0, gp
  lui t1, %hi(shizuku_c6_fastcall_probe_gp_before)
  addi t1, t1, %lo(shizuku_c6_fastcall_probe_gp_before)
  sw t0, 0(t1)
  mv t0, tp
  lui t1, %hi(shizuku_c6_fastcall_probe_tp_before)
  addi t1, t1, %lo(shizuku_c6_fastcall_probe_tp_before)
  sw t0, 0(t1)
  li s0, 0x5A000000
  li s1, 0x5A000001
  li s2, 0x5A000002
  li s3, 0x5A000003
  li s4, 0x5A000004
  li s5, 0x5A000005
  li s6, 0x5A000006
  li s7, 0x5A000007
  li s8, 0x5A000008
  li s9, 0x5A000009
  li s10, 0x5A00000A
  li s11, 0x5A00000B
  li a0, 42
  call shizuku_c6_fastcall_stub
  sw a1, 64(sp)
  sw a2, 68(sp)
  sw a3, 72(sp)
  sw a4, 76(sp)
  sw a5, 80(sp)
  sw a6, 84(sp)
  sw a7, 88(sp)
  sw t0, 92(sp)
  sw t1, 96(sp)
  sw t2, 100(sp)
  sw t3, 104(sp)
  sw t4, 108(sp)
  sw t5, 112(sp)
  sw t6, 116(sp)
  lui t0, %hi(shizuku_c6_fastcall_probe_return)
  addi t0, t0, %lo(shizuku_c6_fastcall_probe_return)
  sw a0, 0(t0)
  li a0, 0
  lw t1, 64(sp)
  or a0, a0, t1
  lw t1, 68(sp)
  or a0, a0, t1
  lw t1, 72(sp)
  or a0, a0, t1
  lw t1, 76(sp)
  or a0, a0, t1
  lw t1, 80(sp)
  or a0, a0, t1
  lw t1, 84(sp)
  or a0, a0, t1
  lw t1, 88(sp)
  or a0, a0, t1
  lw t1, 92(sp)
  or a0, a0, t1
  lw t1, 96(sp)
  or a0, a0, t1
  lw t1, 100(sp)
  or a0, a0, t1
  lw t1, 104(sp)
  or a0, a0, t1
  lw t1, 108(sp)
  or a0, a0, t1
  lw t1, 112(sp)
  or a0, a0, t1
  lw t1, 116(sp)
  or a0, a0, t1
  lui t0, %hi(shizuku_c6_fastcall_probe_scratch)
  addi t0, t0, %lo(shizuku_c6_fastcall_probe_scratch)
  sw a0, 0(t0)
  lui t0, %hi(shizuku_c6_fastcall_probe_s_after)
  addi t0, t0, %lo(shizuku_c6_fastcall_probe_s_after)
  sw s0, 0(t0)
  sw s1, 4(t0)
  sw s2, 8(t0)
  sw s3, 12(t0)
  sw s4, 16(t0)
  sw s5, 20(t0)
  sw s6, 24(t0)
  sw s7, 28(t0)
  sw s8, 32(t0)
  sw s9, 36(t0)
  sw s10, 40(t0)
  sw s11, 44(t0)
  mv t1, gp
  lui t0, %hi(shizuku_c6_fastcall_probe_gp_after)
  addi t0, t0, %lo(shizuku_c6_fastcall_probe_gp_after)
  sw t1, 0(t0)
  mv t1, tp
  lui t0, %hi(shizuku_c6_fastcall_probe_tp_after)
  addi t0, t0, %lo(shizuku_c6_fastcall_probe_tp_after)
  sw t1, 0(t0)
  lw ra, 0(sp)
  lw gp, 4(sp)
  lw tp, 8(sp)
  lw s0, 12(sp)
  lw s1, 16(sp)
  lw s2, 20(sp)
  lw s3, 24(sp)
  lw s4, 28(sp)
  lw s5, 32(sp)
  lw s6, 36(sp)
  lw s7, 40(sp)
  lw s8, 44(sp)
  lw s9, 48(sp)
  lw s10, 52(sp)
  lw s11, 56(sp)
  addi sp, sp, 128
  ret
.size shizuku_c6_fastcall_probe, .-shizuku_c6_fastcall_probe

// ---------------------------------------------------------------------------
// Fastcall check entry point from rv32_trap.S (before full frame save)
// ---------------------------------------------------------------------------
.global shizuku_c6_fastcall_trap_check
.type shizuku_c6_fastcall_trap_check,@function
shizuku_c6_fastcall_trap_check:
  // Swap t0 with mscratch. Outside traps, mscratch points to kernel stack top.
  csrrw t0, mscratch, t0
  beqz t0, .Lfastcall_from_nested
  // Save t1 at top of kernel stack
  sw t1, -4(t0)
  csrr t1, mcause
  addi t1, t1, -11
  bnez t1, .Lfastcall_not_ecall
  li t1, SHIZUKU_FASTCALL_TRAP_CALL
  beq a7, t1, shizuku_c6_fastcall_trap_call
  li t1, SHIZUKU_FASTCALL_TRAP_RETURN
  beq a7, t1, shizuku_c6_fastcall_trap_return

.Lfastcall_not_ecall:
  lw t1, -4(t0)
.Lfastcall_from_nested:
  csrrw t0, mscratch, t0
  j shizuku_rv32_trap_entry_legacy
.size shizuku_c6_fastcall_trap_check, .-shizuku_c6_fastcall_trap_check

// ---------------------------------------------------------------------------
// Trap 1: CALL
// ---------------------------------------------------------------------------
.global shizuku_c6_fastcall_trap_call
.type shizuku_c6_fastcall_trap_call,@function
shizuku_c6_fastcall_trap_call:
#if SHIZUKU_C6_FASTCALL_TRAP_STATS
  csrr t2, 0x7E2  // trap asm body start timestamp (PCCR)
#endif

  // Access private M-mode fixed descriptor
  lui t1, %hi(shizuku_c6_fastcall_descriptor)
  addi t1, t1, %lo(shizuku_c6_fastcall_descriptor)

  // O(1) Fail-Closed Guard: descriptor must be idle (depth == 0)
  lw t3, SHIZUKU_C6_FASTCALL_DESC_DEPTH(t1)
  bnez t3, .Lfastcall_call_reject

  // Save caller sp and caller return PC (mepc + 4)
  sw sp, SHIZUKU_C6_FASTCALL_DESC_CALLER_SP(t1)
  csrr t3, mepc
  addi t3, t3, 4
  sw t3, SHIZUKU_C6_FASTCALL_DESC_CALLER_PC(t1)
  li t3, 1
  sw t3, SHIZUKU_C6_FASTCALL_DESC_DEPTH(t1)          // depth = 1

  // Set callee target PC from fixed descriptor (caller cannot supply arbitrary PC)
  lw t3, SHIZUKU_C6_FASTCALL_DESC_ENTRY_PC(t1)
  csrw mepc, t3

  // Set return address (ra) to fastcall return stub
  lui ra, %hi(shizuku_c6_fastcall_return_stub)
  addi ra, ra, %lo(shizuku_c6_fastcall_return_stub)

  // Restore mscratch = kernel stack top (t0)
  csrw mscratch, t0

  // Record measured asm body duration (not entire architectural IRQ-off window)
#if SHIZUKU_C6_FASTCALL_TRAP_STATS
  csrr t3, 0x7E2
  sub t2, t3, t2
  lui t1, %hi(shizuku_c6_fastcall_record)
  addi t1, t1, %lo(shizuku_c6_fastcall_record)
  lw t3, SHIZUKU_C6_FASTCALL_REC_MAX_TRAP1(t1)
  bleu t2, t3, 1f
  sw t2, SHIZUKU_C6_FASTCALL_REC_MAX_TRAP1(t1)
1:
#endif
  // Register privacy: zero all scratch registers, gp, tp so caller data does not leak
  li gp, 0
  li tp, 0
  li a1, 0
  li a2, 0
  li a3, 0
  li a4, 0
  li a5, 0
  li a6, 0
  li a7, 0
  li t0, 0
  li t1, 0
  li t2, 0
  li t3, 0
  li t4, 0
  li t5, 0
  li t6, 0

  mret

.Lfastcall_call_reject:
  // Rejection diagnostic: restore mscratch and trigger fatal error 0xE005
  csrw mscratch, t0
  call shizuku_rv32_return_failed
1: j 1b
.size shizuku_c6_fastcall_trap_call, .-shizuku_c6_fastcall_trap_call

// ---------------------------------------------------------------------------
// Trap 2: RETURN
// ---------------------------------------------------------------------------
.global shizuku_c6_fastcall_trap_return
.type shizuku_c6_fastcall_trap_return,@function
shizuku_c6_fastcall_trap_return:
#if SHIZUKU_C6_FASTCALL_TRAP_STATS
  csrr t4, 0x7E2  // trap asm body start timestamp (PCCR)
#endif

  lui t1, %hi(shizuku_c6_fastcall_descriptor)
  addi t1, t1, %lo(shizuku_c6_fastcall_descriptor)

  // O(1) Fail-Closed Guard: descriptor must be active (depth == 1)
  lw t3, SHIZUKU_C6_FASTCALL_DESC_DEPTH(t1)
  li t2, 1
  bne t3, t2, .Lfastcall_return_reject

  // Restore caller sp and caller return PC into mepc
  lw sp, SHIZUKU_C6_FASTCALL_DESC_CALLER_SP(t1)
  lw t3, SHIZUKU_C6_FASTCALL_DESC_CALLER_PC(t1)
  csrw mepc, t3

  // Reset depth, increment call_count
  sw zero, SHIZUKU_C6_FASTCALL_DESC_DEPTH(t1)
  lw t3, SHIZUKU_C6_FASTCALL_DESC_CALL_COUNT(t1)
  addi t3, t3, 1
  sw t3, SHIZUKU_C6_FASTCALL_DESC_CALL_COUNT(t1)

  // Restore mscratch = kernel stack top (t0)
  csrw mscratch, t0

  // Record measured asm body duration (not entire architectural IRQ-off window)
#if SHIZUKU_C6_FASTCALL_TRAP_STATS
  csrr t3, 0x7E2
  sub t2, t3, t4
  lui t1, %hi(shizuku_c6_fastcall_record)
  addi t1, t1, %lo(shizuku_c6_fastcall_record)
  lw t3, SHIZUKU_C6_FASTCALL_REC_MAX_TRAP2(t1)
  bleu t2, t3, 1f
  sw t2, SHIZUKU_C6_FASTCALL_REC_MAX_TRAP2(t1)
1:
#endif
  // Register privacy: zero all non-return scratch registers AND ra, gp, tp so callee secrets do not leak
  // Note: a0 preserves callee return value
#if !SHIZUKU_C6_FASTCALL_CLEAR_RETURN_IN_CALLER
  li ra, 0
  li gp, 0
  li tp, 0
  li a1, 0
  li a2, 0
  li a3, 0
  li a4, 0
  li a5, 0
  li a6, 0
  li a7, 0
  li t0, 0
  li t1, 0
  li t2, 0
  li t3, 0
  li t4, 0
  li t5, 0
  li t6, 0
#endif

  mret

.Lfastcall_return_reject:
  // Rejection diagnostic: restore mscratch and trigger fatal error 0xE005
  csrw mscratch, t0
  call shizuku_rv32_return_failed
1: j 1b
.size shizuku_c6_fastcall_trap_return, .-shizuku_c6_fastcall_trap_return

.option pop
