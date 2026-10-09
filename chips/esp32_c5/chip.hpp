#ifndef SHIZUKU_CHIPS_ESP32_C5_CHIP_HPP
#define SHIZUKU_CHIPS_ESP32_C5_CHIP_HPP
// ESP32-C5 (SoC) の型の入口。config.hpp の CHIP 別名がこれを指す (configs/kit.bzl)。
// ★ESP-IDF / pico-sdk を一切 include しない (chips の依存閉包はそれを含まない)。
// ★fail-closed gate (SHIZUKU_C5_SKELETON_PERMITTED) はここに置かない。rv32_c5.hpp の互換入口に残す。
#include <cstdint>
#include "arch/riscv/rv32.hpp"

namespace shizuku::chips::esp32_c5 {

// C5 の割込み方式 (CLIC、mtvec mode=3 の直接エントリ + mtvt)。
// C6 の plic_vector_mode と同じ形の IntMode。定義は trap.cpp (本体) と
// modules/esp_idf_support/c5/link_stubs.cpp (未実装部)。
// 割込みはすべて non-vectored (clicintattr.shv=0) にして mtvec の 1 エントリへ集める。
struct clic_direct_mode {
  static constexpr uint32_t CALL_HEADROOM = 256;
  // U-mode からの rdcycle (mcounteren.CY) は未検証のため false (C6 と同じ)。
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
  // 単一コア (configs/kit.bzl の rv32_c5 は SHIZUKU_CPU_COUNT=1)
  static constexpr uint32_t kCoreCount = 1;
  // PMP: 粒度は ESP-IDF の SOC_CPU_PMP_REGION_GRANULARITY=128 (確定)。
  // ★物理エントリ数は未確認 (IDF にマクロなし)。16 は RISC-V の標準値からの推定で、
  //   実機で pmpcfg/pmpaddr の書戻しを確かめるまで確定しない。M-only pilot は PMP を設定しないので
  //   現状の動作には効かない。
  static constexpr uint32_t kPmpEntries = 16;
  static constexpr uint32_t kPmpGranuleBytes = 128;
  // ARCH として注入される型 (configs/kit.bzl の ${SHIZUKU_ARCH_TYPE})。
  using arch_type = shizuku::archs::rv32<kPmpEntries, kPmpGranuleBytes, clic_direct_mode>;
};

} // namespace shizuku::chips::esp32_c5

#endif // SHIZUKU_CHIPS_ESP32_C5_CHIP_HPP
