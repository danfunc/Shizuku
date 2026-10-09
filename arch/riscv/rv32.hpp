#ifndef SHIZUKU_ARCH_RISCV_RV32_HPP
#define SHIZUKU_ARCH_RISCV_RV32_HPP

#include <cstddef>
#include <cstdint>
#include "arch/riscv/pmp.hpp"
#include "arch/riscv/rv32_frame.h"
#include "shizuku/concepts/arch.hpp"

// 共通の RV32 trap 経路 (arch/riscv/rv32_trap.S) の入口・戻り口。
// dispatch と failure は chips 側の C++ が定義する (shizuku_rv32_trap_dispatch / shizuku_rv32_return_failed)。
extern "C" {
void shizuku_rv32_return_stub();
[[noreturn]] void shizuku_rv32_first_return(void *frame);
}

namespace shizuku::archs {

// RV32 の汎用 arch。チップ差は 3 つの型引数だけで決まる。
//   PMP_ENTRIES      : 物理 PMP エントリ数 (チップが決める。未確定のチップはここに値を置かない)
//   PMP_GRANULE_BYTES: PMP 粒度をバイト数で渡す (4 / 128)。仕様の cfg.G (ビット数) ではない
//   IntMode          : 割込み/タイマ/スレッド起動など、チップ・割込み方式に依る処理の policy
//                      (chips 側。このヘッダは chips を include しない)
template <unsigned PMP_ENTRIES, unsigned PMP_GRANULE_BYTES, class IntMode>
class rv32 {
  static_assert(PMP_ENTRIES > 0 && PMP_ENTRIES <= 64,
                "PMP entry count out of the RV32 range");
  static_assert(PMP_GRANULE_BYTES >= 4 && (PMP_GRANULE_BYTES & (PMP_GRANULE_BYTES - 1)) == 0,
                "PMP granule must be a power of two and at least 4 bytes");

public:
  static constexpr unsigned PMP_ENTRY_COUNT = PMP_ENTRIES;
  static constexpr unsigned PMP_GRANULE = PMP_GRANULE_BYTES;
  using pmp_codec_t = ports::riscv::pmp_codec<PMP_GRANULE_BYTES>;
  using interrupt_mode = IntMode;

  // Caller registers retain their old offsets. Full register image supports
  // nested M-mode traps; s0..s11 are mirrored into the V2 context at ECALL.
  struct exception_frame_t {
    uint32_t ra, t0, t1, t2;
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t t3, t4, t5, t6, mepc, mstatus, mcause, mtval;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint32_t gp, tp, reserved0, reserved1;
  };
  static_assert(sizeof(exception_frame_t) == RV32_FRAME_BYTES);
  static_assert(offsetof(exception_frame_t, ra) == RV32_F_RA);
  static_assert(offsetof(exception_frame_t, t0) == RV32_F_T0);
  static_assert(offsetof(exception_frame_t, t1) == RV32_F_T1);
  static_assert(offsetof(exception_frame_t, t2) == RV32_F_T2);
  static_assert(offsetof(exception_frame_t, a0) == RV32_F_A0);
  static_assert(offsetof(exception_frame_t, a1) == RV32_F_A1);
  static_assert(offsetof(exception_frame_t, a2) == RV32_F_A2);
  static_assert(offsetof(exception_frame_t, a3) == RV32_F_A3);
  static_assert(offsetof(exception_frame_t, a4) == RV32_F_A4);
  static_assert(offsetof(exception_frame_t, a5) == RV32_F_A5);
  static_assert(offsetof(exception_frame_t, a6) == RV32_F_A6);
  static_assert(offsetof(exception_frame_t, a7) == RV32_F_A7);
  static_assert(offsetof(exception_frame_t, t3) == RV32_F_T3);
  static_assert(offsetof(exception_frame_t, t4) == RV32_F_T4);
  static_assert(offsetof(exception_frame_t, t5) == RV32_F_T5);
  static_assert(offsetof(exception_frame_t, t6) == RV32_F_T6);
  static_assert(offsetof(exception_frame_t, mepc) == RV32_F_MEPC);
  static_assert(offsetof(exception_frame_t, mstatus) == RV32_F_MSTATUS);
  static_assert(offsetof(exception_frame_t, mcause) == RV32_F_MCAUSE);
  static_assert(offsetof(exception_frame_t, mtval) == RV32_F_MTVAL);
  static_assert(offsetof(exception_frame_t, s0) == RV32_F_S0);
  static_assert(offsetof(exception_frame_t, s1) == RV32_F_S1);
  static_assert(offsetof(exception_frame_t, s2) == RV32_F_S2);
  static_assert(offsetof(exception_frame_t, s3) == RV32_F_S3);
  static_assert(offsetof(exception_frame_t, s4) == RV32_F_S4);
  static_assert(offsetof(exception_frame_t, s5) == RV32_F_S5);
  static_assert(offsetof(exception_frame_t, s6) == RV32_F_S6);
  static_assert(offsetof(exception_frame_t, s7) == RV32_F_S7);
  static_assert(offsetof(exception_frame_t, s8) == RV32_F_S8);
  static_assert(offsetof(exception_frame_t, s9) == RV32_F_S9);
  static_assert(offsetof(exception_frame_t, s10) == RV32_F_S10);
  static_assert(offsetof(exception_frame_t, s11) == RV32_F_S11);
  static_assert(offsetof(exception_frame_t, gp) == RV32_F_GP);
  static_assert(offsetof(exception_frame_t, tp) == RV32_F_TP);
  static_assert(offsetof(exception_frame_t, reserved0) == RV32_F_RESERVED0);
  static_assert(offsetof(exception_frame_t, reserved1) == RV32_F_RESERVED1);

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

