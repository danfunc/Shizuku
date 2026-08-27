#ifndef SHIZUKU_ARCHS_ARMV8M_HPP
#define SHIZUKU_ARCHS_ARMV8M_HPP
#include <cstddef>
#include <cstdint>
#include "hardware/structs/mpu.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "shizuku/concepts/arch.hpp"
#include "shizuku/kernel_abi.hpp"

// ARMv8-M mainline (Cortex-M33 / RP2350) の arch バックエンド。
// 文脈退避・復帰・戻り口・スレッドモード移行の実体は armv8m_ctx.S。
// レイアウトと退避規約は参照実装 (latest_ver_from_flight_robocon の
// svc_asm_handler.S / kernel.hpp context_t) の実機検証済みの形をそのまま移植した。

extern "C" {
// armv8m_ctx.S の例外入口。board 層がベクタへ登録する。
void shizuku_armv8m_svc_entry();
void shizuku_armv8m_pendsv_entry();
// タイマ例外の入口。文脈は触らないので普通の C 関数でよい (切替を起票するだけ)。
void shizuku_armv8m_systick_entry();
// フォールト入口 (退避 → 判断 → 復帰。普通の例外と同じ経路)。
void shizuku_armv8m_fault_entry();
struct shizuku_armv8m_context;
void shizuku_fault_dispatch(shizuku_armv8m_context *context);
// 呼び先が普通に return したときの戻り口 (RETURN プリミティブを 1 段ぶん発行)。
void shizuku_armv8m_return_stub();
// スレッドスタック (PSP) へ移って entry を呼ぶ。戻らない。
void shizuku_armv8m_debugmon_entry();
[[noreturn]] void shizuku_armv8m_enter_thread_mode(uintptr_t stack_top,
                                                   uintptr_t stack_limit,
                                                   void (*entry)());
// asm から呼ばれるフック。実体は modules/pico_sdk_support/arch_glue.cpp。
struct shizuku_armv8m_context;
shizuku_armv8m_context *shizuku_current_context();
void shizuku_svc_dispatch(shizuku_armv8m_context *context);
void shizuku_pendsv_dispatch(shizuku_armv8m_context *context);
}

namespace shizuku {
namespace archs {

class armv8m {
public:
  struct exception_frame_t {
    uint32_t r0, r1, r2, r3, r12, lr, pc, xPSR;
  };
  // CONTROL レジスタのキャッシュ値 (bit1=SPSEL=1 固定: スレッドは常に PSP、
  // bit0=nPRIV)。bit2(FPCA) はハードウェア管理なので、CTX_RESTORE は MRS で読んだ
  // 現在値の bit0 だけをこの値で置き換える read-modify-write を行う。
  static constexpr uint32_t CONTROL_PRIV_PSP = 0b10;   // nPRIV=0 (特権)
  static constexpr uint32_t CONTROL_UNPRIV_PSP = 0b11; // nPRIV=1 (非特権)
  // 呼び出しフレームを積むときスタック下限の手前に残す余白。
  // ★これは「なんとなくの安全マージン」ではない。**呼び先が最低 1 回はカーネルを
  //   呼び返せる**ことを保証する量でなければならない — 呼び先は戻るためにも
  //   syscall を撃つので、ここが足りないと「戻ることすらできない」状態が作れて
  //   しまう。内訳 (実測): 例外フレーム 32 + 退避域 (ヘッダ 124 + フレーム 32) +
  //   カーネルオブジェクトのハンドラ連鎖の C フレーム約 120 = 約 310。
  //   足りないと PSPLIM の UsageFault が先に出て無言で死ぬ。
  static constexpr uint32_t CALL_HEADROOM = 512;

