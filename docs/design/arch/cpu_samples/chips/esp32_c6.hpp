// サンプル: ESP32-C6 の chip。RISC-V の仕様の外にあるもの (割込み行列、PLIC、SYSTIMER) はここ。
#ifndef SHIZUKU_SAMPLE_CHIP_ESP32_C6_HPP
#define SHIZUKU_SAMPLE_CHIP_ESP32_C6_HPP
#include <stdint.h>
#include "../arch/rv32.hpp"

namespace sample::chips {

struct esp32_c6 {
  using arch = sample::arch::rv32</*PMP_ENTRIES*/ 16, /*PMP_GRANULE*/ 4>;
  static constexpr uint32_t CORE_COUNT = 1; // HP コア。LP コアはカーネルの外

  static constexpr uintptr_t HP_SRAM_BASE = 0x40800000;
  static constexpr uintptr_t HP_SRAM_END = 0x40880000; // 512KiB

  // ---- 割込み: 周辺の source を割込み行列で CPU 割込み 1..31 へ写し、PLIC で優先度を付ける ----
  static constexpr uintptr_t INTMTX_BASE = 0x60010000;
  static constexpr uintptr_t SYSTIMER_BASE = 0x6000A000;
  static constexpr uint32_t SOURCE_SYSTIMER_TARGET0 = 57;
  static constexpr uint32_t CPU_IRQ_TIMER = 17; // chips/esp32_c6/systimer.hpp:21 と同じ

  struct priority {
    // RISC-V では ECALL は例外 (同期) なので優先度を持たない。trap 中は MIE=0 なので
    // 「SVC 最優先」は構造で成り立つ。タイマだけ PLIC の優先度を付ける。
    static constexpr uint8_t TIMER = 3;     // PLIC は priority > threshold で通す
    static constexpr uint8_t THRESHOLD = 1; // IDF の RVHAL_INTR_ENABLE_THRESH と同じ
  };

  // SYSTIMER の 1 本を oneshot に使う (16MHz の 52bit カウンタ)。実装は chip の .cpp。
  static void timer_oneshot(uint32_t ticks);
  static void timer_cancel();
  static void interrupt_init(); // 行列の写像・PLIC の優先度と閾値・mie

  // マシンの拡張 CSR: mpcer(0x7E0) / mpcmr(0x7E1) / mpccr(0x7E2) でサイクル数を数える。
  static bool cycle_counter_enable() {
    asm volatile("csrw 0x7E0, %0\n csrw 0x7E1, %0" ::"r"(1u));
    return true;
  }
  static uint32_t cycle_counter() {
    uint32_t value;
    asm volatile("csrr %0, 0x7E2" : "=r"(value));
    return value;
  }
};

} // namespace sample::chips
#endif