  // 以下の policy 値は IntMode (チップ・割込み方式側) から受ける。generic は値を持たない。
  static constexpr uint32_t CALL_HEADROOM = IntMode::CALL_HEADROOM;
  static constexpr uint32_t EXC_FRAME_MAX_BYTES = sizeof(exception_frame_t);
  static constexpr uint32_t FAULT_CONTEXT_BYTES = sizeof(context_t);
  static constexpr bool HAS_CYCLE_COUNTER = IntMode::HAS_CYCLE_COUNTER;
  static constexpr uint32_t TIMER_MAX_CYCLES = IntMode::TIMER_MAX_CYCLES;
  static constexpr uint32_t TIMER_MIN_CYCLES = IntMode::TIMER_MIN_CYCLES;

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

  static uintptr_t return_stub() {
    return reinterpret_cast<uintptr_t>(&shizuku_rv32_return_stub);
  }
  // trap 深さの問い合わせは IntMode に委ねる。ECALL は割込みではない。
  static bool in_interrupt() { return IntMode::in_interrupt(); }

  static uintptr_t frame_pc(const exception_frame_t &frame) {
    return frame.mepc;
  }

  // 特権の判定は IntMode に委ねる (trap 内の保存 MPP など)。
  static bool current_priv() { return IntMode::current_priv(); }

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
    frame->mstatus = context.privileged ? RV32_MSTATUS_MPP : 0;
    // MPIE stays clear until the interrupt matrix and timer are owned.
    context.sp = frame;
  }

  // タイマ・割込み方式の処理は IntMode (チップ側) に委ねる。
  static void timer_oneshot(uint32_t cycles) { IntMode::timer_oneshot(cycles); }
  static void timer_cancel() { IntMode::timer_cancel(); }
  static uint32_t timer_remaining(bool &wrapped) {
    return IntMode::timer_remaining(wrapped);
  }
  static void pend_context_switch() { IntMode::pend_context_switch(); }

  // デバッグおよびフォールト状態は IntMode に委ねる
  static uint32_t debug_reason_take() { return IntMode::debug_reason_take(); }
  static void debug_step(bool on) { IntMode::debug_step(on); }
  static bool faulted_in_thread_mode(const context_t &context) {
    return IntMode::faulted_in_thread_mode(context);
  }
  static uint32_t fault_status() { return IntMode::fault_status(); }
  static uintptr_t fault_address() { return IntMode::fault_address(); }
  static void fault_status_clear() { IntMode::fault_status_clear(); }

  [[noreturn]] static void enter_thread_mode(uintptr_t stack_top,
                                             uintptr_t stack_limit,
                                             void (*entry)()) {
    IntMode::enter_thread_mode(stack_top, stack_limit, entry);
  }
};

} // namespace shizuku::archs

#endif // SHIZUKU_ARCH_RISCV_RV32_HPP
