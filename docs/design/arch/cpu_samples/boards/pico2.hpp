// サンプル: board = 基板の配線と、起動コード・SDK との継ぎ目だけ。
// 例外の結線・優先度の規約・MPU の基本配置は持たない (cpu_manager が持つ)。
#ifndef SHIZUKU_SAMPLE_BOARD_PICO2_HPP
#define SHIZUKU_SAMPLE_BOARD_PICO2_HPP
#include <stdint.h>
#include "../chips/rp2350.hpp"

extern "C" char __flash_binary_end; // リンカが置く。ファーム本体の終端
extern "C" char __end__;            // 静的領域の終端 = ヒープ先頭

namespace sample::boards {

struct pico2 {
  using chip = sample::chips::rp2350;

  // ファームのコードの範囲 (region0 = 読み+実行に張る範囲)。
  static uintptr_t code_begin() { return chip::XIP_BASE; }
  static uintptr_t code_end() { return reinterpret_cast<uintptr_t>(&__flash_binary_end); }
  // 非特権から届く最下位アドレス。これより下はカーネルの静的領域。
  static uintptr_t unprivileged_floor() { return (reinterpret_cast<uintptr_t>(&__end__) + 31) & ~31u; }

  // この基板で使う SoC 割込みの優先度。カーネルの 3 段 (SVC/タイマ/切替え) には触らない。
  template <class ARCH> static void soc_irq_init(uint32_t core) {
    ARCH::set_irq_priority(chip::IRQ_USBCTRL, 0x20); // 落ちた報告を USB で出し切るため FAULT より上
    ARCH::set_irq_priority(chip::IRQ_SIO_FIFO_CORE0 + core, 0x00);
  }

  static uint32_t clock_hz();                    // 実クロック (SDK の clock_get_hz を呼ぶ .cpp)
  static void launch_core(void (*entry)());       // もう一方のコアを起こす (SDK の multicore)
  static void diag_write(const char *text, uint32_t length);
  static volatile uint32_t &persistent_word(uint32_t index) {
    return reinterpret_cast<volatile uint32_t *>(0x400D800C)[index]; // watchdog SCRATCH0..
  }
};

} // namespace sample::boards
#endif
