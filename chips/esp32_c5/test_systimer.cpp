#include <cassert>
#include <cstdint>
#include "chips/esp32_c5/systimer.hpp"

using namespace shizuku::ports::c5;

int main() {
  // interrupt matrix / CLIC (C6 の PLIC とは別物)
  static_assert(INTMTX_BASE == 0x60010000);
  static_assert(SYSTIMER_BASE == 0x6000A000);
  static_assert(CLIC_BASE == 0x20800000);
  static_assert(CLIC_CTRL_BASE == 0x20801000);
  static_assert(SYSTIMER_TARGET0_SOURCE == 61);
  static_assert(INTMTX_SYSTIMER_TARGET0_MAP == 0x60010000 + 0xf4);
  static_assert(TIMER_CPU_INT == 17);
  static_assert(TIMER_CLIC_INDEX == 33);  // 外部線 17 + 16
  static_assert(clic_ctrl_reg(TIMER_CLIC_INDEX) == 0x20801000 + 4 * 33);
  // NLBITS=3: level 0 -> 0x1f, level 3 -> 0x7f
  static_assert(clic_level_byte(0) == 0x1f);
  static_assert(clic_level_byte(3) == 0x7f);
  // ctl[31:24]=0x7f, attr mode=3 (bit 23:22), trig=0, shv=0, ie (bit 8)
  static_assert(clicint_value(3, true) == ((0x7fu << 24) | (3u << 22) | (1u << 8)));
  static_assert(clicint_value(3, false) == ((0x7fu << 24) | (3u << 22)));
  static_assert(((clicint_value(3, true) >> 16) & 1u) == 0, "shv must stay 0 (non-vectored)");
  static_assert(CSR_MTVT == 0x307 && MTVEC_MODE_CLIC == 3);
  // CLIC の mcause は上位に mpp/mpie/mpil が載る: exccode だけを取り出せること
  static_assert((0xB8000000u | 33u) == (MCAUSE_INTERRUPT | (3u << 28) | (1u << 27) | 33u));
  static_assert(((0xB8000021u) & MCAUSE_EXCCODE_MASK) == TIMER_CLIC_INDEX);

  // SYSTIMER のレジスタ配置は C6 と同一
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

  // tick レートは 16 MHz (IDF esp32c5/systimer.c)
  static_assert(TICKS_PER_US == 16);
  static_assert(us_from_ticks(16) == 1);
  static_assert(us_from_ticks(16000000) == 1000000);

  bool wrapped = true;
  assert(remaining_ticks(100, 40, wrapped) == 60 && !wrapped);
  assert(remaining_ticks(100, 100, wrapped) == 0 && wrapped);
  assert(remaining_ticks(0x200000000ull, 0, wrapped) == 0xFFFFFFFFu && !wrapped);
  assert(alarm_target(10, 5) == 15);
  return 0;
}
