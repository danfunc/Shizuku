#include "arch/riscv/frame_layout.h"
// C6 uses 256-byte-aligned vectored mtvec with 4-byte slots (IDF vectors_intc.S).
.section .iram1,"ax",@progbits
.balign 256
.option push
.option norvc
.option norelax
.global shizuku_c6_vectors
shizuku_c6_vectors:
.rept 32
  j shizuku_c6_trap_entry
.endr
.global shizuku_c6_trap_entry
.type shizuku_c6_trap_entry,@function
shizuku_c6_trap_entry:
  // Stage 2 is M-only. The interrupted stack is trusted and writable.
  // Saving before the kernel-stack swap preserves V2's thread-frame geometry.
  addi sp, sp, -C6_FRAME_BYTES
  sw ra, C6_F_RA(sp)
  sw t0, C6_F_T0(sp)
  sw t1, C6_F_T1(sp)
  sw t2, C6_F_T2(sp)
  sw a0, C6_F_A0(sp)
  sw a1, C6_F_A1(sp)
  sw a2, C6_F_A2(sp)
  sw a3, C6_F_A3(sp)
  sw a4, C6_F_A4(sp)
  sw a5, C6_F_A5(sp)
  sw a6, C6_F_A6(sp)
  sw a7, C6_F_A7(sp)
  sw t3, C6_F_T3(sp)
  sw t4, C6_F_T4(sp)
  sw t5, C6_F_T5(sp)
  sw t6, C6_F_T6(sp)
  sw s0, C6_F_S0(sp)
  sw s1, C6_F_S1(sp)
  sw s2, C6_F_S2(sp)
  sw s3, C6_F_S3(sp)
  sw s4, C6_F_S4(sp)
  sw s5, C6_F_S5(sp)
  sw s6, C6_F_S6(sp)
  sw s7, C6_F_S7(sp)
  sw s8, C6_F_S8(sp)
  sw s9, C6_F_S9(sp)
  sw s10, C6_F_S10(sp)
  sw s11, C6_F_S11(sp)
  sw gp, C6_F_GP(sp)
  sw tp, C6_F_TP(sp)
  csrr t0, mepc
  sw t0, C6_F_MEPC(sp)
  csrr t0, mstatus
  sw t0, C6_F_MSTATUS(sp)
  csrr t0, mcause
  sw t0, C6_F_MCAUSE(sp)
  csrr t0, mtval
  sw t0, C6_F_MTVAL(sp)
  mv a0, sp
  // mscratch != 0 outside traps: dedicated kernel stack top.
  // mscratch == 0 inside traps: nested traps keep the interrupted kernel stack.
  csrrw t0, mscratch, zero
  beqz t0, 1f
  mv sp, t0
1:
  // t0 is the original mscratch; save it separately from the movable V2 frame.
  addi sp, sp, -16
  sw t0, 0(sp)
  sw a0, 4(sp)
  call shizuku_c6_trap_dispatch
  lw t0, 0(sp)
  // Restore only after dispatch. Nested returns write zero, outer returns top.
  csrw mscratch, t0
  mv sp, a0
.global shizuku_c6_restore
shizuku_c6_restore:
  lw t0, C6_F_MEPC(sp)
  csrw mepc, t0
  lw t0, C6_F_MSTATUS(sp)
  csrw mstatus, t0
  lw ra, C6_F_RA(sp)
  lw t0, C6_F_T0(sp)
  lw t1, C6_F_T1(sp)
  lw t2, C6_F_T2(sp)
  lw a0, C6_F_A0(sp)
  lw a1, C6_F_A1(sp)
  lw a2, C6_F_A2(sp)
  lw a3, C6_F_A3(sp)
  lw a4, C6_F_A4(sp)
  lw a5, C6_F_A5(sp)
  lw a6, C6_F_A6(sp)
  lw a7, C6_F_A7(sp)
  lw t3, C6_F_T3(sp)
  lw t4, C6_F_T4(sp)
  lw t5, C6_F_T5(sp)
  lw t6, C6_F_T6(sp)
  lw s0, C6_F_S0(sp)
  lw s1, C6_F_S1(sp)
  lw s2, C6_F_S2(sp)
  lw s3, C6_F_S3(sp)
  lw s4, C6_F_S4(sp)
  lw s5, C6_F_S5(sp)
  lw s6, C6_F_S6(sp)
  lw s7, C6_F_S7(sp)
  lw s8, C6_F_S8(sp)
  lw s9, C6_F_S9(sp)
  lw s10, C6_F_S10(sp)
  lw s11, C6_F_S11(sp)
  lw gp, C6_F_GP(sp)
  lw tp, C6_F_TP(sp)
  addi sp, sp, C6_FRAME_BYTES
  mret
.size shizuku_c6_trap_entry, .-shizuku_c6_trap_entry
.global shizuku_c6_first_return
.type shizuku_c6_first_return,@function
shizuku_c6_first_return:
  mv sp, a0
  j shizuku_c6_restore
.global shizuku_c6_return_stub
.type shizuku_c6_return_stub,@function
shizuku_c6_return_stub:
  // V2 RETURN(number=2, count=1, value=a0, error=0, claim=0).
  mv a2, a0
  li a0, 2
  li a1, 1
  li a3, 0
  li a4, 0
  ecall
  call shizuku_c6_return_failed
2: j 2b
.option pop
