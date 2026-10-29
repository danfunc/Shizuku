// サンプル: chip = SoC の「数」だけ。関数はコア番号の読み出しのような 1 行のものに限る。
// 値の出典は pico-sdk と RP2350 データシート (cpu_reference.typ の表を参照)。
#ifndef SHIZUKU_SAMPLE_CHIP_RP2350_HPP
#define SHIZUKU_SAMPLE_CHIP_RP2350_HPP
#include <stdint.h>
#include "../arch/armv8m.hpp"

namespace sample::chips {

struct rp2350 {
  using arch = sample::arch::armv8m</*MPU_REGIONS*/ 8, /*PRIORITY_BITS*/ 4>;
  static constexpr uint32_t CORE_COUNT = 2;

  // ---- メモリ地図 ----
  static constexpr uintptr_t XIP_BASE = 0x10000000;
  static constexpr uintptr_t SRAM_BASE = 0x20000000;
  static constexpr uintptr_t SRAM_END = 0x20082000; // 520KiB

  // ---- 優先度 (0 が最も高い。上位 4bit だけ効く) ----
  // カーネルの不変条件 SVC > タイマ > 切替え は cpu_manager が static_assert する。
  // chip が決めるのは、その間にどの SoC 割込みを挟むかだけ。
  struct priority {
    static constexpr uint8_t SVC = 0x00;
    static constexpr uint8_t TIMER = 0x40;
    static constexpr uint8_t FAULT = 0x80;     // USB (0x20) より下: 報告中も USB が回る
    static constexpr uint8_t DEBUG = 0xA0;
    static constexpr uint8_t SWITCH = 0xFF;    // PendSV は最低
  };

  // ---- SoC 割込み ----
  static constexpr uint32_t IRQ_USBCTRL = 14;
  static constexpr uint32_t IRQ_SIO_FIFO_CORE0 = 25; // multicore lockout (flash 書込み中の停止)

  static uint32_t core_id() { return *reinterpret_cast<volatile uint32_t *>(0xD0000000); }
};

} // namespace sample::chips
#endif
