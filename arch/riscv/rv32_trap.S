#include "arch/riscv/rv32_frame.h"
// Generic RV32 M-mode trap path: mscratch stack swap, dispatch, restore/mret, first return, and the
// ECALL return stub.
//   - 割込み・例外 (ECALL 以外): 全レジスタ / CSR を保存し、全部復元して戻る (従来どおり)。
//   - M-mode の ECALL: caller が自分で退避する ABI (rv32_abi.hpp) なので、入力 (a0-a7) と戻りに要る
//     mepc / mstatus だけを保存する。戻りは「同じスレッドへ」も「切替え」も、戻す先の frame の
//     mcause を見て出口を選ぶ。カーネルはルータに徹し、レジスタは素通しする:
//       RV32_TAG_ECALL       : ECALL の戻り。a0/a1 だけを入れ、ほかは消さない。
//       RV32_TAG_ENTRY       : 呼び先の入口。引数 a0-a7 と ra だけを入れ、ほかは消さない。
//       RV32_TAG_ECALL_SCRUB : callee が自分の意思で戻らない巻き戻し・別スレッドの再開。
//                              a0/a1 以外 (ra・t*・a2-a7・s*) を 0 に消す。
//     どの出口も s*/gp/tp を復元しない (ECALL の caller はそれらを持ち越さない)。
// Placement is chip-owned: this body has no .section and is textually included by the
// chip's vector wrapper inside the chip's linker section.
.option push
.option norvc
.option norelax
.global shizuku_rv32_trap_entry
.type shizuku_rv32_trap_entry,@function
shizuku_rv32_trap_entry:
#if defined(SHIZUKU_C6_UMODE)
  // 段1安全不変条件:
  // U-mode 走行時は mscratch に trusted stack cursor (非0) が格納されている。
  // M-mode 走行時は mscratch に 0 が格納されている。
  // mscratch と sp をアトミックに交換
  csrrw sp, mscratch, sp
  bnez sp, .Lrv32_from_umode

  // mscratch == 0: M-mode (特権スレッドまたはネストしたカーネル) からのトラップ。
  // sp を元の M-mode スレッドスタックに戻し、スレッドスタック上にフレームを確保 (V2 既定幾何維持)。
  csrrw sp, mscratch, sp
  addi sp, sp, -RV32_FRAME_BYTES
  sw t0, RV32_F_T0(sp)
  addi t0, sp, RV32_FRAME_BYTES
  sw t0, RV32_F_RESERVED0(sp)
  sw zero, RV32_F_RESERVED1(sp)
  j .Lrv32_check_cause

.Lrv32_from_umode:
  // mscratch != 0: U-mode (非特権 worker) からのトラップ。
  // sp は trusted kernel stack cursor、mscratch は元の untrusted U-mode sp。
  // U thread のスタックには一切触れず、trusted stack 上にフレーム領域を確保。
  addi sp, sp, -RV32_FRAME_BYTES
  sw t0, RV32_F_T0(sp)
  csrrw t0, mscratch, zero
  sw t0, RV32_F_RESERVED0(sp)
  addi t0, sp, RV32_FRAME_BYTES
  sw t0, RV32_F_RESERVED1(sp)

.Lrv32_check_cause:
  // M-mode ECALL (exception code 11, interrupt bit clear)? 例外コード以外のビット (C5 の CLIC が載せる
  // mpp / mpie / mpil など) は無視して比べる。
  // (t0 だけを使う: 全保存側へ分岐したとき t1 以降は割込まれたスレッドの値のまま。)
  csrr t0, mcause
  bltz t0, .Lrv32_full_save
  slli t0, t0, 20
  srli t0, t0, 20
  addi t0, t0, -11
  bnez t0, .Lrv32_check_umode_ecall
  li t1, RV32_TAG_ECALL
  j .Lrv32_is_ecall

.Lrv32_check_umode_ecall:
  // U-mode ECALL (exception code 8)?
  addi t0, t0, 3
  bnez t0, .Lrv32_full_save
  li t1, RV32_TAG_U_ECALL
#else
  // U-mode off: M-mode 専用の高速直接退避 (cae459f と同一の旧命令列)
  addi sp, sp, -RV32_FRAME_BYTES
  sw t0, RV32_F_T0(sp)
  csrr t0, mcause
  bltz t0, .Lrv32_full_save
  slli t0, t0, 20
  srli t0, t0, 20
  addi t0, t0, -11
  bnez t0, .Lrv32_full_save
  li t1, RV32_TAG_ECALL
#endif

.Lrv32_is_ecall:
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
  // ECALL は常に 32 ビット命令 (C 拡張が有効でも): 戻り先は次の命令。
  addi t0, t0, 4
  sw t0, RV32_F_MEPC(sp)
  csrr t0, mstatus
  sw t0, RV32_F_MSTATUS(sp)
  sw t1, RV32_F_MCAUSE(sp)
  // 番号 (a0、frame に保存済みなので壊してよい) で表を引き、各 API の処理へ直接入る。
  // 範囲外は 0 番 (thread mode の API・primitive・raw 配送を決める共通の入口)。
  li t0, RV32_ECALL_TABLE_ENTRIES
  bltu a0, t0, .Lrv32_ecall_in_range
  li a0, 0
.Lrv32_ecall_in_range:
  slli a0, a0, 2
  la t1, shizuku_rv32_ecall_table
  add t1, t1, a0
  lw t1, 0(t1)
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
  // 割込み・例外 (ECALL 以外) は chip の共通の入口 (route_trap) へ。t1 は保存済みなので使ってよい。
  la t1, shizuku_rv32_trap_dispatch
