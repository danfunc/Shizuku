#ifndef SHIZUKU_ARCHS_RV32_C6_HPP
#define SHIZUKU_ARCHS_RV32_C6_HPP

#include <cstdint>
#include <cstddef>
#include "ports/esp32/c6/frame_layout.h"
#include "shizuku/concepts/arch.hpp"
#include "ports/esp32/c6/pmp.hpp"

// ===========================================================================
//  ESP32-C6 (RV32IMAC 単核) 用 arch 骨格 (未完成・fail-closed)
// ===========================================================================
//  ★現在の到達点:
//    - RV32IMAC レジスタおよび trap frame / context レイアウトの定義
//    - PMP 設定 (pmp_codec) の統合
//    - M-only trap/ECALL と初回 mret を実装。SYSTIMER/PMP/U-mode は未実装。
//    - ファームウェア組み込み時の欺瞞的 no-op 動作を防ぐため、
//      SHIZUKU_C6_SKELETON_PERMITTED が定義されていない本番ビルドでは
//      明示的にコンパイルを拒否する (fail-closed)。

#if !defined(SHIZUKU_C6_SKELETON_PERMITTED)
#error "ESP32-C6 arch backend is incomplete (SYSTIMER, interrupt matrix, and PMP ownership remain incomplete). Build refused (fail-closed)."
#endif

namespace shizuku {
namespace archs {

class rv32_c6 {
public:
  // Caller registers retain their old offsets. Full register image supports
  // nested M-mode traps; s0..s11 are mirrored into the V2 context at ECALL.
  struct exception_frame_t {
    uint32_t ra, t0, t1, t2;
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t t3, t4, t5, t6, mepc, mstatus, mcause, mtval;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint32_t gp, tp, reserved0, reserved1;
  };
  static_assert(sizeof(exception_frame_t) == C6_FRAME_BYTES);
  static_assert(offsetof(exception_frame_t, ra) == C6_F_RA);
  static_assert(offsetof(exception_frame_t, t0) == C6_F_T0);
  static_assert(offsetof(exception_frame_t, t1) == C6_F_T1);
  static_assert(offsetof(exception_frame_t, t2) == C6_F_T2);
  static_assert(offsetof(exception_frame_t, a0) == C6_F_A0);
  static_assert(offsetof(exception_frame_t, a1) == C6_F_A1);
  static_assert(offsetof(exception_frame_t, a2) == C6_F_A2);
  static_assert(offsetof(exception_frame_t, a3) == C6_F_A3);
  static_assert(offsetof(exception_frame_t, a4) == C6_F_A4);
  static_assert(offsetof(exception_frame_t, a5) == C6_F_A5);
  static_assert(offsetof(exception_frame_t, a6) == C6_F_A6);
  static_assert(offsetof(exception_frame_t, a7) == C6_F_A7);
  static_assert(offsetof(exception_frame_t, t3) == C6_F_T3);
  static_assert(offsetof(exception_frame_t, t4) == C6_F_T4);
  static_assert(offsetof(exception_frame_t, t5) == C6_F_T5);
  static_assert(offsetof(exception_frame_t, t6) == C6_F_T6);
  static_assert(offsetof(exception_frame_t, mepc) == C6_F_MEPC);
  static_assert(offsetof(exception_frame_t, mstatus) == C6_F_MSTATUS);
  static_assert(offsetof(exception_frame_t, mcause) == C6_F_MCAUSE);
  static_assert(offsetof(exception_frame_t, mtval) == C6_F_MTVAL);
  static_assert(offsetof(exception_frame_t, s0) == C6_F_S0);
  static_assert(offsetof(exception_frame_t, s1) == C6_F_S1);
  static_assert(offsetof(exception_frame_t, s2) == C6_F_S2);
  static_assert(offsetof(exception_frame_t, s3) == C6_F_S3);
  static_assert(offsetof(exception_frame_t, s4) == C6_F_S4);
  static_assert(offsetof(exception_frame_t, s5) == C6_F_S5);
  static_assert(offsetof(exception_frame_t, s6) == C6_F_S6);
  static_assert(offsetof(exception_frame_t, s7) == C6_F_S7);
  static_assert(offsetof(exception_frame_t, s8) == C6_F_S8);
  static_assert(offsetof(exception_frame_t, s9) == C6_F_S9);
  static_assert(offsetof(exception_frame_t, s10) == C6_F_S10);
  static_assert(offsetof(exception_frame_t, s11) == C6_F_S11);
  static_assert(offsetof(exception_frame_t, gp) == C6_F_GP);
  static_assert(offsetof(exception_frame_t, tp) == C6_F_TP);
  static_assert(offsetof(exception_frame_t, reserved0) == C6_F_RESERVED0);
  static_assert(offsetof(exception_frame_t, reserved1) == C6_F_RESERVED1);

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

  static uintptr_t return_stub();
  // Provisional V3-facing query, single HP hart only. ECALL is not an IRQ.
  static bool in_interrupt();

  static uintptr_t frame_pc(const exception_frame_t &frame) {
    return frame.mepc;
  }

  // M-only pilot: trap 内では保存した MPP、trap 外では M-mode を返す。
  // U-mode 実復帰は PMP/trusted entry stack の実装まで拒否する。
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
        (stack_top & ~static_cast<uintptr_t>(15)) - sizeof(exception_frame_t));
    *frame = {};
#if defined(__riscv)
    // New RV32 contexts retain the kernel's global-pointer/TLS ABI.
    asm volatile("mv %0, gp\n mv %1, tp" : "=r"(frame->gp), "=r"(frame->tp));
#endif
    frame->ra = return_pc;
    frame->mepc = entry_pc;
    frame->a0 = argument;
    frame->a1 = frame->a2 = frame->a3 = frame->a4 = 0;
    frame->mstatus = context.privileged ? C6_MSTATUS_MPP : 0;
    // MPIE stays clear until the interrupt matrix and timer are owned.
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
