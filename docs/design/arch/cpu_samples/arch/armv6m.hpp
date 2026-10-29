// サンプル: ARMv6-M の arch 層。armv8m.hpp と同じ名前の操作を、v6-M でできる形で持つ。
// 無いものは HAS_* を false にし、空の操作を置く。呼ぶ側 (cpu_manager) は
// if constexpr で分けるので #ifdef は要らない。
#ifndef SHIZUKU_SAMPLE_ARCH_ARMV6M_HPP
#define SHIZUKU_SAMPLE_ARCH_ARMV6M_HPP
#include <stdint.h>
#include "../common/kernel_entry.hpp"

extern "C" void sample_armv6m_svc_entry();
extern "C" void sample_armv6m_pendsv_entry();
extern "C" void sample_armv6m_fault_entry();
extern "C" [[noreturn]] void sample_armv6m_enter_thread_mode(uintptr_t top, uintptr_t limit,
                                                             void (*entry)());

namespace sample::arch {

template <unsigned MPU_REGIONS_T, unsigned PRIORITY_BITS_T> struct armv6m {
  static constexpr unsigned MPU_REGIONS = MPU_REGIONS_T;
  static constexpr unsigned PRIORITY_BITS = PRIORITY_BITS_T;
  static constexpr bool HAS_CYCLE_COUNTER = false;  // DWT CYCCNT が無い
  static constexpr bool HAS_STACK_LIMIT = false;    // PSPLIM が無い。下限はソフトで検査する
  static constexpr bool HAS_FAULT_PRIORITY = false; // HardFault だけ (優先度 -1 固定)
  static constexpr bool HAS_PRIORITY_REGISTERS = true;

  struct exception_frame_t {
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
  };
  struct context_t {
    uintptr_t sp;
    uint32_t exc_return;
  };
  enum exception : uint32_t { HARD_FAULT = 3, SVCALL = 11, PENDSV = 14, SYSTICK = 15 };

  static volatile uint32_t &reg(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
  }
  static void install_vectors() {
    auto *table = reinterpret_cast<void (**)()>(reg(0xE000ED08));
    table[SVCALL] = sample_armv6m_svc_entry;
    table[PENDSV] = sample_armv6m_pendsv_entry;
    table[SYSTICK] = shizuku_tick;
    table[HARD_FAULT] = sample_armv6m_fault_entry;
  }
  // v6-M の SHPR2/3 はワードアクセスのみ。該当バイトだけを書き換える。
  //   SVCall = SHPR2[31:24], PendSV = SHPR3[23:16], SysTick = SHPR3[31:24]
  static uint32_t shpr_address(exception number) { return number == SVCALL ? 0xE000ED1C : 0xE000ED20; }
  static uint32_t shpr_shift(exception number) { return 8u * (number % 4u); }
  static void set_priority(exception number, uint8_t priority) {
    auto &shpr = reg(shpr_address(number));
    const uint32_t shift = shpr_shift(number);
    shpr = (shpr & ~(0xFFu << shift)) | (uint32_t(priority) << shift);
  }
  static uint8_t priority(exception number) {
    return uint8_t(reg(shpr_address(number)) >> shpr_shift(number));
  }
  static void set_irq_priority(uint32_t irq, uint8_t priority) {
    auto &ipr = reg(0xE000E400 + 4u * (irq / 4u));
    const uint32_t shift = 8u * (irq % 4u);
    ipr = (ipr & ~(0xFFu << shift)) | (uint32_t(priority) << shift);
  }
  static void enable_fault_exceptions() {}

  static void pend_switch() { reg(0xE000ED04) = 1u << 28; }
  static constexpr uint32_t TIMER_MAX = 0x00FFFFFF;
  static void timer_oneshot(uint32_t cycles) {
    reg(0xE000E010) = 0;
    reg(0xE000E014) = cycles - 1;
    reg(0xE000E018) = 0;
    reg(0xE000E010) = 0b111;
  }
  static void timer_cancel() { reg(0xE000E010) = 0; }

  static bool cycle_counter_enable() { return false; }
  static uint32_t cycle_counter() { return 0; }

  static uint32_t control() {
    uint32_t value;
    asm volatile("mrs %0, control" : "=r"(value));
    return value;
  }
  static bool privileged() { return (control() & 1u) == 0; }
  static bool fp_active() { return false; } // FPU が無い
  static void apply_privilege(context_t &, bool privileged) {
    uint32_t value = control();
    value = privileged ? (value & ~1u) : (value | 1u);
    asm volatile("msr control, %0\n isb" ::"r"(value) : "memory");
  }
  static void set_stack_limit(uintptr_t) {}

  // ---- MPU (PMSAv6) ---------------------------------------------------------
  // region の大きさは 2 の冪 (256B 以上) で、base はその大きさに整列する。8 分割の
  // subregion を無効化して穴を作れる。張れない範囲は false を返す (黙って広げない)。
  enum access : uint32_t { RW_PRIV = 0b001, RW_ALL = 0b011, RO_PRIV = 0b101, RO_ALL = 0b110 };
  static bool region_set(uint32_t index, uintptr_t base, uintptr_t end_exclusive, access ap,
                         bool execute) {
    const uintptr_t size = end_exclusive - base;
    if (size < 256 || (size & (size - 1)) != 0 || (base & (size - 1)) != 0)
      return false;
    const uint32_t size_field = 31u - uint32_t(__builtin_clz(size)) - 1u; // size = 2^(SIZE+1)
    reg(0xE000ED98) = index;                                               // RNR
    reg(0xE000ED9C) = base;                                                // RBAR
    reg(0xE000EDA0) = (execute ? 0u : 1u << 28) | (ap << 24) | (size_field << 1) | 1u; // RASR
    return true;
  }
  static void region_disable(uint32_t index) {
    reg(0xE000ED98) = index;
    reg(0xE000EDA0) = 0;
  }
  static void protection_enable() {
    reg(0xE000ED94) = 0b101; // PRIVDEFENA | ENABLE
    asm volatile("dsb\n isb" ::: "memory");
  }

  [[noreturn]] static void enter_thread_mode(uintptr_t top, uintptr_t limit, void (*entry)()) {
    sample_armv6m_enter_thread_mode(top, limit, entry);
  }
};

} // namespace sample::arch
#endif
