#ifndef SHIZUKU_CHIPS_ESP32_C6_TRAP_API_HPP
#define SHIZUKU_CHIPS_ESP32_C6_TRAP_API_HPP
#include <cstdint>

// trap.cpp の SoC 側の入口。board 実装 (boards/esp32_c6) はここだけを見る (boards -> chips)。
namespace shizuku::chips::esp32_c6 {
void install_traps();                    // mtvec / mscratch を設定 (M-only vectored)
void interrupt_init();                   // SYSTIMER -> PLIC -> mie の初期化
[[noreturn]] void stop(uint32_t status); // 診断ステータスを残して停止
uint64_t systimer_now();                 // SYSTIMER の現在値 (tick)
} // namespace shizuku::chips::esp32_c6

#endif // SHIZUKU_CHIPS_ESP32_C6_TRAP_API_HPP
