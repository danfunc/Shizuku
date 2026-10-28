#ifndef SHIZUKU_ARCHS_ARMV8M_HPP
#define SHIZUKU_ARCHS_ARMV8M_HPP
#include <cstddef>
#include <cstdint>
#include "arch/ppb.hpp"
#include "shizuku/concepts/arch.hpp"
#include "shizuku/kernel_abi.hpp"
#include "shizuku/hot.hpp"
#include "shizuku/armv8m_abi.hpp"

// ARMv8-M mainline (Cortex-M33 / RP2350) の arch バックエンド。
// 文脈退避・復帰・戻り口・スレッドモード移行の実体は armv8m_ctx.S。
// レイアウトと退避規約は旧参照実装 (svc_asm_handler.S / kernel.hpp context_t) の
// 実機検証済みの形をそのまま移植した。

extern "C" {
// armv8m_ctx.S の例外入口。board 層がベクタへ登録する。
void shizuku_armv8m_svc_entry();
void shizuku_armv8m_svc_handler();
void shizuku_armv8m_call_entry();
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
// asm から呼ばれるフック。実体は modules/pico_sdk_support/rp2350/arch_glue.cpp。
struct shizuku_armv8m_context;
shizuku_armv8m_context *shizuku_current_context();
void shizuku_svc_dispatch(shizuku_armv8m_context *context);
void shizuku_pendsv_dispatch(shizuku_armv8m_context *context);
}

