#ifndef SHIZUKU_C5_SYSTIMER_HPP
#define SHIZUKU_C5_SYSTIMER_HPP
#include <cstdint>
// 出典は .local/esp-idf/components/soc/esp32c5/ の register/soc/ (reg_base.h / systimer_reg.h /
// interrupt_matrix_reg.h / clic_reg.h) と include/soc/interrupts.h。SYSTIMER のレジスタ配置は C6 と同一。
namespace shizuku::ports::c5 {
constexpr uint32_t INTMTX_BASE = 0x60010000;
constexpr uint32_t SYSTIMER_BASE = 0x6000A000;
constexpr uint32_t CLIC_BASE = 0x20800000;
constexpr uint32_t CLIC_CTRL_BASE = 0x20801000;

// INTMTX_CORE0_SYSTIMER_TARGET0_INTR_MAP_REG = INTMTX_BASE + 0xf4 (= 4 * source 61)
constexpr uint32_t SYSTIMER_TARGET0_SOURCE = 61;
constexpr uint32_t INTMTX_SYSTIMER_TARGET0_MAP = INTMTX_BASE + 4 * SYSTIMER_TARGET0_SOURCE;
// CLIC の割込み番号 0..15 は内部、16..47 が外部 (CLIC_EXT_INTR_NUM_OFFSET=16)。
// 外部線 17 を使う (C6 の TIMER_CPU_INT=17 と同じ番号)。matrix へ書く値と clicint の添字は +16 した値。
constexpr uint32_t CLIC_EXT_OFFSET = 16;
constexpr uint32_t clic_index_for_external_irq(uint32_t cpu_irq) {
  return cpu_irq + CLIC_EXT_OFFSET;
}
static_assert(clic_index_for_external_irq(18) == 34 &&
              clic_index_for_external_irq(31) == 47);
constexpr uint32_t TIMER_CPU_INT = 17;
constexpr uint32_t TIMER_CLIC_INDEX = CLIC_EXT_OFFSET + TIMER_CPU_INT;  // = 33 = mcause の exccode
constexpr uint32_t MCAUSE_EXCCODE_MASK = 0xFFFu;  // CLIC では mcause に mpp/mpie/mpil が載る
constexpr uint32_t MCAUSE_INTERRUPT = 0x80000000u;
// NLBITS=3: clicintctl の上位 3 bit が優先度、下位 5 bit は 1 埋め。
constexpr uint32_t NLBITS_SHIFT = 5;
constexpr uint32_t clic_level_byte(uint32_t level) { return (level << NLBITS_SHIFT) | ((1u << NLBITS_SHIFT) - 1u); }
constexpr uint32_t TIMER_PRIORITY = 3;
constexpr uint32_t CLIC_THRESHOLD_LEVEL = 0;  // IDF RVHAL_INTR_ENABLE_THRESH (mintthresh = clic_level_byte(0))
constexpr uint32_t CSR_MTVT = 0x307;
constexpr uint32_t MTVEC_MODE_CLIC = 3;

// clicint[i] (32bit): [7:0]=ip [15:8]=ie [23:16]=attr (mode[7:6] trig[2:1] shv[0]) [31:24]=ctl
constexpr uint32_t clic_ctrl_reg(uint32_t index) { return CLIC_CTRL_BASE + 4 * index; }
constexpr uint32_t CLICINT_IE = 1u << 8;
constexpr uint32_t CLICINT_ATTR_MODE_M = 3u << 22;  // machine mode; trig=0 (level), shv=0 (non-vectored)
constexpr uint32_t CLICINT_ATTR_EDGE = 1u << 21;  // trig=01 (rising edge), shv=0
constexpr uint32_t clicint_value(uint32_t level, bool enable, bool edge = false) {
  return (clic_level_byte(level) << 24) | CLICINT_ATTR_MODE_M |
         (edge ? CLICINT_ATTR_EDGE : 0u) | (enable ? CLICINT_IE : 0u);
}

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

// 確定 (IDF components/esp_hw_support/port/esp32c5/systimer.c): XTAL が 48MHz でも 40MHz でも
// systimer の分解能は常に 16MHz (固定小数分周が EFUSE_XTAL_48M_SEL に応じて変わる)。C6 と同じ。
// V2 の「cycles」は SYSTIMER tick と定義する。
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
