#include "arch/riscv/rv32_frame.h"
// Generic RV32 M-mode trap path: full register/CSR save, mscratch stack swap, dispatch,
// restore/mret, first return, and the ECALL return stub.
// Placement is chip-owned: this body has no .section and is textually included by the
// chip's vector wrapper inside the chip's linker section.
#if defined(SHIZUKU_EXP_C6_STAGE1)
.option push
.option norvc
.option norelax
.global shizuku_rv32_trap_entry
.type shizuku_rv32_trap_entry,@function
shizuku_rv32_trap_entry:
  // 段1安全不変条件:
  // U-mode 走行時は mscratch に trusted stack cursor (非0) が格納されている。
  // M-mode 走行時は mscratch に 0 が格納されている。
  // mscratch と sp をアトミックに交換
  csrrw sp, mscratch, sp
  bnez sp, 1f

  // mscratch == 0: M-mode (特権スレッドまたはネストしたカーネル) からのトラップ。
  // sp を元の M-mode スレッドスタックに戻し、スレッドスタック上にフレームを確保 (V2 既定幾何維持)。
  csrrw sp, mscratch, sp
  addi sp, sp, -RV32_FRAME_BYTES
  sw ra, RV32_F_RA(sp)
  sw t0, RV32_F_T0(sp)
  sw t1, RV32_F_T1(sp)
  sw t2, RV32_F_T2(sp)
  addi t0, sp, RV32_FRAME_BYTES
  sw t0, RV32_F_RESERVED0(sp)
  sw zero, RV32_F_RESERVED1(sp)
  j 2f

1:
  // mscratch != 0: U-mode (非特権 worker) からのトラップ。
  // sp は trusted kernel stack cursor、mscratch は元の untrusted U-mode sp。
  // U thread のスタックには一切触れず、trusted stack 上にフレーム領域を確保。
  addi sp, sp, -RV32_FRAME_BYTES
  sw ra, RV32_F_RA(sp)
  sw t0, RV32_F_T0(sp)
  sw t1, RV32_F_T1(sp)
  sw t2, RV32_F_T2(sp)
  csrrw t0, mscratch, zero
  sw t0, RV32_F_RESERVED0(sp)
  addi t0, sp, RV32_FRAME_BYTES
  sw t0, RV32_F_RESERVED1(sp)

2:
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
  la t0, shizuku_c6_dispatch_stack
  sub t1, a0, t0
  li t3, 4096
  bltu t1, t3, 1f
  la sp, shizuku_c6_dispatch_stack + 4096
1:
  addi sp, sp, -16
  lw t0, RV32_F_RESERVED1(a0)
  sw t0, 0(sp)
  sw a0, 4(sp)
  call shizuku_rv32_trap_dispatch
  mv sp, a0
.global shizuku_rv32_restore
shizuku_rv32_restore:
  lw t0, RV32_F_MEPC(sp)
  csrw mepc, t0
  lw t0, RV32_F_MSTATUS(sp)
  csrw mstatus, t0
  lw ra, RV32_F_RA(sp)
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
  // 復帰先特権モードに応じた mscratch 設定:
  // RESERVED1 には復帰先が U-mode のときは trusted_stack_cursor、
  // M-mode のときは 0 が格納されている。
  // t0 をスクラッチとして mscratch を復元した後に、保存済み t0 を最終復元する。
  lw t0, RV32_F_RESERVED1(sp)
  csrw mscratch, t0
  lw t0, RV32_F_T0(sp)
  lw sp, RV32_F_RESERVED0(sp)
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

#else
// Non-C6 generic RV32 trap handler (RP2350, C5, etc. - original code preserved exactly)
.option push
.option norvc
.option norelax
.global shizuku_rv32_trap_entry
.type shizuku_rv32_trap_entry,@function
shizuku_rv32_trap_entry:
#if defined(SHIZUKU_EXP_FASTCALL) && SHIZUKU_EXP_FASTCALL
  j shizuku_c6_fastcall_trap_check
.global shizuku_rv32_trap_entry_legacy
shizuku_rv32_trap_entry_legacy:
#endif
  // The interrupted stack is trusted and writable (no U-mode stack switch here).
  // Saving before the kernel-stack swap preserves V2's thread-frame geometry.
  addi sp, sp, -RV32_FRAME_BYTES
  sw ra, RV32_F_RA(sp)
  sw t0, RV32_F_T0(sp)
#if defined(SHIZUKU_C6_STAGE05_TRACE) && SHIZUKU_C6_STAGE05_TRACE
  csrr t0, 0x7E2
  sw t0, RV32_F_RESERVED0(sp)
#endif
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
#if defined(SHIZUKU_C6_STAGE05_TRACE) && SHIZUKU_C6_STAGE05_TRACE
  lui t1, %hi(shizuku_c6_stage05_active)
  lw t1, %lo(shizuku_c6_stage05_active)(t1)
  beqz t1, 2f
  csrr t0, 0x7E2
  lui t1, %hi(shizuku_c6_stage05_t6)
  sw t0, %lo(shizuku_c6_stage05_t6)(t1)
2:
#endif
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
#endif
