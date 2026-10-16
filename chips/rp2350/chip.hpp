#ifndef SHIZUKU_CHIPS_RP2350_CHIP_HPP
#define SHIZUKU_CHIPS_RP2350_CHIP_HPP
// RP2350 (SoC) の SDK 非依存の定義。出典は pico-sdk (local checkout) の値。
// ★pico-sdk のヘッダを include しない (chips は SDK に依存しない)。
// ★名前は k 接頭辞にする (SDK の XIP_BASE / SRAM_END はマクロ)。
#include <cstdint>

#ifndef SHIZUKU_RAM_TEXT
#define SHIZUKU_RAM_TEXT __attribute__((section(".time_critical.text")))
#endif

namespace shizuku::chips::rp2350 {
struct chip {
  // RP2350 は Cortex-M33 x2 (この構成)
  static constexpr uint32_t kCoreCount = 2;

  // ---- メモリ地図 (出典: src/rp2350/hardware_regs/include/hardware/regs/addressmap.h)
  static constexpr uintptr_t kXipBase = 0x10000000u;
  static constexpr uintptr_t kSramBase = 0x20000000u;
  static constexpr uintptr_t kSramStripedEnd = 0x20080000u;
  static constexpr uintptr_t kSramEnd = 0x20082000u;
  // MPU で張る SRAM の上限 = kSramEnd - 32。RP2040 と同じ選択 (PMSAv8 の 32B 整列手前)。
  static constexpr uintptr_t kSramRegionEnd = kSramEnd - 32u;

  // ---- 割込み優先度 (NVIC) ----
  // 実装 bit 数: src/rp2_common/cmsis/stub/CMSIS/Device/RP2350/Include/RP2350.h:112 (__NVIC_PRIO_BITS=4)
  // ★差し戻し文の「3」ではなく SDK の device header の値 4 を採る。
  static constexpr uint32_t kNvicPriorityBits = 4;
  // 生バイトの値。順序は SVC > SysTick > USB > fault > DebugMon > PendSV (最低)。
  static constexpr uint8_t kPrioritySvc = 0x00;
  static constexpr uint8_t kPrioritySysTick = 0x40;
  // 出典: src/rp2_common/hardware_irq/include/hardware/irq.h:166 (PICO_LOWEST_IRQ_PRIORITY 0xff、無条件 #define)
  static constexpr uint8_t kPriorityLowest = 0xFF;
  static constexpr uint8_t kPriorityUsbCtrl = 0x20;
  static constexpr uint8_t kPriorityFault = 0x80;
  static constexpr uint8_t kPriorityDebugMon = 0xA0;

  // ---- MPU ----
  // 出典: src/rp2350/hardware_regs/include/hardware/platform_defs.h:61 (NUM_MPU_REGIONS=8)
  static constexpr uint32_t kMpuRegionCount = 8;

  // ---- SIO (コア番号) ----
  // 出典: addressmap.h の SIO_BASE (0xd0000000)、sio.h の SIO_CPUID_OFFSET (0)。
  static constexpr uintptr_t kSioBase = 0xD0000000u;
  static constexpr uintptr_t kSioCpuidOffset = 0x0u;
  static uint32_t core_id() {
    return *reinterpret_cast<volatile uint32_t *>(kSioBase + kSioCpuidOffset);
  }
};
} // namespace shizuku::chips::rp2350

#endif // SHIZUKU_CHIPS_RP2350_CHIP_HPP
