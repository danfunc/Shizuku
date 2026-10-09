#ifndef SHIZUKU_C6_SYSTIMER_HPP
#define SHIZUKU_C6_SYSTIMER_HPP
#include <cstdint>
// 出典は .local/esp-idf/components/soc/esp32c6/register/soc/ の
// reg_base.h / systimer_reg.h / plic_reg.h / interrupt_matrix_reg.h。
namespace shizuku::ports::c6 {
constexpr uint32_t INTMTX_BASE = 0x60010000;
constexpr uint32_t SYSTIMER_BASE = 0x6000A000;
constexpr uint32_t PLIC_MX_BASE = 0x20001000;

// INTMTX_CORE0_SYSTIMER_TARGET0_INTR_MAP_REG = INTMTX_BASE + 0xe4 (= 4 * source 57)
constexpr uint32_t SYSTIMER_TARGET0_SOURCE = 57;
constexpr uint32_t INTMTX_SYSTIMER_TARGET0_MAP = INTMTX_BASE + 4 * SYSTIMER_TARGET0_SOURCE;
// vectors_intc.S: CPU 割込み 1..23 は空き。mcause 下位 5bit = CPU 割込み番号 = ベクタ slot。
constexpr uint32_t TIMER_CPU_INT = 17;
constexpr uint32_t TIMER_PRIORITY = 3;
constexpr uint32_t PLIC_THRESHOLD = 1; // IDF RVHAL_INTR_ENABLE_THRESH

constexpr uint32_t PLIC_MXINT_ENABLE = PLIC_MX_BASE + 0x0;
constexpr uint32_t PLIC_MXINT_TYPE = PLIC_MX_BASE + 0x4;
constexpr uint32_t PLIC_MXINT_PRI_BASE = PLIC_MX_BASE + 0x10;
constexpr uint32_t PLIC_MXINT_THRESH = PLIC_MX_BASE + 0x90;
constexpr uint32_t plic_pri_reg(uint32_t n) { return PLIC_MXINT_PRI_BASE + 4 * n; }

constexpr uint32_t SYSTIMER_CONF = SYSTIMER_BASE + 0x0;
constexpr uint32_t SYSTIMER_UNIT0_OP = SYSTIMER_BASE + 0x4;
constexpr uint32_t SYSTIMER_TARGET0_HI = SYSTIMER_BASE + 0x1c;
constexpr uint32_t SYSTIMER_TARGET0_LO = SYSTIMER_BASE + 0x20;
constexpr uint32_t SYSTIMER_TARGET0_CONF = SYSTIMER_BASE + 0x34;
constexpr uint32_t SYSTIMER_UNIT0_VALUE_HI = SYSTIMER_BASE + 0x40;
constexpr uint32_t SYSTIMER_UNIT0_VALUE_LO = SYSTIMER_BASE + 0x44;
constexpr uint32_t SYSTIMER_COMP0_LOAD = SYSTIMER_BASE + 0x50;
constexpr uint32_t SYSTIMER_INT_ENA = SYSTIMER_BASE + 0x64;
constexpr uint32_t SYSTIMER_INT_CLR = SYSTIMER_BASE + 0x6c;

constexpr uint32_t CONF_TARGET0_WORK_EN = 1u << 24;
constexpr uint32_t CONF_UNIT0_WORK_EN = 1u << 30;
constexpr uint32_t CONF_CLK_EN = 1u << 31;
constexpr uint32_t UNIT0_OP_VALUE_VALID = 1u << 29;
constexpr uint32_t UNIT0_OP_UPDATE = 1u << 30;
constexpr uint32_t TARGET0_CONF_PERIOD_MODE = 1u << 30;
constexpr uint32_t TARGET0_CONF_UNIT_SEL = 1u << 31;
constexpr uint32_t COMP0_LOAD_BIT = 1u;
constexpr uint32_t TARGET0_INT_BIT = 1u;

// XTAL 由来の 16 MHz。V2 の「cycles」は SYSTIMER tick と定義する。
constexpr uint32_t TICKS_PER_US = 16;

constexpr uint64_t us_from_ticks(uint64_t ticks) { return ticks / TICKS_PER_US; }
constexpr uint64_t alarm_target(uint64_t now, uint32_t cycles) { return now + cycles; }
// 発火済み (now >= target) なら wrapped=true / 0。残りは 32bit に収める。
constexpr uint32_t remaining_ticks(uint64_t target, uint64_t now, bool &wrapped) {
  wrapped = now >= target;
  if (wrapped) return 0;
  const uint64_t left = target - now;
  return left > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(left);
}
}
#endif
