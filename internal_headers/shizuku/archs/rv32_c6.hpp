#ifndef SHIZUKU_ARCHS_RV32_C6_HPP
#define SHIZUKU_ARCHS_RV32_C6_HPP

#include <cstdint>
#include "shizuku/concepts/arch.hpp"
#include "ports/esp32/c6/pmp.hpp"

// ===========================================================================
//  ESP32-C6 (RV32IMAC 単核) 用 arch 骨格 (未完成・fail-closed)
// ===========================================================================
//  ★現在の到達点:
//    - RV32IMAC レジスタおよび trap frame / context レイアウトの定義
//    - PMP 設定 (pmp_codec) の統合
//    - 実機 M-mode trap entry、SYSTIMER 割込み、U-mode 切替は未実装。
//    - ファームウェア組み込み時の欺瞞的 no-op 動作を防ぐため、
//      SHIZUKU_C6_SKELETON_PERMITTED が定義されていない本番ビルドでは
//      明示的にコンパイルを拒否する (fail-closed)。

#if !defined(SHIZUKU_C6_SKELETON_PERMITTED)
#error "ESP32-C6 arch backend is incomplete (trap entry, SYSTIMER, and M-mode PMP ownership are under exploration). Build refused (fail-closed)."
#endif

namespace shizuku {
namespace archs {

class rv32_c6 {
public:
  // RV32 トラップフレーム (caller-saved registers + mepc + mstatus)
  // a0..a7 (10..17), t0..t6 (5..7, 28..31), ra (1)
  struct exception_frame_t {
    uint32_t ra;
    uint32_t t0, t1, t2;
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t t3, t4, t5, t6;
    uint32_t mepc;
    uint32_t mstatus;
  };
  static_assert(sizeof(exception_frame_t) == 72,
                "exception_frame_t must be exactly 72 bytes (18 RV32 words)");

  // スレッド文脈 (callee-saved registers: sp, s0..s11)
  struct context_t {
    exception_frame_t *sp = nullptr;
    uintptr_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    uintptr_t s4 = 0, s5 = 0, s6 = 0, s7 = 0;
    uintptr_t s8 = 0, s9 = 0, s10 = 0, s11 = 0;
    uintptr_t stack_limit = 0;
    bool privileged = true;
    uintptr_t region_base = 0;
    uintptr_t region_limit = 0;
  };

  using method_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                 uintptr_t);

  struct syscall_result {
    uintptr_t error;
    uintptr_t value;
  };

  static constexpr uint32_t CALL_HEADROOM = 256;
  static constexpr uint32_t EXC_FRAME_MAX_BYTES = sizeof(exception_frame_t);
  static constexpr uint32_t FAULT_CONTEXT_BYTES = sizeof(context_t);
  // ESP32-C6 TRM / RISC-V 特権仕様において、U-mode からの rdcycle (mcounteren.CY) の
  // 許可状態および実測は未検証のため、未完成 API を欺瞞的に成功させず false とする。
  static constexpr bool HAS_CYCLE_COUNTER = false;
  static constexpr uint32_t TIMER_MAX_CYCLES = 0xFFFFFFFF;
  static constexpr uint32_t TIMER_MIN_CYCLES = 100;

  static uint32_t exc_frame_bytes(const context_t &) {
    return static_cast<uint32_t>(sizeof(exception_frame_t));
  }

  static uintptr_t psp_after_return(const context_t &context) {
    return reinterpret_cast<uintptr_t>(context.sp) + sizeof(exception_frame_t);
  }

  static void normalize_frame(exception_frame_t &) {
    // RV32 ではハードウェアによる SP の奇数ワード自動調整等は存在しない
  }

  // syscall ABI: a0..a4 = frame.a0..frame.a4 (source/kernel/dispatch.cpp:209, 218-239 と整合)
  static uintptr_t arg(const exception_frame_t &frame, unsigned index) {
    switch (index) {
    case 0: return frame.a0;
    case 1: return frame.a1;
    case 2: return frame.a2;
    case 3: return frame.a3;
    case 4: return frame.a4;
    case 5: return frame.a5;
    case 6: return frame.a6;
    case 7: return frame.a7;
    default: return 0;
    }
  }