  // armv8m_ctx.S の .equ (CTX_*) と一致させること。下の static_assert が両縛りする。
  struct context_t {
    uint32_t r4 = 0, r5 = 0, r6 = 0, r7 = 0;   // offset 0..12
    uint32_t r8 = 0, r9 = 0, r10 = 0, r11 = 0; // offset 16..28
    exception_frame_t *sp = nullptr;           // offset 32
    // キャッシュした EXC_RETURN。bit4=0 なら拡張 (FP) フレーム。新規スレッドは
    // 基本フレームの Thread/PSP へ復帰する (0 のままだと初回復帰で即 HardFault)。
    uint32_t exc_return = 0xFFFFFFFD;    // offset 36
    uint32_t fp[16] = {};                // offset 40 (S16-S31 退避域)
    uint32_t psplim = 0;                 // offset 104 (0 = 制限なし)
    uint32_t control = CONTROL_PRIV_PSP; // offset 108
    // ★軸 B (Q8)。control と同じ扱い: ここは値を持つだけで、実際に MPU へ
    //   書くのは CTX_RESTORE 直前の shizuku_restore_region_window (offset は
    //   asm から触らないので .equ は要らない)。0/0 = 窓なし。
    uint32_t region_base = 0;  // offset 112
    uint32_t region_limit = 0; // offset 116
  };
  // メソッド ABI (引数 4 本 + r12)。
  using method_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                 uintptr_t);

  // ---- 例外フレームの幾何 -------------------------------------------------
  // 基本 32B / FP 拡張 104B。EXC_RETURN bit4 で判別する。
  static uint32_t exc_frame_bytes(const context_t &context) {
    return (context.exc_return & 0x10u) ? 32u : 104u;
  }
  // 例外復帰「後」のスレッド SP。xPSR bit9 (例外エントリ時の 8B 整列パディング) が
  // 立っていると、ハードウェアは復帰時に SP へ +4 余分に足す。ここで吸収しないと
  // 呼び出しフレームの退避域を踏み潰す (I-4。実機で実際に踏んだ落とし穴)。
  static uintptr_t psp_after_return(const context_t &context) {
    return (uintptr_t)context.sp + exc_frame_bytes(context) +
           ((context.sp->xPSR & (1u << 9)) ? 4u : 0u);
  }
  // カーネルが 8B 境界へ置き直した作業コピーには追加調整を入れさせない。
  static void normalize_frame(exception_frame_t &frame) {
    frame.xPSR &= ~(uint32_t)(1u << 9);
  }

  // ---- syscall ABI のスロット (a0..a4 = r0,r1,r2,r3,r12) -------------------
  static uintptr_t arg(const exception_frame_t &frame, unsigned index) {
    switch (index) {
    case 0:
      return frame.r0;
    case 1:
      return frame.r1;
    case 2:
      return frame.r2;
    case 3:
      return frame.r3;
    default:
      return frame.r12;
    }
  }
  static void set_args(exception_frame_t &frame, const uintptr_t *args) {
    frame.r0 = (uint32_t)args[0];
    frame.r1 = (uint32_t)args[1];
    frame.r2 = (uint32_t)args[2];
    frame.r3 = (uint32_t)args[3];
  }
  static void set_result(exception_frame_t &frame, uintptr_t error,
                         uintptr_t value) {
    frame.r0 = (uint32_t)error;
    frame.r1 = (uint32_t)value;
  }
  static void set_entry(exception_frame_t &frame, uintptr_t pc, uintptr_t lr) {
    frame.pc = (uint32_t)pc;
    frame.lr = (uint32_t)lr;
  }
  // 戻り口 (1 本だけ)。呼び先が普通に return するとここへ落ち、RETURN を撃つ。
  // ハンドラから出ればプリミティブとして巻き戻し、オブジェクトから出れば経路判定で
  // オブジェクトランドのハンドラへ「戻った」という知らせとして届く。
  static uintptr_t return_stub() {
    return (uintptr_t)&shizuku_armv8m_return_stub;
  }
  // ---- スレッドの最初の 1 回 -----------------------------------------------
  // 「例外から復帰してきた」ように見せかけた文脈を組み立てる。これで最初の実行も
  // 普通の復帰経路 (CTX_RESTORE) に一本化でき、入口だけ別扱いにならずに済む。
  static void prepare_thread_entry(context_t &context, uintptr_t stack_top,
                                   uintptr_t entry_pc, uintptr_t return_pc,
                                   uintptr_t argument) {
    exception_frame_t *frame =
        (exception_frame_t *)((stack_top - sizeof(exception_frame_t)) &
                              ~(uintptr_t)7);
    frame->r0 = (uint32_t)argument;
    frame->r1 = frame->r2 = frame->r3 = frame->r12 = 0;
    frame->lr = (uint32_t)return_pc;
    frame->pc = (uint32_t)entry_pc;
    frame->xPSR = (1u << 24); // Thumb ビット。整列パディングは無し
    context.sp = frame;
    // 基本フレーム (FP 無し) の Thread/PSP へ復帰する。0 のままだと初回復帰で
    // bx 0 になって即 HardFault するので、必ず種を入れる。
    context.exc_return = 0xFFFFFFFD;
  }

