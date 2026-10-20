#ifndef SHIZUKU_CHIPS_ESP32_C6_TRAP_API_HPP
#define SHIZUKU_CHIPS_ESP32_C6_TRAP_API_HPP
#include <cstdint>
#include <cstddef>

// trap.cpp の SoC 側の入口。board 実装 (boards/esp32_c6) はここだけを見る (boards -> chips)。
namespace shizuku::chips::esp32_c6 {
void install_traps();                    // mtvec / mscratch を設定 (M-only vectored)
void interrupt_init();                   // SYSTIMER -> PLIC -> mie の初期化
[[noreturn]] void stop(uint32_t status); // 診断ステータスを残して停止
uint64_t systimer_now();                 // SYSTIMER の現在値 (tick)
using irq_handler_t = void (*)(void *);
struct irq_dispatch_snapshot { uint32_t source; uint32_t dispatch_count; };
size_t irq_snapshot(irq_dispatch_snapshot *out, size_t capacity);
int irq_register(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg,
                 uint32_t flags, bool enabled = true, uint32_t level = 3, bool edge = false);
int irq_set_enabled(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg,
                    bool enabled);
int irq_unregister(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg);

struct irq_dispatch_hooks {
  void (*on_enter)(uint32_t source, uint32_t mepc);
  void (*on_exit)(uint32_t source);
};

void set_irq_dispatch_hooks(irq_dispatch_hooks hooks);

template <class Policy>
constexpr irq_dispatch_hooks make_irq_dispatch_hooks() {
  irq_dispatch_hooks hooks{nullptr, nullptr};
  if constexpr (requires(uint32_t s, uint32_t pc) { Policy::on_enter(s, pc); }) {
    hooks.on_enter = &Policy::on_enter;
  }
  if constexpr (requires(uint32_t s) { Policy::on_exit(s); }) {
    hooks.on_exit = &Policy::on_exit;
  }
  return hooks;
}
} // namespace shizuku::chips::esp32_c6

#endif // SHIZUKU_CHIPS_ESP32_C6_TRAP_API_HPP
