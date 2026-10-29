// サンプル: RP2040 の chip。形は rp2350.hpp と同じ。
#ifndef SHIZUKU_SAMPLE_CHIP_RP2040_HPP
#define SHIZUKU_SAMPLE_CHIP_RP2040_HPP
#include <stdint.h>
#include "../arch/armv6m.hpp"

namespace sample::chips {

struct rp2040 {
  using arch = sample::arch::armv6m</*MPU_REGIONS*/ 8, /*PRIORITY_BITS*/ 2>;
  static constexpr uint32_t CORE_COUNT = 2;

  static constexpr uintptr_t XIP_BASE = 0x10000000;
  static constexpr uintptr_t SRAM_BASE = 0x20000000;
  static constexpr uintptr_t SRAM_END = 0x20042000; // 264KiB

  // 優先度は 4 段 (0x00/0x40/0x80/0xC0) しかない。
  // ★SIO FIFO (multicore lockout) を SVC より上に置く。flash を書く側のコアが、
  //   SVC の中にいる相手を止められるようにするため。よって SVC は 0x40。
  struct priority {
    static constexpr uint8_t SIO_FIFO = 0x00;
    static constexpr uint8_t SVC = 0x40;
    static constexpr uint8_t TIMER = 0x80;
    static constexpr uint8_t USB = 0x80;
    static constexpr uint8_t SWITCH = 0xC0;
  };

  static constexpr uint32_t IRQ_USBCTRL = 5;
  static constexpr uint32_t IRQ_SIO_PROC0 = 15;

  static uint32_t core_id() { return *reinterpret_cast<volatile uint32_t *>(0xD0000000); }
};

} // namespace sample::chips
#endif
