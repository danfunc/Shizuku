// サンプル: RV32 (ESP32-C6, M/U モード) の trap 入口。mtvec は direct mode (1 本) を想定。
//
// ARM との違いはハードウェアが何も積まないことだけ。代わりに:
//   - mscratch には常に「今のスレッドの文脈ブロック (カーネルのメモリ)」を指させておく。
//     入口で sp と入れ替えれば、スレッドのスタックに一切触れずに退避先が手に入る。
//     (非特権スレッドのスタックは信用できないので、ここに退避しない)
//   - ECALL の速い経路は a0-a7 をそのまま C の引数にし、a0/a1 をそのまま返す。
//     s0-s11 は C が守る。t*/a2-a7/ra は呼び手の converter が clobber 宣言する。
//   - 切替えは trap の出口で pend を見てから 1 か所で行う (ARM の PendSV に相当)。
.option norvc

.equ CTX_RA,      0*4       // 文脈ブロック: x1..x31 を x 番号 - 1 の位置に置く
.equ CTX_SP,      1*4
.equ CTX_T0,      4*4
.equ CTX_S0,      7*4
.equ CTX_S1,      8*4
.equ CTX_A0,      9*4
.equ CTX_A1,     10*4
.equ CTX_S2,     17*4
.equ CTX_MEPC,   31*4
.equ CTX_MSTATUS,32*4

.macro SAVE_CALLEE_SAVED      // s0-s11
    sw    s0,  CTX_S0(sp)
    sw    s1,  CTX_S1(sp)
    sw    s2,  (CTX_S2+0*4)(sp)
    sw    s3,  (CTX_S2+1*4)(sp)
    sw    s4,  (CTX_S2+2*4)(sp)
    sw    s5,  (CTX_S2+3*4)(sp)
    sw    s6,  (CTX_S2+4*4)(sp)
    sw    s7,  (CTX_S2+5*4)(sp)
    sw    s8,  (CTX_S2+6*4)(sp)
    sw    s9,  (CTX_S2+7*4)(sp)
    sw    s10, (CTX_S2+8*4)(sp)
    sw    s11, (CTX_S2+9*4)(sp)
.endm

// x1..x31 (sp=x2 を除く) を ctx へ / ctx から。
.macro SAVE_ALL_BUT_SP
    .irp n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    sw    x\n, (\n-1)*4(sp)
    .endr
.endm
.macro LOAD_ALL_BUT_SP
    .irp n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    lw    x\n, (\n-1)*4(sp)
    .endr
.endm

.balign 4
.global sample_rv32_trap_entry
sample_rv32_trap_entry:
    csrrw sp, mscratch, sp      // sp = ctx, mscratch = スレッドの sp
    sw    t0, CTX_T0(sp)
    csrr  t0, mcause
    bltz  t0, .Lslow            // 割込み
    addi  t0, t0, -8            // 8 = U-mode ECALL, 11 = M-mode ECALL
    beqz  t0, .Lecall
    addi  t0, t0, -3
    bnez  t0, .Lslow            // それ以外の例外 (フォールト)

// ---- ECALL の速い経路 ------------------------------------------------------
.Lecall:
    csrr  t0, mscratch
    sw    t0, CTX_SP(sp)        // スレッドの sp (in-place call では C が書き換える)
    csrw  mscratch, sp          // mscratch を ctx に戻す
    csrr  t0, mepc
    addi  t0, t0, 4             // ECALL は常に 4 バイト
    sw    t0, CTX_MEPC(sp)
    csrr  t0, mstatus
    sw    t0, CTX_MSTATUS(sp)
    sw    s1, CTX_S1(sp)        // スレッドの s1 を退避してから
    mv    s1, sp                // s1 = ctx として使う (C の呼出しをまたいで残る)
    la    sp, sample_rv32_kernel_stack_top
    mv    a7, s1                // 8 番目の引数 = ctx (frame)。番号と引数は a0-a6
    call  shizuku_svc_rv        // a0/a1 = 結果
    mv    sp, s1
    lw    s1, CTX_S1(sp)
    lw    t0, sample_rv32_pend
    bnez  t0, .Lswitch_after_ecall
    lw    t0, CTX_MEPC(sp)
    csrw  mepc, t0
    lw    t0, CTX_MSTATUS(sp)
    csrw  mstatus, t0
    lw    sp, CTX_SP(sp)
    mret

// ECALL のあとで切替えが要る: 結果 a0/a1 と s0-s11 を ctx へ入れてから切り替える。
.Lswitch_after_ecall:
    sw    a0, CTX_A0(sp)
    sw    a1, CTX_A1(sp)
    SAVE_CALLEE_SAVED
    j     .Lswitch

// ---- 割込み・フォールト: 全レジスタを ctx へ --------------------------------
.Lslow:
    lw    t0, CTX_T0(sp)        // t0 を元に戻してから全部まとめて退避
    SAVE_ALL_BUT_SP
    csrr  t0, mscratch
    sw    t0, CTX_SP(sp)
    csrw  mscratch, sp
    csrr  t0, mepc
    sw    t0, CTX_MEPC(sp)
    csrr  t0, mstatus
    sw    t0, CTX_MSTATUS(sp)
    mv    s1, sp
    la    sp, sample_rv32_kernel_stack_top
    mv    a0, s1
    csrr  a1, mcause
    call  shizuku_trap_rv       // 割込みの配送 / フォールト処理。pend を立てることがある
    mv    sp, s1
    lw    t0, sample_rv32_pend
    beqz  t0, .Lresume

// ---- 切替え (ARM の PendSV に相当する唯一の場所) ----------------------------
.Lswitch:
    sw    zero, sample_rv32_pend, t0
    mv    a0, sp                // 退避済みの ctx
    li    a1, 0
    la    sp, sample_rv32_kernel_stack_top
    call  shizuku_switch        // a0 = 次の ctx (PMP と mstatus.MPP は C が設定済み)
    mv    sp, a0
    csrw  mscratch, sp

// ---- 全部戻して mret ------------------------------------------------------
// 初回の起動も同じ道を通る (rv32.hpp の enter_thread_mode が sp = ctx で飛んでくる)。
.global sample_rv32_resume_first
sample_rv32_resume_first:
.Lresume:
    lw    t0, CTX_MEPC(sp)
    csrw  mepc, t0
    lw    t0, CTX_MSTATUS(sp)
    csrw  mstatus, t0
    LOAD_ALL_BUT_SP
    lw    sp, CTX_SP(sp)
    mret
