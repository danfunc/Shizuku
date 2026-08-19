#ifndef SHIZUKU_ARCHS_ARMV8M_HPP
#define SHIZUKU_ARCHS_ARMV8M_HPP
#include <cstddef>
#include <cstdint>
#include "shizuku/concepts/arch.hpp"

// ARMv8-M mainline (Cortex-M33 / RP2350) の arch バックエンド。
// 文脈退避・復帰の実体は armv8m_ctx.S (CTX_SAVE / CTX_RESTORE)。
// レイアウトと退避規約は参照実装 (latest_ver_from_flight_robocon の
// svc_asm_handler.S / kernel.hpp context_t) の実機検証済みの形をそのまま移植した。

extern "C" {
// armv8m_ctx.S の例外入口。board 層がベクタへ登録する。
void shizuku_armv8m_svc_entry();
void shizuku_armv8m_pendsv_entry();
// asm から呼ばれるフック。カーネル側 (当面は board 層のグルー) が実装する。
// shizuku_current_context は「今このコアで走っているスレッドの文脈」を返す。
// dispatch は退避完了後に呼ばれ、戻った後 shizuku_current_context を**再取得**して
// 復帰する (= dispatch 内で現在文脈を差し替えればスレッド切替になる)。
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

  // armv8m_ctx.S の .equ (CTX_*) と一致させること。下の static_assert が両縛りする。
  struct context_t {
    uint32_t r4 = 0, r5 = 0, r6 = 0, r7 = 0;     // offset 0..12
    uint32_t r8 = 0, r9 = 0, r10 = 0, r11 = 0;   // offset 16..28
    exception_frame_t *sp = nullptr;             // offset 32
    // キャッシュした EXC_RETURN。bit4=0 なら拡張 (FP) フレーム。新規スレッドは
    // 基本フレームの Thread/PSP へ復帰する (0 のままだと初回復帰で即 HardFault)。
    uint32_t exc_return = 0xFFFFFFFD;            // offset 36
    uint32_t fp[16] = {};                        // offset 40 (S16-S31 退避域)
    // PSP 下限。0 = 制限なし。スレッド寿命の間は不変なので CTX_RESTORE のみが適用。
    uint32_t psplim = 0;                         // offset 104
    uint32_t control = CONTROL_PRIV_PSP;         // offset 108
  };
  // メソッド ABI (呼び出し 4 引数 + r12)。svc ラッパの ABI 確定 (Q1) までの暫定形。
  using method_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                 uintptr_t);

  // 例外フレーム実サイズ: 基本 32B / FP 拡張 104B。EXC_RETURN bit4 で判別する。
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
  // 「今の実行が特権か」の自己申告 (CONTROL.nPRIV を実際に読む)。
  static bool current_priv() {
    uint32_t control;
    asm volatile("MRS %0, CONTROL" : "=r"(control));
    return (control & 1u) == 0;
  }
  // この文脈が次に復帰するときの特権状態 (適用は CTX_RESTORE)。
  static void set_priv(context_t &context, bool priv) {
    context.control = priv ? CONTROL_PRIV_PSP : CONTROL_UNPRIV_PSP;
  }
  static void stack_limit_set(context_t &context, uintptr_t limit) {
    context.psplim = (uint32_t)limit;
  }
  // ★ARCH SEAM — マルチコア atomic (DESIGN §14.5.3)。RP2350 (ARMv8-M) は
  // LDREX/STREX (= __atomic 系)。SIO ハードウェアスピンロックは E2 erratum で不可。
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
static_assert(shizuku::concepts::arch_requires<armv8m>,
              "armv8m does not satisfy the arch concept");

} // namespace archs
} // namespace shizuku
#endif // SHIZUKU_ARCHS_ARMV8M_HPP
