#ifndef SHIZUKU_KERNEL_SERVICES_HPP
#define SHIZUKU_KERNEL_SERVICES_HPP
#include <cstdint>
namespace shizuku::kernel_services {
// Privileged internal interface, not a public object/atomic ABI.
enum class status { ok, not_privileged, bad_context, operation_failed };
struct result { status code; uintptr_t value; uintptr_t detail; };
struct irq_state { uint32_t primask; uint32_t started; };
result yield();
result grant(uint32_t target, uint32_t cycles);
result sleep_us(uintptr_t microseconds);
uint64_t time_us();
bool cas(volatile uint32_t *address, uint32_t expected, uint32_t desired);
bool in_isr();
irq_state irq_save();
void irq_restore(irq_state state);
} // namespace shizuku::kernel_services
// C6 port supplies the context hook in its own worktree; no guessed implementation.
extern "C" bool shizuku_c6_in_isr();
#endif
