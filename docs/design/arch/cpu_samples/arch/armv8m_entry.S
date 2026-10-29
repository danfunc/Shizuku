// サンプル: ARMv8-M Mainline (Cortex-M33 / RP2350) の例外入口。全部でこれだけ。
//
// 前提:
//   - スレッドは常に PSP で走る (CONTROL.SPSEL=1)。カーネルは MSP で走る。
//   - SVC はスレッドモードからしか撃たない。よってフレームは常に PSP 上にある。
//   - 切替えは PendSV (最低優先度) だけで起こす。SVC/SysTick/フォールトは pend を立てるだけ。
// 診断用の計測点は置かない。計測は C 側の DIAG 型 (common/diag.hpp) で行う。
.syntax unified
.cpu cortex-m33
.fpu fpv5-sp-d16
.thumb

// ---------------------------------------------------------------------------
// SVC: r0 = 例外フレーム (PSP) を渡して C を 1 回呼ぶだけ。
// r4-r11 は C (AAPCS の callee-saved) が守るので触らない。
// push {r4, lr} は MSP の 8B 整列を保つためと、EXC_RETURN を守るため。
// ---------------------------------------------------------------------------
.thumb_func
.global sample_armv8m_svc_entry
sample_armv8m_svc_entry:
    mrs   r0, psp
    push  {r4, lr}
    bl    shizuku_svc
    pop   {r4, pc}              // pc <- EXC_RETURN: 例外復帰

// ---------------------------------------------------------------------------
// PendSV: ここだけが文脈を切り替える。
//   退避: [FP: s16-s31 (使っていれば)] [r4-r11] を現スレッドの PSP 側へ積む。
//   EXC_RETURN はスタックに置かず C へ渡す (kernel_entry.hpp の注意を参照)。
//   C (shizuku_switch) は CONTROL.nPRIV / PSPLIM / MPU の窓を次のスレッド用に書いてから戻る。
// ---------------------------------------------------------------------------
.thumb_func
.global sample_armv8m_pendsv_entry
sample_armv8m_pendsv_entry:
    mrs   r0, psp
    tst   lr, #0x10             // EXC_RETURN.FType == 0 なら FP 拡張フレーム
    it    eq
    vstmdbeq r0!, {s16-s31}
    stmdb r0!, {r4-r11}
    mov   r1, lr
    bl    shizuku_switch        // r0 = 次の sp, r1 = 次の EXC_RETURN
    ldmia r0!, {r4-r11}
    tst   r1, #0x10
    it    eq
    vldmiaeq r0!, {s16-s31}
    msr   psp, r0
    bx    r1

// ---------------------------------------------------------------------------
// フォールト (MemManage / BusFault / UsageFault / HardFault 共通)。
// 落ちた側がスレッドなら PSP、ハンドラなら MSP にフレームがある (EXC_RETURN.SPSEL)。
// ---------------------------------------------------------------------------
.thumb_func
.global sample_armv8m_fault_entry
sample_armv8m_fault_entry:
    tst   lr, #0x4
    ite   eq
    mrseq r0, msp
    mrsne r0, psp
    mov   r1, lr
    push  {r4, lr}
    bl    shizuku_fault
    pop   {r4, pc}

// SysTick は asm 不要。Cortex-M の例外入口は AAPCS 互換なので、
// ベクタへ C 関数 shizuku_tick を直接置く (arch/armv8m.hpp の install_vectors)。

// ---------------------------------------------------------------------------
// 起動: スレッドスタックへ移って entry を呼ぶ (戻らない)。
//   r0 = スタック先頭, r1 = スタック下限, r2 = entry
// ---------------------------------------------------------------------------
.thumb_func
.global sample_armv8m_enter_thread_mode
sample_armv8m_enter_thread_mode:
    msr   psplim, r1
    msr   psp, r0
    movs  r3, #2                // SPSEL=1, nPRIV=0
    msr   control, r3
    isb
    blx   r2
1:  b     1b