  // ---- 時限つき実行権のためのタイマと遅延切替 --------------------------------
  // SysTick は per-core banked。24bit なので遠い期限は上位が刻んで継ぎ足す。
  static constexpr uint32_t TIMER_MAX_CYCLES = 0x00FFFFFF;
  static constexpr uint32_t TIMER_MIN_CYCLES = 100; // 装填直後発火を避ける下限
  static void timer_oneshot(uint32_t cycles) {
    systick_hw->rvr = cycles;
    systick_hw->cvr = 0;   // 書き込みでカウンタをクリア (rvr から数え直す)
    systick_hw->csr = 0x7; // ENABLE | TICKINT | プロセッサクロック
  }
  static void timer_cancel() { systick_hw->csr = 0; }

  // ---- ★自己ホスト型デバッグ (DebugMonitor) --------------------------------
  //  halting debug (SWD のプローブ) はコアを丸ごと止めるが、DebugMonitor は
  //  **優先度が設定できる普通の例外**。だから「ブレークポイントでそのスレッドだけ
  //  止め、他は走り続ける」ができる — I-9 とまっすぐ噛み合う。
  //  ★プローブが繋がっている間は MON_EN が効かない (C_DEBUGEN が勝つ)。両方は
  //    同時に使えないので、使う側がどちらかを選ぶ。
  static constexpr uintptr_t DEMCR_ADDRESS = 0xE000EDFCu;
  static constexpr uintptr_t DFSR_ADDRESS = 0xE000ED30u;
  static constexpr uintptr_t FP_CTRL_ADDRESS = 0xE0002000u;
  static constexpr uintptr_t FP_COMP_ADDRESS = 0xE0002008u;
  static constexpr uint32_t DEMCR_TRCENA = 1u << 24;
  static constexpr uint32_t DEMCR_MON_REQ = 1u << 19;
  static constexpr uint32_t DEMCR_MON_STEP = 1u << 18;
  static constexpr uint32_t DEMCR_MON_PEND = 1u << 17;
  static constexpr uint32_t DEMCR_MON_EN = 1u << 16;
  // DFSR: bit0 HALTED (1 命令実行の完了もこれ), bit1 BKPT, bit2 DWTTRAP
  static constexpr uint32_t DFSR_HALTED = 1u << 0;
  static constexpr uint32_t DFSR_BKPT = 1u << 1;
  static constexpr uint32_t DFSR_DWTTRAP = 1u << 2;

  static volatile uint32_t &at(uintptr_t address) {
    return *(volatile uint32_t *)address;
  }