.Lrv32_save_done:
  // t1 = 呼ぶ関数 (ECALL は番号の表の要素、それ以外は shizuku_rv32_trap_dispatch)。引数は frame。
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
  jalr t1
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
#if defined(SHIZUKU_C6_UMODE)
  // U の往復で通る U_ENTRY / U_ECALL を、まれな SCRUB 系より先に判定する (M の ECALL / ENTRY は先頭のまま)。
  li t1, RV32_TAG_U_ENTRY
  beq t0, t1, .Lrv32_umode_exit_entry
  li t1, RV32_TAG_U_ECALL
  beq t0, t1, .Lrv32_umode_exit_ecall
  li t1, RV32_TAG_ECALL_SCRUB
  beq t0, t1, .Lrv32_exit_scrub
  li t1, RV32_TAG_U_SCRUB
  beq t0, t1, .Lrv32_umode_exit_scrub
#else
  li t1, RV32_TAG_ECALL_SCRUB
  beq t0, t1, .Lrv32_exit_scrub
#endif
1:
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
#if defined(SHIZUKU_C6_UMODE)
  // 復帰先特権モードに応じた mscratch 設定:
  // RESERVED1 には復帰先が U-mode のときは trusted_stack_cursor、
  // M-mode のときは 0 が格納されている。
  lw t0, RV32_F_RESERVED1(sp)
  csrw mscratch, t0
  lw t0, RV32_F_T0(sp)
  lw sp, RV32_F_RESERVED0(sp)
  mret
#else
  addi sp, sp, RV32_FRAME_BYTES
  mret
#endif
// ---- ECALL からの戻り (RV32_TAG_ECALL、素通し)。
//   a0/a1 (戻り値) と mepc/mstatus/sp だけを戻す。ほかのレジスタには触らない (カーネルを抜けた時点の
//   値のまま)。caller は a0/a1 以外を信用せず、自分の s*・ra・gp・tp を自分のスタックから戻す。
//   callee の値を見せたくない callee は、自分の stub で消してから戻る。
.Lrv32_exit_ecall:
  lw t0, RV32_F_MEPC(sp)
  csrw mepc, t0
  lw t0, RV32_F_MSTATUS(sp)
  csrw mstatus, t0
  lw a0, RV32_F_A0(sp)
  lw a1, RV32_F_A1(sp)
  addi sp, sp, RV32_FRAME_BYTES
#if defined(SHIZUKU_C6_UMODE)
  csrw mscratch, zero
#endif
  mret
// ---- ECALL からの戻り (RV32_TAG_ECALL_SCRUB、消去)。
//   callee が自分の意思で戻らない巻き戻し (多段の EXIT_METHOD / RETURN、FORWARD_CHILD_EXIT) と、
//   別スレッドへ切り替わったあとの再開。a0/a1 と mepc/mstatus/sp だけを戻し、ra・t*・a2-a7・s* は
//   0 に消す (取り上げられた callee や、切り替わる前のスレッドの値を見せない)。
//   gp/tp は触らない: caller stub が戻った直後に自分の値を入れ直すが、その 1 命令の間に
//   割込みが入っても IDF 側の ISR が使う gp が壊れないように、ここでは 0 にしない。
.Lrv32_exit_scrub:
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
#if defined(SHIZUKU_C6_UMODE)
  csrw mscratch, zero
#endif
  mret
// ---- 呼び先の入口 (RV32_TAG_ENTRY、素通し。CALL_METHOD / descriptor call / kobj への dispatch)。
//   引数 a0-a7 と ra (戻り口)・mepc (入口)・sp だけをカーネルが決める。s*/gp/tp は caller の値の
//   まま通す (カーネルの C は callee-saved を保ち、この経路では切替えが起きない)。t* にはカーネルの
//   値が残るが消さない (「レジスタは全部 caller からの入力」、callee は宣言した引数以外を信用しない)。
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
  lw a6, RV32_F_A6(sp)
  lw a7, RV32_F_A7(sp)
  addi sp, sp, RV32_FRAME_BYTES
#if defined(SHIZUKU_C6_UMODE)
  csrw mscratch, zero
#endif
  mret

// ---- U-mode 高速出口 (RV32_TAG_U_*)
#if defined(SHIZUKU_C6_UMODE)
//   復帰先が U-mode の場合、mscratch に trusted stack cursor を戻し、
//   sp に U-mode の元のスタックポインタ (RESERVED0) を戻して mret する。
.Lrv32_umode_exit_entry:
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
  lw a6, RV32_F_A6(sp)
  lw a7, RV32_F_A7(sp)
  lw t0, RV32_F_RESERVED1(sp)
  csrw mscratch, t0
  lw sp, RV32_F_RESERVED0(sp)
  mret

.Lrv32_umode_exit_ecall:
  lw t0, RV32_F_MEPC(sp)
  csrw mepc, t0
  lw t0, RV32_F_MSTATUS(sp)
  csrw mstatus, t0
  lw a0, RV32_F_A0(sp)
  lw a1, RV32_F_A1(sp)
  lw t0, RV32_F_RESERVED1(sp)
  csrw mscratch, t0
  lw sp, RV32_F_RESERVED0(sp)
  mret

.Lrv32_umode_exit_scrub:
  lw t0, RV32_F_MEPC(sp)
  csrw mepc, t0
  lw t0, RV32_F_MSTATUS(sp)
  csrw mstatus, t0
  lw a0, RV32_F_A0(sp)
  lw a1, RV32_F_A1(sp)
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
  lw t0, RV32_F_RESERVED1(sp)
  csrw mscratch, t0
  lw sp, RV32_F_RESERVED0(sp)
  mret
#endif
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
