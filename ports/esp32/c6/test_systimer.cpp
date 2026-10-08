#include <cassert>
#include <cstdint>
#include "ports/esp32/c6/systimer.hpp"

using namespace shizuku::ports::c6;

int main() {
  // 1. レジスタアドレスとオフセットの検証
  static_assert(INTMTX_BASE == 0x60010000);
  static_assert(SYSTIMER_BASE == 0x6000A000);
  static_assert(PLIC_MX_BASE == 0x20001000);
  static_assert(SYSTIMER_TARGET0_SOURCE == 57);
  static_assert(INTMTX_SYSTIMER_TARGET0_MAP == 0x60010000 + 0xe4);
  static_assert(TIMER_CPU_INT == 17);
  static_assert(TIMER_PRIORITY == 3);
  static_assert(PLIC_THRESHOLD == 1);

  // PLIC レジスタ
  static_assert(PLIC_MXINT_ENABLE == 0x20001000 + 0x0);
  static_assert(PLIC_MXINT_TYPE == 0x20001000 + 0x4);
  static_assert(PLIC_MXINT_PRI_BASE == 0x20001000 + 0x10);
  static_assert(PLIC_MXINT_THRESH == 0x20001000 + 0x90);
  static_assert(plic_pri_reg(17) == 0x20001000 + 0x10 + 4 * 17);

  // SYSTIMER レジスタ
  static_assert(SYSTIMER_CONF == 0x6000A000 + 0x0);
  static_assert(SYSTIMER_UNIT0_OP == 0x6000A000 + 0x4);
  static_assert(SYSTIMER_TARGET0_HI == 0x6000A000 + 0x1c);
  static_assert(SYSTIMER_TARGET0_LO == 0x6000A000 + 0x20);
  static_assert(SYSTIMER_TARGET0_CONF == 0x6000A000 + 0x34);
  static_assert(SYSTIMER_UNIT0_VALUE_HI == 0x6000A000 + 0x40);
  static_assert(SYSTIMER_UNIT0_VALUE_LO == 0x6000A000 + 0x44);
  static_assert(SYSTIMER_COMP0_LOAD == 0x6000A000 + 0x50);
  static_assert(SYSTIMER_INT_ENA == 0x6000A000 + 0x64);
  static_assert(SYSTIMER_INT_CLR == 0x6000A000 + 0x6c);

  // ビット定義
  static_assert(CONF_TARGET0_WORK_EN == (1u << 24));
  static_assert(CONF_UNIT0_WORK_EN == (1u << 30));
  static_assert(CONF_CLK_EN == (1u << 31));
  static_assert(UNIT0_OP_VALUE_VALID == (1u << 29));
  static_assert(UNIT0_OP_UPDATE == (1u << 30));
  static_assert(COMP0_LOAD_BIT == 1u);
  static_assert(TARGET0_INT_BIT == 1u);
  static_assert(TICKS_PER_US == 16);

  // 2. 純粋関数の単体テスト
  // us_from_ticks
  assert(us_from_ticks(0) == 0);
  assert(us_from_ticks(15) == 0);
  assert(us_from_ticks(16) == 1);
  assert(us_from_ticks(32) == 2);
  assert(us_from_ticks(160000) == 10000);  // 10ms

  // alarm_target
  assert(alarm_target(100, 50) == 150);
  assert(alarm_target(0xFFFFFFFFFFFFFF00ULL, 0x200) == 0x100);  // 64-bit wrap

  // remaining_ticks
  bool wrapped = false;
  // 未発火 (now < target)
  uint32_t rem = remaining_ticks(1000, 800, wrapped);
  assert(!wrapped);
  assert(rem == 200);

  // ちょうど発火 (now == target)
  rem = remaining_ticks(1000, 1000, wrapped);
  assert(wrapped);
  assert(rem == 0);

  // 発火済み (now > target)
  rem = remaining_ticks(1000, 1050, wrapped);
  assert(wrapped);
  assert(rem == 0);

  // 32-bit 超過クランプ
  rem = remaining_ticks(0x200000000ULL, 0, wrapped);
  assert(!wrapped);
  assert(rem == 0xFFFFFFFFu);

  return 0;
}