  static void debug_enable(bool on) {
    uint32_t demcr = at(DEMCR_ADDRESS);
    // TRCENA は DWT/FPB を動かすのに要る (デバッグ機能全体の元栓)。
    demcr = on ? (demcr | DEMCR_TRCENA | DEMCR_MON_EN)
               : (demcr & ~DEMCR_MON_EN);
    at(DEMCR_ADDRESS) = demcr;
    __asm__ volatile("dsb\n\tisb" ::: "memory");
  }
  // 有効になったか。★**プローブが繋がっていると MON_EN は立たない**ので、
  //   「立てた」ではなく「立ったか」を読んで確かめる。
  static bool debug_enabled() {
    return (at(DEMCR_ADDRESS) & DEMCR_MON_EN) != 0;
  }
  // 次の 1 命令だけ実行して、また DebugMon へ戻ってくるようにする。
  static void debug_step(bool on) {
    uint32_t demcr = at(DEMCR_ADDRESS);
    at(DEMCR_ADDRESS) = on ? (demcr | DEMCR_MON_STEP) : (demcr & ~DEMCR_MON_STEP);
    // ★★書いたら **dsb + isb** で効かせる。入れないと DEMCR への書き込みが後ろへ
    //   ずれ、「次の命令」ではなく数命令あとで止まる。実測で踏んだ: 直後に置いた
    //   nop では止まらず、その先で止まっていた (「事象は起きているのに狙った場所
    //   ではない」という一番読み違えやすい形で出る)。
    __asm__ volatile("dsb\n\tisb" ::: "memory");
  }
  // なぜ止まったか (DFSR)。★読んだら**書き戻して消す** — 消さないと次の判定に
  //   古い理由が混ざる (フォールトの CFSR で同じ罠を踏んでいる)。
  static uint32_t debug_reason_take() {
    const uint32_t reason = at(DFSR_ADDRESS);
    at(DFSR_ADDRESS) = reason; // 1 を書いたビットが消える
    return reason;
  }
  // ハードウェアブレークポイントの数。★実装依存なので**実行時に読む** (データ
  //   シートの数字を信じない)。FP_CTRL の NUM_CODE は 2 か所に分かれている。
  static uint32_t breakpoint_count() {
    const uint32_t control = at(FP_CTRL_ADDRESS);
    return (((control >> 12) & 0x7u) << 4) | ((control >> 4) & 0xFu);
  }
  static void breakpoint_enable(bool on) {
    // bit1 (KEY) を一緒に書かないと ENABLE の変更が無視される。
    at(FP_CTRL_ADDRESS) = (on ? 0x3u : 0x2u);
  }
  static void breakpoint_set(uint32_t index, uintptr_t address) {
    // FPBv2: [31:1] が比較するアドレス、bit0 が有効。
    at(FP_COMP_ADDRESS + 4u * index) = ((uint32_t)address & ~1u) | 1u;
    __asm__ volatile("dsb\n\tisb" ::: "memory");
  }
  static void breakpoint_clear(uint32_t index) {
    at(FP_COMP_ADDRESS + 4u * index) = 0;
    // ★breakpoint_set と同じ理由で要る。無いと「解除した」つもりの後も
    //   古い比較器の値がしばらく効いたままになり得る (実測で踏んだ:
    //   detach で resume したはずの相手が、次に繋いだときに同じ
    //   ブレークポイントの pc で止まったまま止まっていた)。
    __asm__ volatile("dsb\n\tisb" ::: "memory");
  }
  // 今の刻みで**まだ残っている**サイクル数。wrapped は「刻みを撃ち切った」印
  // (COUNTFLAG)。★svc は SysTick より優先度が高いので、発火が保留されたまま
  //   ここへ来ることがある。そのときカウンタは既に折り返して数え直しているので、
  //   残りをそのまま信じると**使った時間を取りこぼす**。折り返していたら
  //   「刻みは撃ち切った」と見なす (足りなく数えるより多く数えるほうが安全 —
  //   貸した実行権が予定より長く握られる側に倒れない)。
  static uint32_t timer_remaining(bool &wrapped) {
    const uint32_t csr = systick_hw->csr; // ★読むと COUNTFLAG は落ちる。1 回だけ読む
    wrapped = (csr & (1u << 16)) != 0;
    if (!(csr & 0x1u)) { // 動いていない
      wrapped = false;
      return 0;
    }
    return systick_hw->cvr & 0x00FFFFFFu;
  }
  // ★切替は最低優先度の遅延例外でしか起こさない (DESIGN §14.5.1 の規約)。
  //   これにより「syscall ハンドラの中にいる = そのコアでは切り替わらない」が
  //   取得コストゼロで手に入る。
  static void pend_context_switch() {
    scb_hw->icsr = 1u << 28; // PENDSVSET
  }

