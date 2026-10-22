#include "arch/riscv/rv32_frame.h"
// Generic RV32 M-mode trap path: full register/CSR save, mscratch stack swap, dispatch,
// restore/mret, first return, and the ECALL return stub.
// Placement is chip-owned: this body has no .section and is textually included by the
// chip's vector wrapper inside the chip's linker section.
.option push
.option norvc
.option norelax
.global shizuku_rv32_trap_entry
.type shizuku_rv32_trap_entry,@function
shizuku_rv32_trap_entry:
  // The interrupted stack is trusted and writable (no U-mode stack switch here).
  // Saving before the kernel-stack swap preserves V2's thread-frame geometry.
  addi sp, sp, -RV32_FRAME_BYTES
  sw ra, RV32_F_RA(sp)
  sw t0, RV32_F_T0(sp)
  sw t1, RV32_F_T1(sp)
  sw t2, RV32_F_T2(sp)
  sw a0, RV32_F_A0(sp)
  sw a1, RV32_F_A1(sp)
  sw a2, RV32_F_A2(sp)
  sw a3, RV32_F_A3(sp)
  sw a4, RV32_F_A4(sp)
  sw a5, RV32_F_A5(sp)
  sw a6, RV32_F_A6(sp)
  sw a7, RV32_F_A7(sp)
  sw t3, RV32_F_T3(sp)
  sw t4, RV32_F_T4(sp)
  sw t5, RV32_F_T5(sp)
  sw t6, RV32_F_T6(sp)
  sw s0, RV32_F_S0(sp)
  sw s1, RV32_F_S1(sp)
  sw s2, RV32_F_S2(sp)
  sw s3, RV32_F_S3(sp)
  sw s4, RV32_F_S4(sp)
  sw s5, RV32_F_S5(sp)
  sw s6, RV32_F_S6(sp)
  sw s7, RV32_F_S7(sp)
  sw s8, RV32_F_S8(sp)
  sw s9, RV32_F_S9(sp)
  sw s10, RV32_F_S10(sp)
  sw s11, RV32_F_S11(sp)
  sw gp, RV32_F_GP(sp)
  sw tp, RV32_F_TP(sp)
  csrr t0, mepc
  sw t0, RV32_F_MEPC(sp)
  csrr t0, mstatus
  sw t0, RV32_F_MSTATUS(sp)
  csrr t0, mcause
  sw t0, RV32_F_MCAUSE(sp)
  csrr t0, mtval
  sw t0, RV32_F_MTVAL(sp)
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
  call shizuku_rv32_trap_dispatch
  lw t0, 0(sp)
  // Restore only after dispatch. Nested returns write zero, outer returns top.
  csrw mscratch, t0
  mv sp, a0
.global shizuku_rv32_restore
shizuku_rv32_restore:
  lw t0, RV32_F_MEPC(sp)
  csrw mepc, t0
  lw t0, RV32_F_MSTATUS(sp)
  csrw mstatus, t0
  lw ra, RV32_F_RA(sp)
  lw t0, RV32_F_T0(sp)
  lw t1, RV32_F_T1(sp)
  lw t2, RV32_F_T2(sp)
  lw a0, RV32_F_A0(sp)
  lw a1, RV32_F_A1(sp)
  lw a2, RV32_F_A2(sp)
  lw a3, RV32_F_A3(sp)
  lw a4, RV32_F_A4(sp)
  lw a5, RV32_F_A5(sp)
  lw a6, RV32_F_A6(sp)
  lw a7, RV32_F_A7(sp)
  lw t3, RV32_F_T3(sp)
  lw t4, RV32_F_T4(sp)
  lw t5, RV32_F_T5(sp)
  lw t6, RV32_F_T6(sp)
  lw s0, RV32_F_S0(sp)
  lw s1, RV32_F_S1(sp)
  lw s2, RV32_F_S2(sp)
  lw s3, RV32_F_S3(sp)
  lw s4, RV32_F_S4(sp)
  lw s5, RV32_F_S5(sp)
  lw s6, RV32_F_S6(sp)
  lw s7, RV32_F_S7(sp)
  lw s8, RV32_F_S8(sp)
  lw s9, RV32_F_S9(sp)
  lw s10, RV32_F_S10(sp)
  lw s11, RV32_F_S11(sp)
  lw gp, RV32_F_GP(sp)
  lw tp, RV32_F_TP(sp)
  addi sp, sp, RV32_FRAME_BYTES
  mret
.size shizuku_rv32_trap_entry, .-shizuku_rv32_trap_entry
.global shizuku_rv32_first_return
.type shizuku_rv32_first_return,@function
shizuku_rv32_first_return:
  mv sp, a0
  j shizuku_rv32_restore
.global shizuku_rv32_return_stub
.type shizuku_rv32_return_stub,@function
shizuku_rv32_return_stub:
  // 戻り口の ecall も caller stub と同じく呼出し記録の枠を空ける: スレッドの最外のメソッドが戻る
  // 場合 (d == 0 && 呼出し段なし) は kobj の exit_thread へ dispatch 段が積まれ、その枠が要る。
  addi sp, sp, -RV32_CALL_HEADER_BYTES
  // V2 RETURN(number=2, count=1, value=a0, error=0, return-stub marker).
  mv a2, a0
  li a0, 2
  li a1, 1
  li a3, 0
  li a4, 0xA55A
  ecall
  call shizuku_rv32_return_failed
2: j 2b
.option pop