  static void set_args(exception_frame_t &frame, const uintptr_t *args) {
    frame.a0 = args[0];
    frame.a1 = args[1];
    frame.a2 = args[2];
    frame.a3 = args[3];
  }

  static void set_result(exception_frame_t &frame, uintptr_t error,
                         uintptr_t value) {
    frame.a0 = error;
    frame.a1 = value;
  }

  static void set_entry(exception_frame_t &frame, uintptr_t pc, uintptr_t lr) {
    frame.mepc = pc;
    frame.ra = lr;
  }

  static uintptr_t return_stub(); // 実装は rv32_ctx.S (未作成)

  static uintptr_t frame_pc(const exception_frame_t &frame) {
    return frame.mepc;
  }

  // M-mode / U-mode の特権状態の判定は現在未実装。
  // 欺瞞的な常時 true を返さず、未実装呼出しを fail-closed (リンク不能) とする。
  static bool current_priv();

  static void set_priv(context_t &context, bool privileged) {
    context.privileged = privileged;
  }

  static void stack_limit_set(context_t &context, uintptr_t limit) {
    context.stack_limit = limit;
  }

  static uintptr_t stack_limit(const context_t &context) {
    return context.stack_limit;
  }

  static void set_region_window(context_t &context, uintptr_t base,
                                uintptr_t limit) {
    context.region_base = base;
    context.region_limit = limit;
  }

  // RV32 A 拡張 (LR.W / SC.W) によるアトミック操作
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

#if defined(__riscv)
  static syscall_result syscall(uintptr_t number, uintptr_t a1 = 0,
                                uintptr_t a2 = 0, uintptr_t a3 = 0,
                                uintptr_t a4 = 0) {
    register uintptr_t r_a0 asm("a0") = number;
    register uintptr_t r_a1 asm("a1") = a1;
    register uintptr_t r_a2 asm("a2") = a2;
    register uintptr_t r_a3 asm("a3") = a3;
    register uintptr_t r_a4 asm("a4") = a4;
    asm volatile("ecall"
                 : "+r"(r_a0), "+r"(r_a1)
                 : "r"(r_a2), "r"(r_a3), "r"(r_a4)
                 : "memory");
    return {r_a0, r_a1};
  }
#else
  // 非-RISC-V 環境でのホスト呼出しを成功に見せる no-op は禁止。未対応時リンク不能とする。
  static syscall_result syscall(uintptr_t number, uintptr_t a1 = 0,
                                uintptr_t a2 = 0, uintptr_t a3 = 0,
                                uintptr_t a4 = 0);
#endif

  static void prepare_thread_entry(context_t &context, uintptr_t stack_top,
                                   uintptr_t entry_pc, uintptr_t return_pc,
                                   uintptr_t argument) {
    auto *frame = reinterpret_cast<exception_frame_t *>(
        (stack_top - sizeof(exception_frame_t)) & ~static_cast<uintptr_t>(7));
    frame->ra = return_pc;
    frame->mepc = entry_pc;
    frame->a0 = argument;
    frame->a1 = frame->a2 = frame->a3 = frame->a4 = 0;
    // MPP = U-mode (0) または M-mode (3) を準備 (未完成)
    frame->mstatus = 0;
    context.sp = frame;
  }

  static void timer_oneshot(uint32_t); // 未完成
  static void timer_cancel();          // 未完成
  static uint32_t timer_remaining(bool &wrapped); // 未完成
  static void pend_context_switch();   // 未完成

  // ハードウェアデバッグおよびフォールト状態 (未完成)
  static uint32_t debug_reason_take();
  static void debug_step(bool on);
  static bool faulted_in_thread_mode(const context_t &context);
  static uint32_t fault_status();
  static uintptr_t fault_address();
  static void fault_status_clear();

  [[noreturn]] static void enter_thread_mode(uintptr_t stack_top,
                                             uintptr_t stack_limit,
                                             void (*entry)());
};

static_assert(shizuku::concepts::arch_requires<rv32_c6>,
              "rv32_c6 must satisfy the arch concept");

} // namespace archs
} // namespace shizuku

#endif // SHIZUKU_ARCHS_RV32_C6_HPP
