#ifndef SHIZUKU_CHIPS_ESP32_C5_TRAP_API_HPP
#define SHIZUKU_CHIPS_ESP32_C5_TRAP_API_HPP
#include <cstdint>
#include <cstddef>

// trap.cpp の SoC 側の入口。board 実装 (boards/esp32_c5) はここだけを見る (boards -> chips)。
namespace shizuku::chips::esp32_c5 {
void install_traps();                    // mtvec (CLIC mode) / mtvt / mscratch を設定
void interrupt_init();                   // SYSTIMER -> interrupt matrix -> CLIC の初期化
[[noreturn]] void stop(uint32_t status); // 診断ステータスを残して停止
uint64_t systimer_now();                 // SYSTIMER の現在値 (tick)

// C5-only peripheral IRQ registration. cpu_irq is the RISC-V IRQ number
// routed by the interrupt matrix; trap cause is cpu_irq + CLIC_EXT_OFFSET.
using irq_handler_t = void (*)(void *);
struct irq_dispatch_snapshot {
  uint32_t source;
  uint32_t dispatch_count;
};
size_t irq_snapshot(irq_dispatch_snapshot *out, size_t capacity);
int irq_register(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg,
                 uint32_t flags, bool enabled = true, uint32_t level = 3, bool edge = false);
int irq_set_enabled(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg,
                    bool enabled);
int irq_unregister(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg);
} // namespace shizuku::chips::esp32_c5

#endif // SHIZUKU_CHIPS_ESP32_C5_TRAP_API_HPP
