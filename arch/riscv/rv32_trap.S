#include "arch/riscv/rv32_frame.h"
// Generic RV32 M-mode trap path: mscratch stack swap, dispatch, restore/mret, first return, and the
// ECALL return stub.
//   - 割込み・例外 (ECALL 以外): 全レジスタ / CSR を保存し、全部復元して戻る (従来どおり)。
//   - M-mode の ECALL: caller が自分で退避する ABI (rv32_abi.hpp) なので、入力 (a0-a7) と戻りに要る
//     mepc / mstatus だけを保存する。戻りは「同じスレッドへ」も「切替え」も、戻す先の frame の
//     mcause を見て出口を選ぶ: ECALL の戻り (RV32_TAG_ECALL) は a0/a1 以外を消し、呼び先の入口
//     (RV32_TAG_ENTRY) は引数と ra だけを入れてほかは caller の値を通す。どちらも s*/gp/tp を
//     復元しない (ECALL の caller はそれらを持ち越さない)。
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
  sw t0, RV32_F_T0(sp)
  // M-mode ECALL (exception code 11, interrupt bit clear)? 例外コード以外のビット (C5 の CLIC が載せる
  // mpp / mpie / mpil など) は無視して比べる。
  // (t0 だけを使う: 全保存側へ分岐したとき t1 以降は割込まれたスレッドの値のまま。)
  csrr t0, mcause
  bltz t0, .Lrv32_full_save
  slli t0, t0, 20
  srli t0, t0, 20
  addi t0, t0, -11
  bnez t0, .Lrv32_full_save
  // ---- ECALL: caller-saved ABI。入力と戻りに要るものだけ保存する。
  sw a0, RV32_F_A0(sp)
  sw a1, RV32_F_A1(sp)
  sw a2, RV32_F_A2(sp)
  sw a3, RV32_F_A3(sp)
  sw a4, RV32_F_A4(sp)
  sw a5, RV32_F_A5(sp)
  sw a6, RV32_F_A6(sp)
  sw a7, RV32_F_A7(sp)
  csrr t0, mepc
  sw t0, RV32_F_MEPC(sp)
  csrr t0, mstatus
  sw t0, RV32_F_MSTATUS(sp)
  li t0, RV32_TAG_ECALL
  sw t0, RV32_F_MCAUSE(sp)
  j .Lrv32_save_done
.Lrv32_full_save:
  sw t1, RV32_F_T1(sp)
  sw ra, RV32_F_RA(sp)
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
.Lrv32_save_done:
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
  lw t0, RV32_F_MCAUSE(sp)
  li t1, RV32_TAG_ECALL
  beq t0, t1, .Lrv32_exit_ecall
  li t1, RV32_TAG_ENTRY
  beq t0, t1, .Lrv32_exit_entry
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
// ---- ECALL からの戻り (同じスレッドへ / 切替え先の ECALL 待ちスレッドへ共通)。
//   a0/a1 (戻り値) と mepc/mstatus/sp だけを戻す。ra・t*・a2-a7・s* は 0 に消す (呼び先やカーネルの
//   値を見せない)。gp/tp は触らない: caller stub が戻った直後に自分の値を入れ直すが、その 1 命令の間に
//   割込みが入っても IDF 側の ISR が使う gp が壊れないように、ここでは 0 にしない。
.Lrv32_exit_ecall:
  lw t0, RV32_F_MEPC(sp)
  csrw mepc, t0
  lw t0, RV32_F_MSTATUS(sp)
  csrw mstatus, t0
  lw a0, RV32_F_A0(sp)
  lw a1, RV32_F_A1(sp)
  addi sp, sp, RV32_FRAME_BYTES
  mv ra, zero
  mv t0, zero
  mv t1, zero
  mv t2, zero
  mv t3, zero
  mv t4, zero
  mv t5, zero
  mv t6, zero
  mv a2, zero
  mv a3, zero
  mv a4, zero
  mv a5, zero
  mv a6, zero
  mv a7, zero
  mv s0, zero
  mv s1, zero
  mv s2, zero
  mv s3, zero
  mv s4, zero
  mv s5, zero
  mv s6, zero
  mv s7, zero
  mv s8, zero
  mv s9, zero
  mv s10, zero
  mv s11, zero
  mret
// ---- 呼び先の入口 (CALL_METHOD / descriptor call / kobj への dispatch)。
//   引数 a0-a5 と ra (戻り口)・mepc (入口)・sp だけをカーネルが決める。s*/gp/tp は caller の値の
//   まま通す (カーネルの C は callee-saved を保ち、この経路では切替えが起きない)。t*・a6・a7 には
//   カーネルの値が残っているので消す。
.Lrv32_exit_entry:
  lw t0, RV32_F_MEPC(sp)
  csrw mepc, t0
  lw t0, RV32_F_MSTATUS(sp)
  csrw mstatus, t0
  lw ra, RV32_F_RA(sp)
  lw a0, RV32_F_A0(sp)
  lw a1, RV32_F_A1(sp)
  lw a2, RV32_F_A2(sp)
  lw a3, RV32_F_A3(sp)
  lw a4, RV32_F_A4(sp)
  lw a5, RV32_F_A5(sp)
  addi sp, sp, RV32_FRAME_BYTES
  mv t0, zero
  mv t1, zero
  mv t2, zero
  mv t3, zero
  mv t4, zero
  mv t5, zero
  mv t6, zero
  mv a6, zero
  mv a7, zero
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
