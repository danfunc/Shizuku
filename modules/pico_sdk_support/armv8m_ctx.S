// ARMv8-M (Cortex-M33) の文脈退避・復帰。参照実装 (latest_ver_from_flight_robocon
// svc_asm_handler.S) の実機検証済みコードを移植したもの。シンボル名だけ
// shizuku_* に付け替えてある。
.syntax unified
.cpu cortex-m33
#if defined(__ARM_FP)
.fpu fpv5-sp-d16   // .cpu 指定は FPU を含まないため、vstmia/vldmia 用に明示する
#endif
.thumb

// context_t のオフセット。shizuku/archs/armv8m.hpp の static_assert と一致させること。
//   0..28 : r4-r11
//   32    : sp (PSP)
//   36    : exc_return (キャッシュした EXC_RETURN)
//   40    : fp[16] (S16-S31 退避域)
.equ CTX_EXC_RETURN, 36
.equ CTX_FP,         40
.equ CTX_PSPLIM,     104  // armv8m.hpp context_t::psplim と一致
.equ CTX_CONTROL,    108  // armv8m.hpp context_t::control と一致

// ---------------------------------------------------------------------------
// CTX_SAVE — 現在スレッドの context_t へ {r4-r11, PSP, EXC_RETURN, 遅延 S16-S31}
// を退避する。入口条件: lr = EXC_RETURN (例外入口直後)。出口: r0 = context 先頭。
// ---------------------------------------------------------------------------
.macro CTX_SAVE
    push {lr}                     // EXC_RETURN を退避 (直後の bl で lr が壊れる)
    bl   shizuku_current_context  // r0 = 現在スレッドの context 先頭

    // 整数 callee-saved と PSP を退避
    stmia r0!,{r4-r7}             // ctx[0..3] = r4-r7 ; r0 += 16
    mov  r1,r8
    mov  r2,r9
    mov  r3,r10
    mov  r4,r11
    mrs  r5,PSP
    stmia r0!,{r1-r5}             // ctx[4..8] = r8,r9,r10,r11,PSP ; r0 = base+36
    add  r0, #-36                 // r0 を context 先頭へ巻き戻す

    pop  {r1}                     // r1 = 退避しておいた EXC_RETURN
    str  r1,[r0, #CTX_EXC_RETURN] // ctx->exc_return = EXC_RETURN

#if defined(__ARM_FP)
    // EXC_RETURN bit4 == 0 なら拡張(FP)フレーム。S16-S31 を退避する。
    // (この vstmia 自体が FP 命令なので、遅延スタッキングされていた呼び出し元
    //  フレームの push を完了させる。)
    tst.w r1, #0x10
    bne  1f                       // bit4=1 → FP 無し。退避スキップ
    add.w r1, r0, #CTX_FP         // r1 = &ctx->fp[0]
    vstmia r1!, {s16-s31}
1:
#endif
.endm

// ---------------------------------------------------------------------------
// CTX_RESTORE — shizuku_current_context を再取得し (dispatch が別スレッドへ
// 切替えたかもしれない)、context_t から復元して例外復帰する。
// ---------------------------------------------------------------------------
.macro CTX_RESTORE
    bl   shizuku_current_context  // r0 = 復帰対象 context (別スレッドかもしれない)

    // 整数 callee-saved と PSP を復元
    ldmia r0!,{r4-r7}             // r4-r7 ; r0 += 16
    ldmia r0!,{r1-r3}             // r8,r9,r10 ; r0 += 12
    mov  r8,r1
    mov  r9,r2
    mov  r10,r3
    ldmia r0!,{r1-r2}             // r11,PSP ; r0 = base+36
    mov  r11,r1
    msr  PSP,r2

    ldr  r3,[r0]                  // r3 = ctx->exc_return (r0 は base+36)

    // スタック上限を復元 (スレッドごと不変なので restore のみ。PSP がここを下回ると
    // UsageFault→HardFault でスタックオーバーフローを即検出)。
    ldr  r1,[r0, #(CTX_PSPLIM - CTX_EXC_RETURN)]
    msr  PSPLIM, r1

    // CONTROL.nPRIV を復元。bit0(nPRIV) だけを ctx->control の値へ差し替える
    // read-modify-write — MRS で読んだ「現在の」CONTROL を土台にすることで、
    // bit1(SPSEL, 常に1で不変) と bit2(FPCA, ハードウェアが遅延 FPU 文脈で管理) を
    // 絶対に壊さない。r0/r3 (context 先頭+36 / exc_return) は温存必須。
    ldr  r1,[r0, #(CTX_CONTROL - CTX_EXC_RETURN)]
    mrs  r2, CONTROL
    bic  r2, r2, #1
    and  r1, r1, #1
    orr  r2, r2, r1
    msr  CONTROL, r2
    isb

#if defined(__ARM_FP)
    // 復帰対象が拡張(FP)フレームなら S16-S31 を復元する。
    tst.w r3, #0x10
    bne  2f                       // bit4=1 → FP 無し。復元スキップ
    add  r0, #4                   // r0 = base+40 = &ctx->fp[0]
    vldmia r0!, {s16-s31}
2:
#endif

    mov  lr, r3                   // LR = 復帰対象の EXC_RETURN
    bx   lr                       // 例外復帰
.endm

// ---------------------------------------------------------------------------
// SVC 入口。CTX_SAVE → shizuku_svc_dispatch(context) → CTX_RESTORE。
// ---------------------------------------------------------------------------
.thumb_func
.global shizuku_armv8m_svc_entry
.align 8
.type shizuku_armv8m_svc_entry, %function
shizuku_armv8m_svc_entry:
    CTX_SAVE
    bl   shizuku_svc_dispatch     // shizuku_svc_dispatch(context = r0)
    CTX_RESTORE

// ---------------------------------------------------------------------------
// PendSV 入口 (時限実行権の期限回収 = 最低優先度の遅延切替例外)。退避/復元は
// SVC と完全同型 (遅延 FPU 退避含む)。
// ---------------------------------------------------------------------------
.thumb_func
.global shizuku_armv8m_pendsv_entry
.align 2
.type shizuku_armv8m_pendsv_entry, %function
shizuku_armv8m_pendsv_entry:
    CTX_SAVE
    bl   shizuku_pendsv_dispatch  // shizuku_pendsv_dispatch(context = r0)
    CTX_RESTORE

// ---------------------------------------------------------------------------
// 戻り口 — 呼び先が普通に return するとここへ落ちる (カーネルが lr に載せる)。
// 自分の活性化を 1 枚ぶん巻き戻すだけ。
//   r0 = 呼び先の戻り値 (C の return 値がそのまま乗っている)
// ★ネスト数の申告は 0 (= 申告しない)。この経路の発行者はカーネル自身で、落とす枚数も
//   カーネルが決めた 1 枚なので、両側チェックの対象になる「発行側の思い違い」が
//   原理的に存在しない。申告が意味を持つのはオブジェクト側が段数を指定して巻き戻す
//   とき (exit の 2 枚 pop や異常系の連鎖畳み) で、そちらは必ず申告する。
//   ここで callee-saved レジスタ (r7 等) を読まないのも重要 — 呼び先の C 関数が
//   フレームポインタとして潰しているので、値を信用できない。
// ---------------------------------------------------------------------------
.thumb_func
.global shizuku_armv8m_return_stub
.align 2
.type shizuku_armv8m_return_stub, %function
shizuku_armv8m_return_stub:
    mov   r2, r0        // a2 = 戻り値
    movs  r1, #1        // a1 = 落とす段数 1
    movs  r3, #0        // a3 = エラーコード 0 (成功)
    mov.w r12, #0       // a4 = ネスト数の申告なし
    movs  r0, #2        // a0 = RETURN (kernel_abi.hpp と static_assert で両縛り)
    svc   0
    b     shizuku_return_stub_failed // 戻ってきた = 巻き戻しが弾かれた

// ---------------------------------------------------------------------------
// ブート時にスレッドスタック (PSP) へ移り、entry を呼ぶ。
//   r0 = スタック先頭 (高位), r1 = スタック下限, r2 = entry
// SP を差し替える処理をコンパイラに任せると、切替後に SP 相対の局所変数へ触って
// 壊れ得る。ここは全部 asm で書いて確定させる。entry が万一 return しても、
// blx の lr がすぐ下の無限ループを指すので暴走しない。
// ---------------------------------------------------------------------------
.thumb_func
.global shizuku_armv8m_enter_thread_mode
.align 2
.type shizuku_armv8m_enter_thread_mode, %function
shizuku_armv8m_enter_thread_mode:
    msr  PSPLIM, r1
    msr  PSP, r0
    movs r3, #2         // CONTROL: SPSEL=1 (スレッドは PSP), nPRIV=0 (特権)
    msr  CONTROL, r3
    isb
    blx  r2
1:  b    1b
