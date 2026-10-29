// サンプル: ARMv6-M (Cortex-M0+ / RP2040) の例外入口。
// 形は armv8m_entry.S と同じ。違いは Thumb-1 の制約 (stm/ldm は r0-r7 だけ、
// PSPLIM・FP・設定可能なフォールト例外が無い) から来るものだけ。
.syntax unified
.cpu cortex-m0plus
.thumb

.thumb_func
.global sample_armv6m_svc_entry
sample_armv6m_svc_entry:
    mrs   r0, psp
    push  {r4, lr}
    bl    shizuku_svc
    pop   {r4, pc}

// ---------------------------------------------------------------------------
// PendSV: [r4-r7][r8-r11] の 32B を PSP 側へ積む。stmdb が無いので先に sp を下げる。
// ---------------------------------------------------------------------------
.thumb_func
.global sample_armv6m_pendsv_entry
sample_armv6m_pendsv_entry:
    mrs   r0, psp
    subs  r0, #32
    mov   r2, r0
    stmia r2!, {r4-r7}
    mov   r4, r8
    mov   r5, r9
    mov   r6, r10
    mov   r7, r11
    stmia r2!, {r4-r7}
    mov   r1, lr
    bl    shizuku_switch        // r0 = 次の sp, r1 = 次の EXC_RETURN
    adds  r0, #16
    ldmia r0!, {r4-r7}          // 上半分 = r8-r11
    mov   r8, r4
    mov   r9, r5
    mov   r10, r6
    mov   r11, r7
    msr   psp, r0               // sp = 例外フレームの先頭
    subs  r0, #32
    ldmia r0!, {r4-r7}
    bx    r1

// ARMv6-M には HardFault しか無い (優先度 -1 固定)。
.thumb_func
.global sample_armv6m_fault_entry
sample_armv6m_fault_entry:
    mov   r1, lr
    movs  r2, #4
    tst   r1, r2
    bne   1f
    mrs   r0, msp
    b     2f
1:  mrs   r0, psp
2:  push  {r4, lr}
    bl    shizuku_fault
    pop   {r4, pc}

.thumb_func
.global sample_armv6m_enter_thread_mode
sample_armv6m_enter_thread_mode:
    msr   psp, r0               // r1 (下限) は使わない: v6-M に PSPLIM は無い
    movs  r3, #2
    msr   control, r3
    isb
    blx   r2
1:  b     1b
