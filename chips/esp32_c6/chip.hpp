#ifndef SHIZUKU_CHIPS_ESP32_C6_CHIP_HPP
#define SHIZUKU_CHIPS_ESP32_C6_CHIP_HPP
// ESP32-C6 (SoC) の型の入口。config.hpp の CHIP 別名がこれを指す (configs/kit.bzl)。
// ★ESP-IDF / pico-sdk を一切 include しない (chips の依存閉包はそれを含まない)。
// ★fail-closed gate (SHIZUKU_C6_SKELETON_PERMITTED) はここに置かない。rv32_c6.hpp の互換入口に残す。
#include <cstdint>
#include "arch/riscv/rv32.hpp"

#ifndef SHIZUKU_RAM_TEXT
#define SHIZUKU_RAM_TEXT __attribute__((section(".iram1.text")))
#endif

namespace shizuku::chips::esp32_c6 {

// C6 の割込み方式 (PLIC + 256B 整列 32 スロットの vector table)。
// rv32<...>::IntMode としてだけ使う。定義は trap.cpp (本体) と modules/esp_idf_support/c6/link_stubs.cpp (未実装部)。
// 将来 C5 (CLIC 直接エントリ + mtvt 関数ポインタ表) を載せるなら、別の IntMode 型を同じ形で作る。
struct plic_vector_mode {
  // M-only pilot の policy 値 (従来値のまま。generic arch からは IntMode 経由で参照)。
  static constexpr uint32_t CALL_HEADROOM = 256;
  // ESP32-C6 TRM / RISC-V 特権仕様で U-mode からの rdcycle (mcounteren.CY) は未検証のため false。
  static constexpr bool HAS_CYCLE_COUNTER = false;
  static constexpr uint32_t TIMER_MAX_CYCLES = 0xFFFFFFFF;
  static constexpr uint32_t TIMER_MIN_CYCLES = 100;

  static bool in_interrupt();
  static bool current_priv();
  static void timer_oneshot(uint32_t cycles);
  static void timer_cancel();
  static uint32_t timer_remaining(bool &wrapped);
  static void pend_context_switch();
  [[noreturn]] static void enter_thread_mode(uintptr_t stack_top,
                                             uintptr_t stack_limit,
                                             void (*entry)());
  static uint32_t debug_reason_take();
  static void debug_step(bool on);
  // 型は rv32<...>::context_t。link_stubs.cpp で明示インスタンス化する。
  template <class Context> static bool faulted_in_thread_mode(const Context &context);
  static uint32_t fault_status();
  static uintptr_t fault_address();
  static void fault_status_clear();
};

struct chip {
  // 単一コア (configs/kit.bzl の rv32_c6 は SHIZUKU_CPU_COUNT=1)
  static constexpr uint32_t kCoreCount = 1;
  // PMP: 物理エントリ 16 本 (pmpcfg0..3 / pmpaddr0..15)、粒度 4B。
  // arch/riscv/pmp.hpp の pmp_codec<kPmpGranuleBytes> と組む (現行動作を変えない)。
  static constexpr uint32_t kPmpEntries = 16;
  static constexpr uint32_t kPmpGranuleBytes = 4;
  // ARCH として注入される型 (configs/kit.bzl の ${SHIZUKU_ARCH_TYPE})。
  using arch_type = shizuku::archs::rv32<kPmpEntries, kPmpGranuleBytes, plic_vector_mode>;
};

} // namespace shizuku::chips::esp32_c6

#endif // SHIZUKU_CHIPS_ESP32_C6_CHIP_HPP
