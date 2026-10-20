#ifndef SHIZUKU_ARCH_RISCV_TRAP_ROUTE_HPP
#define SHIZUKU_ARCH_RISCV_TRAP_ROUTE_HPP
#include <cstdint>

#ifndef SHIZUKU_STAGE05_CPP_ATTR
#if defined(SHIZUKU_C6_STAGE05_IRAM_CPP) && SHIZUKU_C6_STAGE05_IRAM_CPP
#define SHIZUKU_STAGE05_CPP_ATTR __attribute__((section(".iram1.text")))
#else
#define SHIZUKU_STAGE05_CPP_ATTR
#endif
#endif

namespace shizuku::ports::riscv {
enum class trap_kind { ecall, interrupt, fault };
SHIZUKU_STAGE05_CPP_ATTR
constexpr trap_kind classify_trap(uint32_t cause) {
  if (cause & 0x80000000u) return trap_kind::interrupt;
  return cause == 8 || cause == 11 ? trap_kind::ecall : trap_kind::fault;
}
// Shared production/test routing. Only hardware IRQs contribute to irq_depth.
// ECALL is always 32 bits, even when the C extension is enabled.
template<class Frame, class Ecall, class Interrupt, class Fault>
SHIZUKU_STAGE05_CPP_ATTR
void route_trap(Frame &frame, volatile uint32_t &irq_depth,
                Ecall ecall, Interrupt interrupt, Fault fault) {
  switch (classify_trap(frame.mcause)) {
  case trap_kind::ecall:
    frame.mepc += 4;
    ecall();
    break;
  case trap_kind::interrupt:
    irq_depth = irq_depth + 1;
    interrupt();
    irq_depth = irq_depth - 1;
    break;
  case trap_kind::fault:
    fault();
    break;
  }
}
}
#endif