  // ---- メモリ保護 (機構だけ。どこに何を張るかは board の知識) ----------------
  // ★PMSAv8 は有効 region の重なりを許さない。「広い RW に穴を開ける」ことが
  //   できないので、配置で解くしかない (参照実装が PMSAv7 の知識を捨てろと
  //   書いているのはこの点)。base/limit は 32B 粒度で limit は内包。
  static constexpr uint32_t ACCESS_RW_ALL = 0b01; // 特権/非特権とも読み書き
  static constexpr uint32_t ACCESS_RO_ALL = 0b11; // 特権/非特権とも読みだけ
  static void region_set(uint32_t index, uintptr_t base, uintptr_t limit,
                         uint32_t access, bool execute_never,
                         uint32_t attribute) {
    mpu_hw->rnr = index;
    mpu_hw->rbar =
        ((uint32_t)base & ~0x1Fu) | (access << 1) | (execute_never ? 1u : 0u);
    mpu_hw->rlar = ((uint32_t)limit & ~0x1Fu) | (attribute << 1) | 1u;
  }
  static void region_disable(uint32_t index) {
    mpu_hw->rnr = index;
    mpu_hw->rlar = 0;
  }
  // attr0 = 通常メモリ (書き戻し), attr1 = 通常メモリ (キャッシュしない)。
  static void protection_enable() {
    mpu_hw->mair[0] = 0xFFu | (0x44u << 8);
    // ★PRIVDEFENA: region で明示していない場所は「特権だけが触れる」。これが
    //   単一アドレス空間における「カーネル空間」の実体で、MMU が無くても
    //   非特権から見えない領域を作れる (DESIGN §11.1)。
    mpu_hw->ctrl = M33_MPU_CTRL_PRIVDEFENA_BITS | M33_MPU_CTRL_ENABLE_BITS;
    asm volatile("dsb\nisb" ::: "memory");
  }
  static void protection_disable() {
    mpu_hw->ctrl = 0;
    asm volatile("dsb\nisb" ::: "memory");
  }
  // 今の実行の CONTROL をそのまま返す。**状態は対象自身に申告させる** ための口
  // (DESIGN §16 / §11.2.0: 「非特権で動いた」は自己申告なしには言えない)。
  // 落ちたのがスレッドモードか (= そのスレッドを止めれば復帰できるか)。
  // ハンドラモードで落ちたならカーネル自身の破れなので、止めても直らない。
  static bool faulted_in_thread_mode(const context_t &context) {
    return (context.exc_return & 0x4u) != 0;
  }
  // 落ちた場所と、触ろうとした先。★診断は「読み出す場所を間違えると静かに嘘を
  //   吐く」ので、引数スロットの流用ではなく専用の口にする (実際 arg(6) は
  //   引数の既定枝に落ちて r12 を返しており、pc が 0 に見えていた)。
  static uintptr_t frame_pc(const exception_frame_t &frame) { return frame.pc; }
  static uintptr_t frame_lr(const exception_frame_t &frame) { return frame.lr; }
  static uintptr_t fault_address() { return *(volatile uint32_t *)0xE000ED34u; }
  // どの違反だったか (CFSR)。読んだら書き戻して消す (sticky なので残る)。
  static uint32_t fault_status() { return *(volatile uint32_t *)0xE000ED28u; }
  static void fault_status_clear() {
    *(volatile uint32_t *)0xE000ED28u = fault_status();
  }
  static uint32_t control_register() {
    uint32_t control;
    asm volatile("MRS %0, CONTROL" : "=r"(control));
    return control;
  }

  // ---- 特権とスタック上限 -------------------------------------------------
  // 「今の実行が特権か」の自己申告 (CONTROL.nPRIV を実際に読む)。
  static bool current_priv() {
    uint32_t control;
    asm volatile("MRS %0, CONTROL" : "=r"(control));
    return (control & 1u) == 0;
  }
  static void set_priv(context_t &context, bool privileged) {
    context.control = privileged ? CONTROL_PRIV_PSP : CONTROL_UNPRIV_PSP;
  }
  // ★set_priv と同じ形 (Q8 / DESIGN §11.3)。ここでは値を持たせるだけで、
  //   実際に MPU region へ書くのは復帰の直前 (shizuku_restore_region_window)。
  //   limit==0 は「窓なし」。
  static void set_region_window(context_t &context, uintptr_t base,
                                uintptr_t limit) {
    context.region_base = (uint32_t)base;
    context.region_limit = (uint32_t)limit;
  }
  // GRANT_REGION が使う MPU region。0/1 は固定 (board.cpp)、2
  // 以降は空いている。
  static constexpr uint32_t GRANT_REGION_INDEX = 2;
  static void stack_limit_set(context_t &context, uintptr_t limit) {
    context.psplim = (uint32_t)limit;
  }
  static uintptr_t stack_limit(const context_t &context) {
    return context.psplim;
  }