namespace shizuku {
namespace archs {

// MpuRegionCount: PMSAv8 の region 数 (RP2350 は 8)。
// PrioBits: NVIC に実装された優先度 bit 数 (RP2350 は 4)。値は chips/ 側が渡す。
template <unsigned MpuRegionCount, unsigned PrioBits> class armv8m {
public:
  static constexpr unsigned MPU_REGION_COUNT = MpuRegionCount;
  static constexpr unsigned NVIC_PRIORITY_BITS = PrioBits;
  struct exception_frame_t {
    uint32_t r0, r1, r2, r3, r12, lr, pc, xPSR;
  };
  // CONTROL レジスタのキャッシュ値 (bit1=SPSEL=1 固定: スレッドは常に PSP、
  // bit0=nPRIV)。bit2(FPCA) はハードウェア管理なので、CTX_RESTORE は MRS で読んだ
  // 現在値の bit0 だけをこの値で置き換える read-modify-write を行う。
  static constexpr uint32_t CONTROL_PRIV_PSP = 0b10;   // nPRIV=0 (特権)
  static constexpr uint32_t CONTROL_UNPRIV_PSP = 0b11; // nPRIV=1 (非特権)
  // 呼び出しフレームを積むときスタック下限の手前に残す余白のうち、
  // **ISA では決まらない分** = カーネルオブジェクトのハンドラ連鎖が使う C フレーム。
  // ★これは「なんとなくの安全マージン」ではない。**呼び先が最低 1 回はカーネルを
  //   呼び返せる**ことを保証する量でなければならない — 呼び先は戻るためにも
  //   syscall を撃つので、ここが足りないと「戻ることすらできない」状態が作れて
  //   しまう。足りないと PSPLIM の UsageFault が先に出て無言で死ぬ。
  // ★★2026-09-05: 退避域 (ヘッダ + 例外フレーム) のぶんは**ここに書かない**。
  //   カーネル側が sizeof(call_frame_header) と EXC_FRAME_MAX_BYTES /
  //   FAULT_CONTEXT_BYTES から算術で足す (dispatch.cpp の call_frame_push)。ここへ数字で書き写していたせいで、ヘッダに 1 語足しただけで
  //   余白が痩せ、実機が無言で固まった (原因の特定に実機 6 往復を要した)。
  //   数えるべきものが増えたら、数字ではなく**式**を直すこと。
  static constexpr uint32_t CALL_HEADROOM = 512;
  static constexpr bool HAS_CYCLE_COUNTER = true;
  static constexpr bool IS_ARMV8M = true;
  static constexpr bool CALL_IN_PLACE = true;
  static constexpr bool SVC_BREAKDOWN_ENABLED = SHIZUKU_RP2350_SVC_BREAKDOWN != 0;

  // armv8m_ctx.S の .equ (CTX_*) と一致させること。下の static_assert が両縛りする。
  struct context_t {
    static constexpr bool COMPACT_LEDGER = true;
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
    uint32_t stack_ceiling = 0; // trusted activation limit; not supplied by PSP
  };
  // 余白の算術に使う、実際に積まれる量。
  // 例外 1 回でハードウェアが積む最大 (FPU 活性時の拡張フレーム 104B)。
  static constexpr uint32_t EXC_FRAME_MAX_BYTES = 104;
  // The fifth C argument is passed on the stack by AAPCS. Reserve an aligned
  // eight-byte slot in every call frame so dispatch can populate it safely.
  static constexpr uint32_t HANDLER_STACK_ARG_BYTES = 8;
  // fault 入口が CTX_SAVE でスレッドスタックへ退避する文脈の大きさ。
  static constexpr uint32_t FAULT_CONTEXT_BYTES = sizeof(context_t);
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
  static void set_handler_args(exception_frame_t &frame, const uintptr_t *args,
                               uintptr_t stack_arg_address) {
    (void)stack_arg_address;
    set_args(frame, args);
    frame.r12 = static_cast<uint32_t>(args[4]);
  }
  static void set_result(exception_frame_t &frame, uintptr_t error,
                         uintptr_t value) {
    frame.r0 = (uint32_t)error;
    frame.r1 = (uint32_t)value;
  }
  static void set_entry(exception_frame_t &frame, uintptr_t pc, uintptr_t lr) {
    if (lr == return_stub()) {
      // The thread-mode veneer translates r12 to AAPCS's fifth stack argument.
      frame.pc = (uint32_t)(uintptr_t)&shizuku_armv8m_call_entry;
      frame.lr = (uint32_t)pc;
    } else {
      frame.pc = (uint32_t)pc;
      frame.lr = (uint32_t)lr;
    }
  }
  // 戻り口 (1 本だけ)。呼び先が普通に return するとここへ落ち、RETURN を撃つ。
  // ハンドラから出ればプリミティブとして巻き戻し、オブジェクトから出れば経路判定で
  // オブジェクトランドのハンドラへ「戻った」という知らせとして届く。
  static uintptr_t return_stub() {
    return (uintptr_t)&shizuku_armv8m_return_stub;
  }
  template <auto F> static void (*method_stub())() {
    return reinterpret_cast<void (*)()>(shizuku::armv8m_abi::entry_address<F>());
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
    // Spawned methods use the same r12 -> AAPCS stack-argument veneer as
    // in-place calls. A direct five-word wrapper would read at stack_top.
    set_entry(*frame, entry_pc, return_pc);
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
    ppb::systick->rvr = cycles;
    ppb::systick->cvr = 0;   // 書き込みでカウンタをクリア (rvr から数え直す)
    ppb::systick->csr = 0x7; // ENABLE | TICKINT | プロセッサクロック
  }
  static void timer_cancel() { ppb::systick->csr = 0; }

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

  // DWT サイクルカウンタ。HAS_CYCLE_COUNTER が true の arch だけが持つ
  // (concepts/arch.hpp が条件付きで要求する)。TRCENA が元栓、DWT_CTRL bit0 が CYCCNT。
  static constexpr uintptr_t DWT_CTRL_ADDRESS = 0xE0001000u;
  static constexpr uintptr_t DWT_CYCCNT_ADDRESS = 0xE0001004u;
  static constexpr uint32_t DWT_CTRL_CYCCNTENA = 1u << 0;

  // 動いていることを確かめてから true を返す。実装されていない / 元栓が閉まって
  // いると 0 のままで、気づかないと「0 サイクルで呼べた」と報告してしまう。
  static bool cycle_counter_enable() {
    at(DEMCR_ADDRESS) = at(DEMCR_ADDRESS) | DEMCR_TRCENA;
    asm volatile("dsb" ::: "memory");
    asm volatile("isb" ::: "memory");
    at(DWT_CYCCNT_ADDRESS) = 0;
    at(DWT_CTRL_ADDRESS) = at(DWT_CTRL_ADDRESS) | DWT_CTRL_CYCCNTENA;
    asm volatile("dsb" ::: "memory");
    asm volatile("isb" ::: "memory");
    const uint32_t first = at(DWT_CYCCNT_ADDRESS);
    for (uint32_t spin = 0; spin < 64; ++spin)
      asm volatile("nop" ::: "memory");
    return at(DWT_CYCCNT_ADDRESS) != first;
  }

  static uint32_t cycle_counter_read() { return at(DWT_CYCCNT_ADDRESS); }

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
    const uint32_t csr = ppb::systick->csr; // ★読むと COUNTFLAG は落ちる。1 回だけ読む
    wrapped = (csr & (1u << 16)) != 0;
    if (!(csr & 0x1u)) { // 動いていない
      wrapped = false;
      return 0;
    }
    return ppb::systick->cvr & 0x00FFFFFFu;
  }
  // ★切替は最低優先度の遅延例外でしか起こさない (DESIGN §14.5.1 の規約)。
  //   これにより「syscall ハンドラの中にいる = そのコアでは切り替わらない」が
  //   取得コストゼロで手に入る。
  static void pend_context_switch() {
    asm volatile("dmb" ::: "memory");
    *ppb::scb_icsr = 1u << 28; // PENDSVSET
    asm volatile("dsb\nisb" ::: "memory");
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
    ppb::mpu->rnr = index;
    ppb::mpu->rbar =
        ((uint32_t)base & ~0x1Fu) | (access << 1) | (execute_never ? 1u : 0u);
    ppb::mpu->rlar = ((uint32_t)limit & ~0x1Fu) | (attribute << 1) | 1u;
  }
  static void region_disable(uint32_t index) {
    ppb::mpu->rnr = index;
    ppb::mpu->rlar = 0;
  }
  // attr0 = 通常メモリ (書き戻し), attr1 = 通常メモリ (キャッシュしない), attr2 = Device-nGnRE
  static void protection_enable() {
    *ppb::mpu_mair0 = 0xFFu | (0x44u << 8) | (0x04u << 16);
    // ★PRIVDEFENA: region で明示していない場所は「特権だけが触れる」。これが
    //   単一アドレス空間における「カーネル空間」の実体で、MMU が無くても
    //   非特権から見えない領域を作れる (DESIGN §11.1)。
    ppb::mpu->ctrl = ppb::MPU_CTRL_PRIVDEFENA | ppb::MPU_CTRL_ENABLE;
    asm volatile("dsb\nisb" ::: "memory");
  }
  static void protection_disable() {
    ppb::mpu->ctrl = 0;
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
  [[gnu::always_inline]] static inline bool context_privileged(const context_t &context) {
    return (context.control & 1u) == 0;
  }
  [[gnu::always_inline]] static inline void set_method_argument(exception_frame_t &frame,
                                                                uintptr_t argument) {
    frame.r0 = (uint32_t)argument;
  }
  [[gnu::always_inline]] static inline void move_descriptor_arguments(exception_frame_t &frame) {
    frame.r0 = frame.r2;
  }
  // Kept as a source-compatibility type for S1 fixture instantiations only.
  // No context/ledger payload is connected in the caller-clobber ABI.
  struct inplace_payload_t { uint32_t r4_r11[8] = {}; };
  // ★set_priv と同じ形 (Q8 / DESIGN §11.3)。ここでは値を持たせるだけで、
  //   実際に MPU region へ書くのは復帰の直前 (shizuku_restore_region_window)。
  //   limit==0 は「窓なし」。
  static void set_region_window(context_t &context, uintptr_t base,
                                uintptr_t limit) {
    context.region_base = (uint32_t)base;
    context.region_limit = (uint32_t)limit;
  }
  static constexpr bool HAS_OBJECT_PROTECTION = true;
  // The converter owns normal register preservation; only exception paths scrub.
  static constexpr bool SCRUB_CALLEE_SAVED_ON_CALL = false;

  static uintptr_t call_return_sp(const context_t &context) {
    return psp_after_return(context);
  }
  static constexpr bool call_stack_has_room(uintptr_t sp, uintptr_t limit,
                                             uintptr_t ceiling) {
    const uintptr_t reserve = EXC_FRAME_MAX_BYTES + FAULT_CONTEXT_BYTES +
                              CALL_HEADROOM + armv8m_abi::max_reservation_bytes +
                              HANDLER_STACK_ARG_BYTES;
    return (sp & 31u) == 0 && sp >= reserve &&
           (ceiling == 0 || sp <= ceiling) && (limit == 0 || sp - reserve >= limit);
  }
  static bool call_header_has_room(const context_t &context, uint32_t) {
    return call_stack_has_room(call_return_sp(context), context.psplim, context.stack_ceiling);
  }
  struct lazy_fp_state {
    static void materialize() {
      uint32_t fpscr;
      asm volatile("vmrs %0, fpscr" : "=r"(fpscr) :: "memory");
      (void)fpscr;
    }
  };
  template <class FloatingPointState = lazy_fp_state>
  static exception_frame_t *call_resume_at(context_t &context, uintptr_t pc,
      uintptr_t sp, bool privileged, uintptr_t base, uintptr_t limit, bool scrub) {
    const uint32_t bytes = exc_frame_bytes(context);
    auto *next = reinterpret_cast<exception_frame_t *>(sp - bytes);
    if (bytes != sizeof(exception_frame_t)) {
      // Materialize any lazy FP frame before relocating its return-value slots.
      FloatingPointState::materialize();
      __builtin_memmove(reinterpret_cast<uint8_t *>(next) + sizeof(exception_frame_t),
                        reinterpret_cast<uint8_t *>(context.sp) + sizeof(exception_frame_t),
                        bytes - sizeof(exception_frame_t));
    }
    *next = exception_frame_t{};
    next->pc = (uint32_t)pc;
    next->xPSR = 1u << 24;
    context.sp = next;
    set_priv(context, privileged);
    set_region_window(context, base, limit);
    if (scrub) s_resume_scrub[core_slot()] = true;
    return next;
  }

  static inline bool s_in_svc[2] = {};
  static inline bool s_deferred_svc[2] = {};
  static inline bool s_resume_scrub[2] = {};
  static bool defer_svc_switch() {
    const auto core = core_slot();
    if (!s_in_svc[core]) return false;
    s_deferred_svc[core] = true;
    return true;
  }
  static bool take_deferred_svc() {
    const auto core = core_slot();
    const bool pending = s_deferred_svc[core];
    s_deferred_svc[core] = false;
    return pending;
  }
  static constexpr uint32_t shrink_stack_limit(uint32_t original, uintptr_t ceiling) {
    return (uint32_t)((ceiling - 1) & ~uintptr_t(31)) | (original & 31u);
  }
  // 貸与窓の RLAR.LIMIT: 排他的な終端 end を含む最後の 32B ブロックの先頭 (end-1 を 32B に切り下げ)。
  static constexpr uint32_t grant_window_limit(uintptr_t end) {
    return (uint32_t)((end - 1u) & ~(uintptr_t)0x1Fu);
  }
  // stack 窓は 32B 境界。arena の stack はこの境界に置いて、窓が隣のブロックの header にかからないようにする。
  static constexpr uintptr_t STACK_WINDOW_ALIGN = 32;
  static bool context_unprivileged(const context_t &c) { return (c.control & 1u) != 0; }

  // Region 配置 (Plan §4.1)
  // Region 0: Flash コード固定
  // Region 1: 無効 (PRIVDEFENA でカーネルのみアクセス可)
  // Region 2: スレッドスタック窓
  // Region 3: オブジェクトデータ窓
  // Region 4: オブジェクト MMIO 窓
  // Region 5: 動的貸与窓 (GRANT_REGION_INDEX)
  static constexpr uint32_t REGION_FLASH = 0;
  static constexpr uint32_t REGION_UNUSED = 1;
  static constexpr uint32_t REGION_STACK = 2;
  static constexpr uint32_t REGION_DATA = 3;
  static constexpr uint32_t REGION_MMIO = 4;
  static constexpr uint32_t GRANT_REGION_INDEX = 5;

  struct mpu_window_entry {
    uint32_t rbar = 0;
    uint32_t rlar = 0;
  };

  struct object_protection_window {
    uint32_t owner_id = 0;
    mpu_window_entry data{};
    mpu_window_entry mmio{};
  };

  static inline constexpr bool is_dma_or_system_peripheral(uintptr_t base, uintptr_t limit) {
    if (base < 0xE0100000u && limit > 0xE0000000u) return true; // PPB / NVIC / SCB
    if (base < 0x50010000u && limit > 0x50000000u) return true; // RP2350 DMA コントローラ
    if (base < 0x50500000u && limit > 0x50200000u) return true; // RP2350 PIO0..2
    return false;
  }

  static inline constexpr mpu_window_entry encode_data_window(uintptr_t base, uintptr_t limit) {
    if (limit <= base || base == 0) return {0, 0};
    uint32_t rbar = ((uint32_t)base & ~0x1Fu) | (ACCESS_RW_ALL << 1) | 1u; // XN=1
    // RLAR.LIMIT は含む側の最後の 32B ブロック。排他的な終端 (limit) から 1 を引いてそのブロックを指す。
    uint32_t lim32 = (uint32_t)((limit - 1u) & ~31u);
    uint32_t rlar = lim32 | (1u << 1) | 1u; // Attr 1 (Normal Non-cacheable), Enable=1
    return {rbar, rlar};
  }

  static inline constexpr mpu_window_entry encode_mmio_window(uintptr_t base, uintptr_t limit) {
    if (limit <= base || base == 0) return {0, 0};
    if (is_dma_or_system_peripheral(base, limit)) return {0, 0};
    uint32_t rbar = ((uint32_t)base & ~0x1Fu) | (ACCESS_RW_ALL << 1) | 1u; // XN=1
    uint32_t lim32 = (uint32_t)((limit - 1u) & ~31u);
    uint32_t rlar = lim32 | (2u << 1) | 1u; // Attr 2 (Device-nGnRE), Enable=1
    return {rbar, rlar};
  }

  static inline constexpr mpu_window_entry encode_stack_window(uintptr_t base, uintptr_t limit) {
    if (limit <= base || base == 0) return {0, 0};
    uint32_t rbar = ((uint32_t)base & ~0x1Fu) | (ACCESS_RW_ALL << 1) | 1u; // XN=1
    // RLAR.LIMIT は含む側の最後の 32B ブロック。排他的な終端 (limit) から 1 を引いてそのブロックを指す。
    uint32_t lim32 = (uint32_t)((limit - 1u) & ~31u);
    uint32_t rlar = lim32 | (1u << 1) | 1u; // Attr 1 (Normal Non-cacheable), Enable=1
    return {rbar, rlar};
  }

  static constexpr size_t kMaxObjectWindows = 72;  // OBJECT_COUNT (configs/kit.bzl) 以上にする
  static inline object_protection_window s_object_windows[kMaxObjectWindows] = {};
  static inline uint32_t s_object_window_seq[kMaxObjectWindows] = {};
  // MPU は core ごとに持つので、「今 region 3/4 に張ってある窓の持ち主」も core ごとに覚える。
  static uint32_t core_slot() { return *(volatile const uint32_t *)0xD0000000u & 1u; }
  static inline uint32_t s_programmed_object[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
  static inline uint32_t s_programmed_object_seq[2] = {};

  // region 2..5 (stack / data / MMIO / 貸与窓) にいま張ってある (RBAR, RLAR) も core ごとに覚え、
  // 同じ値なら MPU へ書かない。無効な region は {0, 0} に正規化する (RLAR=0 のとき RBAR は意味を
  // 持たない)。初期値は「未知」(どの実値とも一致しない) で、最初の 1 回は必ず書く。
  // ★この 4 つの region への書込みは region_program だけを通すこと。直書きすると覚えた値と実機が
  //   ずれ、以後の「同じだから書かない」が誤る。
  struct region_shadow {
    uint32_t rbar = 0xFFFFFFFFu;
    uint32_t rlar = 0xFFFFFFFFu;
  };
  static inline region_shadow s_programmed_region[2][MpuRegionCount] = {};

  static constexpr mpu_window_entry canonical_region(uint32_t rbar, uint32_t rlar) {
    return rlar == 0 ? mpu_window_entry{0, 0} : mpu_window_entry{rbar, rlar};
  }
  static constexpr bool region_matches(const region_shadow &shadow, uint32_t rbar, uint32_t rlar) {
    const auto want = canonical_region(rbar, rlar);
    return shadow.rbar == want.rbar && shadow.rlar == want.rlar;
  }
  // 貸与窓 (GRANT_REGION_INDEX) の値。end==0 は窓なし。RLAR.LIMIT は排他的な終端 end を含む
  // 最後の 32B ブロック (grant_window_limit)。粒度の限界として、要求範囲の前後は最大 31B だけ
  // 同じブロックの道連れで読めてしまい得る — 隣接ファイルとの間に 32B 以上の余白を置くのは
  // 呼び出し側 (flash_fs のセクタ境界配置) の仕事。
  static constexpr mpu_window_entry grant_window_entry(uintptr_t base, uintptr_t end) {
    if (end == 0) return {0, 0};
    return {((uint32_t)base & ~0x1Fu) | (ACCESS_RO_ALL << 1),
            (grant_window_limit(end) & ~0x1Fu) | 1u};
  }
  // region index を (rbar, rlar) にする。覚えた値と同じなら何も書かず false。書いたら true
  // (書込みの完了待ち DSB は呼び手の責任)。
  [[gnu::always_inline]] static inline bool region_program(uint32_t core, uint32_t index,
                                                           uint32_t rbar, uint32_t rlar) {
    auto &shadow = s_programmed_region[core][index];
    if (region_matches(shadow, rbar, rlar)) return false;
    ppb::mpu->rnr = index;
    if (rlar != 0) {
      ppb::mpu->rbar = rbar;
      ppb::mpu->rlar = rlar;
    } else {
      ppb::mpu->rlar = 0;
    }
    const auto want = canonical_region(rbar, rlar);
    shadow.rbar = want.rbar;
    shadow.rlar = want.rlar;
    return true;
  }
  [[gnu::always_inline]] static inline bool apply_grant_window(uintptr_t base, uintptr_t end) {
    const auto entry = grant_window_entry(base, end);
    return region_program(core_slot(), GRANT_REGION_INDEX, entry.rbar, entry.rlar);
  }

  static void invalidate_active_object() {
    s_programmed_object[core_slot()] = 0xFFFFFFFFu;
  }

  static bool has_object_bounds(uint32_t object_id) {
    object_protection_window copy{};
    uint32_t version;
    return snapshot_object_window(object_id, copy, version) && copy.owner_id != 0;
  }

  static bool set_object_bounds(uint32_t object_id, uintptr_t data_base, uintptr_t data_limit,
                                uintptr_t mmio_base, uintptr_t mmio_limit) {
    if (object_id >= kMaxObjectWindows) return false;
    auto &seq = s_object_window_seq[object_id];
    uint32_t version = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
    if ((version & 1u) || version >= UINT32_MAX - 1u ||
        !__atomic_compare_exchange_n(&seq, &version, version + 1u, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
      return false;
    auto &window = s_object_windows[object_id];
    const auto data = encode_data_window(data_base, data_limit);
    const auto mmio = encode_mmio_window(mmio_base, mmio_limit);
    __atomic_store_n(&window.owner_id, object_id, __ATOMIC_SEQ_CST);
    __atomic_store_n(&window.data.rbar, data.rbar, __ATOMIC_SEQ_CST);
    __atomic_store_n(&window.data.rlar, data.rlar, __ATOMIC_SEQ_CST);
    __atomic_store_n(&window.mmio.rbar, mmio.rbar, __ATOMIC_SEQ_CST);
    __atomic_store_n(&window.mmio.rlar, mmio.rlar, __ATOMIC_SEQ_CST);
    __atomic_store_n(&seq, version + 2u, __ATOMIC_RELEASE);
    return true;
  }

  static bool snapshot_object_window(uint32_t id, object_protection_window &copy,
                                     uint32_t &version) {
    if (id >= kMaxObjectWindows) return false;
    const uint32_t before = __atomic_load_n(&s_object_window_seq[id], __ATOMIC_SEQ_CST);
    if (before & 1u) return false;
    const auto &window = s_object_windows[id];
    copy.owner_id = __atomic_load_n(&window.owner_id, __ATOMIC_SEQ_CST);
    copy.data.rbar = __atomic_load_n(&window.data.rbar, __ATOMIC_SEQ_CST);
    copy.data.rlar = __atomic_load_n(&window.data.rlar, __ATOMIC_SEQ_CST);
    copy.mmio.rbar = __atomic_load_n(&window.mmio.rbar, __ATOMIC_SEQ_CST);
    copy.mmio.rlar = __atomic_load_n(&window.mmio.rlar, __ATOMIC_SEQ_CST);
    version = __atomic_load_n(&s_object_window_seq[id], __ATOMIC_SEQ_CST);
    return before == version && !(version & 1u);
  }

  // 窓の書込みは例外復帰の前にしか呼ばれない。例外復帰は文脈同期イベントなので ISB は要らず、
  // 書込みの完了待ち (DSB) だけ置く。
  static void apply_object_window(const object_protection_window &win) {
    const auto core = core_slot();
    region_program(core, REGION_DATA, win.data.rbar, win.data.rlar);
    region_program(core, REGION_MMIO, win.mmio.rbar, win.mmio.rlar);
    asm volatile("dsb" ::: "memory");
  }

  // 前回張った値と同じなら書かない (同じ thread へ戻る例外復帰は毎回ここを通る)。
  static void apply_stack_window(uint32_t rbar, uint32_t rlar) {
    if (region_program(core_slot(), REGION_STACK, rbar, rlar))
      asm volatile("dsb" ::: "memory");
  }

  // 非特権の窓 (region 3/4) を id のものにする。すでにそうなら何も書かない。特権は PRIVDEFENA で
  // 全域に触れ、窓に制限されないので、特権の呼び先/呼び元のときは窓を触らない (呼ばない)。
  static void ensure_object_window(uint32_t id) {
    const auto core = core_slot();
    uint32_t &programmed = s_programmed_object[core];
    uint32_t version = id < kMaxObjectWindows
        ? __atomic_load_n(&s_object_window_seq[id], __ATOMIC_ACQUIRE) : 1u;
    if (programmed == id && !(version & 1u) && s_programmed_object_seq[core] == version)
      return;
    object_protection_window copy{};
    if (!snapshot_object_window(id, copy, version)) {
      region_program(core, REGION_DATA, 0, 0);
      region_program(core, REGION_MMIO, 0, 0);
      programmed = 0xFFFFFFFFu;
      return;
    }
    apply_object_window(copy);
    programmed = id;
    s_programmed_object_seq[core] = version;
  }

  template <class Frame>
  static void switch_callee_protection(context_t &context, Frame &frame, uint32_t callee_id, bool privileged) {
    (void)context;
    (void)frame;
    if (!privileged) ensure_object_window(callee_id);
  }

  // 呼び元が非特権のときだけ呼ぶ (戻り先の窓を戻す)。
  static void switch_caller_protection(uint32_t caller_object) {
    ensure_object_window(caller_object);
  }
  static void stack_limit_set(context_t &context, uintptr_t limit) {
    context.psplim = (uint32_t)limit;
  }
  static uintptr_t stack_limit(const context_t &context) {
    return context.psplim;
  }
  // CALL 時に、戻りの dispatch が積む 1 枚ぶんの余白が残るか (dispatch.cpp の
  // do_call)。PSPLIM は実スタック下限の検出であり、call_frame_push の**ソフトな**
  // 余白判定 (NO_STACK) は別物なので、ハード検出の有無には頼らず同じ式で見る。
  // 式は call_frame_push と同じ構成要素 (ヘッダ・実フレーム・余白) から出す。
  static bool call_entry_has_return_room(const context_t &context,
                                         uint32_t header_bytes) {
    const uintptr_t limit = stack_limit(context);
    if (limit == 0)
      return true;
    const uintptr_t frame_bytes = exc_frame_bytes(context);
    const uintptr_t unaligned = header_bytes + frame_bytes + HANDLER_STACK_ARG_BYTES;
    const uintptr_t total = (unaligned + 7u) & ~(uintptr_t)7u;
    const uintptr_t dispatch_reserve = unaligned + EXC_FRAME_MAX_BYTES +
                                       FAULT_CONTEXT_BYTES + CALL_HEADROOM;
    const uintptr_t sp = (uintptr_t)context.sp;
    // total は呼び先のフレーム、もう 1 つの total は戻りの dispatch のフレーム。
    return sp >= limit && sp - limit >= 2u * total + dispatch_reserve;
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
  using syscall_result = shizuku::armv8m_abi::result;
  [[gnu::always_inline]] static inline syscall_result syscall(uintptr_t number, uintptr_t a1 = 0,
                                       uintptr_t a2 = 0, uintptr_t a3 = 0,
                                       uintptr_t a4 = 0) {
    return shizuku::armv8m_abi::syscall<>(number, a1, a2, a3, a4);
  }

  [[noreturn]] static void enter_thread_mode(uintptr_t stack_top,
                                             uintptr_t stack_limit_address,
                                             void (*entry)()) {
    shizuku_armv8m_enter_thread_mode(stack_top, stack_limit_address, entry);
  }
};

// context_t のレイアウトは region 数・優先度 bit 数に依らないので、RP2350 の実体で代表して検査する。
using armv8m_rp2350 = armv8m<8, 4>;
// armv8m_ctx.S の .equ と一致させる (ズレたら文脈が黙って壊れる)。
#if UINTPTR_MAX == 0xFFFFFFFFu  // asm のオフセットは 32bit の実機の配置。host (64bit) では検査しない
static_assert(offsetof(armv8m_rp2350::context_t, sp) == 32,
              "context_t layout mismatch: sp offset (asm CTX_SP)");
static_assert(offsetof(armv8m_rp2350::context_t, exc_return) == 36,
              "context_t layout mismatch: exc_return offset (asm CTX_EXC_RETURN)");
static_assert(offsetof(armv8m_rp2350::context_t, fp) == 40,
              "context_t layout mismatch: fp offset (asm CTX_FP)");
static_assert(offsetof(armv8m_rp2350::context_t, psplim) == 104,
              "context_t layout mismatch: psplim offset (asm CTX_PSPLIM)");
static_assert(offsetof(armv8m_rp2350::context_t, control) == 108,
              "context_t layout mismatch: control offset (asm CTX_CONTROL)");
static_assert(offsetof(armv8m_rp2350::context_t, region_base) == 112,
              "context_t layout mismatch: region_base offset");
static_assert(offsetof(armv8m_rp2350::context_t, region_limit) == 116,
              "context_t layout mismatch: region_limit offset");
#endif
// 戻り口 (armv8m_ctx.S) が即値で埋め込んでいる ABI 定数との一致。
static_assert((uintptr_t)shizuku::primitive::RETURN == 2,
              "armv8m_ctx.S の return stub が撃つ番号と一致させること");
static_assert(shizuku::concepts::arch_requires<armv8m_rp2350>,
              "armv8m does not satisfy the arch concept");

} // namespace archs
} // namespace shizuku
#endif // SHIZUKU_ARCHS_ARMV8M_HPP
