#ifndef SHIZUKU_CHIPS_RP2040_CHIP_HPP
#define SHIZUKU_CHIPS_RP2040_CHIP_HPP
// RP2040 (SoC) の SDK 非依存の定義。出典は pico-sdk (local checkout) の値。
// ★pico-sdk のヘッダを include しない (chips は SDK に依存しない)。
// ★名前は k 接頭辞にする。SDK の XIP_BASE / SRAM_END 等はマクロなので、
//   SDK ヘッダと同じ翻訳単位で使うと定義が展開されて壊れる。
#include <cstdint>

namespace shizuku::chips::rp2040 {
struct chip {
  // RP2040 は Cortex-M0+ x2
  static constexpr uint32_t kCoreCount = 2;

  // ---- メモリ地図 (出典: src/rp2040/hardware_regs/include/hardware/regs/addressmap.h)
  static constexpr uintptr_t kXipBase = 0x10000000u;
  static constexpr uintptr_t kSramBase = 0x20000000u;
  // SRAM の 256KiB 連続部分の終端 (ストライプ全体の終端は kSramEnd)
  static constexpr uintptr_t kSramStripedEnd = 0x20040000u;
  static constexpr uintptr_t kSramEnd = 0x20042000u;
  // MPU で張る SRAM の上限 = kSramEnd - 32。
  // 出典: 既存 board の値 0x20041FE0 と同値。-32 は PMSAv6 の region 整列単位 (32B)
  // に合わせた手前の位置で、ハードウェアの境界というより「region を張る上限」の選択。
  static constexpr uintptr_t kSramRegionEnd = kSramEnd - 32u;

  // ---- 割込み優先度 (NVIC) ----
  // 実装されている優先度 bit 数。RP2040 は上位 2 bit だけ。
  // 出典: src/rp2_common/cmsis/stub/CMSIS/Device/RP2040/Include/RP2040.h:87 (__NVIC_PRIO_BITS=2)
  static constexpr uint32_t kNvicPriorityBits = 2;
  // 生バイトの値。4 段のみ。順序は syscall (SVC) > SysTick > USB > PendSV。
  // HardFault は ARMv6-M で固定優先度 -1 のため USB より下には置けない (board 側の事情)。
  static constexpr uint8_t kPrioritySvc = 0x00;
  static constexpr uint8_t kPrioritySysTick = 0x40;
  static constexpr uint8_t kPriorityPendSv = 0xC0;
  static constexpr uint8_t kPriorityUsbCtrl = 0x80;

  // ---- MPU ----
  // 出典: M0+ MPU_TYPE.DREGION の既定値 8 (src/rp2040/hardware_regs/include/hardware/regs/m0plus.h:966)
  static constexpr uint32_t kMpuRegionCount = 8;

  // ---- SIO (コア番号) ----
  // 出典: addressmap.h の SIO_BASE、sio.h の SIO_CPUID_OFFSET (0)。
  // SDK の get_core_num() は *(SIO_BASE + SIO_CPUID_OFFSET) を読むだけなので同じ読み出し。
  static constexpr uintptr_t kSioBase = 0xD0000000u;
  static constexpr uintptr_t kSioCpuidOffset = 0x0u;
  static uint32_t core_id() {
    return *reinterpret_cast<volatile uint32_t *>(kSioBase + kSioCpuidOffset);
  }

  // ---- atomic 用の hardware spin lock ID ----
  // 出典: src/rp2_common/hardware_sync_spin_lock/include/hardware/sync/spin_lock.h:42
  //   (#ifndef PICO_SPINLOCK_ID_ATOMIC、既定 13)。上書き可能なので、SDK の実値との
  //   一致は modules/pico_sdk_support/rp2040/arch_glue_v6m.cpp の static_assert で固定する。
  static constexpr uint32_t kAtomicSpinlockId = 13;
};
} // namespace shizuku::chips::rp2040

#endif // SHIZUKU_CHIPS_RP2040_CHIP_HPP