  // ---- ★ARCH SEAM — マルチコア atomic (DESIGN §14.5.3) --------------------
  // RP2350 (ARMv8-M) は LDREX/STREX (= __atomic 系)。SIO ハードウェアスピンロックは
  // E2 erratum で使えない。RP2040 (ARMv6-M) では逆に LDREX/STREX が無い。
  static bool cas32(volatile uint32_t *address, uint32_t expected,
                    uint32_t desired) {
    return __atomic_compare_exchange_n((uint32_t *)address, &expected, desired,
                                       false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
  }
  static void store_release32(volatile uint32_t *address, uint32_t value) {
    __atomic_store_n((uint32_t *)address, value, __ATOMIC_RELEASE);
  }
  static uint32_t load_acquire32(volatile uint32_t *address) {
    return __atomic_load_n((uint32_t *)address, __ATOMIC_ACQUIRE);
  }

  // ---- syscall の発行口 ---------------------------------------------------
  // a0 = 番号 (Q1: 即値でなくレジスタ渡し。RISC-V の ecall に即値が無いため)。
  // レジスタを明示束縛するので、最適化レベルによらずオペランド割付が壊れない。
  struct syscall_result {
    uintptr_t error;
    uintptr_t value;
  };
  static inline syscall_result syscall(uintptr_t number, uintptr_t a1 = 0,
                                       uintptr_t a2 = 0, uintptr_t a3 = 0,
                                       uintptr_t a4 = 0) {
    register uintptr_t r0 asm("r0") = number;
    register uintptr_t r1 asm("r1") = a1;
    register uintptr_t r2 asm("r2") = a2;
    register uintptr_t r3 asm("r3") = a3;
    register uintptr_t r12 asm("r12") = a4;
    asm volatile("svc 0"
                 : "+r"(r0), "+r"(r1)
                 : "r"(r2), "r"(r3), "r"(r12)
                 : "memory");
    return {r0, r1};
  }

  [[noreturn]] static void enter_thread_mode(uintptr_t stack_top,
                                             uintptr_t stack_limit_address,
                                             void (*entry)()) {
    shizuku_armv8m_enter_thread_mode(stack_top, stack_limit_address, entry);
  }
};

// armv8m_ctx.S の .equ と一致させる (ズレたら文脈が黙って壊れる)。
static_assert(offsetof(armv8m::context_t, sp) == 32,
              "context_t layout mismatch: sp offset (asm CTX_SP)");
static_assert(offsetof(armv8m::context_t, exc_return) == 36,
              "context_t layout mismatch: exc_return offset (asm CTX_EXC_RETURN)");
static_assert(offsetof(armv8m::context_t, fp) == 40,
              "context_t layout mismatch: fp offset (asm CTX_FP)");
static_assert(offsetof(armv8m::context_t, psplim) == 104,
              "context_t layout mismatch: psplim offset (asm CTX_PSPLIM)");
static_assert(offsetof(armv8m::context_t, control) == 108,
              "context_t layout mismatch: control offset (asm CTX_CONTROL)");
static_assert(offsetof(armv8m::context_t, region_base) == 112,
              "context_t layout mismatch: region_base offset");
static_assert(offsetof(armv8m::context_t, region_limit) == 116,
              "context_t layout mismatch: region_limit offset");
// 戻り口 (armv8m_ctx.S) が即値で埋め込んでいる ABI 定数との一致。
static_assert((uintptr_t)shizuku::primitive::RETURN == 2,
              "armv8m_ctx.S の return stub が撃つ番号と一致させること");
static_assert(shizuku::concepts::arch_requires<armv8m>,
              "armv8m does not satisfy the arch concept");

} // namespace archs
} // namespace shizuku
#endif // SHIZUKU_ARCHS_ARMV8M_HPP
