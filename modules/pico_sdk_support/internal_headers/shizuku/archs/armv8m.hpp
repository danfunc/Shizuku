#ifndef SHIZUKU_ARCHS_ARMV8M_HPP
#define SHIZUKU_ARCHS_ARMV8M_HPP
#include <cstddef>
#include <cstdint>
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
// 呼び先が普通に return したときの戻り口 (RETURN プリミティブを 1 段ぶん発行)。
void shizuku_armv8m_return_stub();
// スレッドスタック (PSP) へ移って entry を呼ぶ。戻らない。
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
  // 呼び出しフレームを積むときスタック下限の手前に残す余裕。呼び先のプロローグと
  // 最初の数フレームぶんを見込む (足りないと PSPLIM の UsageFault が先に出る)。
  static constexpr uint32_t CALL_HEADROOM = 256;

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
  // ハンドラを起こすときにカーネルが渡す情報。呼び先の引数は a0..a3 で埋まって
  // いるので callee-saved レジスタで渡す。
  //   r4 = svc 番号 (a0 と同値。ハンドラ側の読みやすさのため)
  //   r7 = **今のネスト数**。オブジェクトは RETURN を撃てないので、何段巻き戻すかを
  //        決めるのはハンドラであり、その申告 (両側チェック) の材料がこれ。
  // 下の handler_shim がこれらを C の引数 5..8 へ変換する。
  static void set_handler_info(context_t &context, uintptr_t number,
                               uintptr_t depth) {
    context.r4 = (uint32_t)number;
    context.r5 = 0;
    context.r6 = 0;
    context.r7 = (uint32_t)depth;
  }

  // ハンドラの ABI シム。カーネルが callee-saved で渡した情報を、C の第 5..8 引数
  // (AAPCS ではスタック渡し) として受け取れる形に変換する。
  // ★push 順で [sp+0/4/8/12] に来るのは r4, r5, r6, **r7** (r12 ではない)。
  //   参照実装はここを取り違えて時間を溶かしている。だから naked は共通ヘッダの
  //   この 1 ヶ所にだけ置き、サブシステムごとに書かせない。
  // ★戻るときは r4-r7 を復元してから lr (カーネルの戻り口) へ返る。戻り口は r7 の
  //   ネスト数を申告に使うので、ここで復元されていることが前提になる。
  template <auto FUNCTION>
  __attribute__((naked, aligned(4))) static void handler_shim() {
    asm volatile("push {r4-r7, r12, lr}\n"
                 "ldr  r4, 1f\n"
                 "blx  r4\n"
                 "pop  {r4-r7, r12, pc}\n"
                 ".align 2\n"
                 "1: .word %c0\n"
                 :
                 : "i"(FUNCTION) // シンボルをそのまま即値として埋める
                 :);
  }
  template <auto FUNCTION> static uintptr_t handler_entry() {
    return (uintptr_t)&handler_shim<FUNCTION>;
  }
  // 戻り口 (1 本だけ)。呼び先が普通に return するとここへ落ち、RETURN を撃つ。
  // ハンドラから出ればプリミティブとして巻き戻し、オブジェクトから出れば経路判定で
  // オブジェクトランドのハンドラへ「戻った」という知らせとして届く。
  static uintptr_t return_stub() {
    return (uintptr_t)&shizuku_armv8m_return_stub;
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
// 戻り口 (armv8m_ctx.S) が即値で埋め込んでいる ABI 定数との一致。
static_assert((uintptr_t)shizuku::primitive::RETURN == 2,
              "armv8m_ctx.S の return stub が撃つ番号と一致させること");
static_assert(shizuku::concepts::arch_requires<armv8m>,
              "armv8m does not satisfy the arch concept");

} // namespace archs
} // namespace shizuku
#endif // SHIZUKU_ARCHS_ARMV8M_HPP
