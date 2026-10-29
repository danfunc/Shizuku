#include <cstdint>
#include "boards/esp32_c6/esp32_c6.hpp"
#include "chips/esp32_c6/systimer.hpp"
#include "chips/esp32_c6/trap_api.hpp"
#include "boards/esp32_c6/heartbeat_logic.hpp"
#include "boards/esp32_c6/led_gpio.hpp"

#ifndef SHIZUKU_C6_HEARTBEAT_LED
#define SHIZUKU_C6_HEARTBEAT_LED 1
#endif

extern "C" {
extern char _bss_end;
}
// trap/timer の SoC 側 API (chips/esp32_c6/trap_api.hpp) を、元の名前のまま使う。
using namespace shizuku::chips::esp32_c6;
namespace c6 = shizuku::ports::c6;

namespace {
// LED 点滅に使う SYSTIMER target1 (target0 は kernel の one-shot timer 専用なので触らない)。
// 出典: ESP-IDF v6.1 soc/esp32c6 の systimer_reg.h (TARGET1_HI/LO/CONF, COMP1_LOAD,
// INT_ENA/INT_CLR bit1, CONF bit23 = TARGET1_WORK_EN)。割込み源は ETS_SYSTIMER_TARGET1_INTR_SOURCE = 58
// (INTMTX_CORE0_SYSTIMER_TARGET1_INTR_MAP_REG = base + 0xe8 = 4 * 58)。
constexpr uint32_t LED_TIMER_SOURCE = 58;
// cpu_irq 5: IDF の予約 (1,3,4,6,7) と BLE 割当の除外 (0,1,3,4,6,7,8,17,31) のどちらにも入らない。
// 起動時に先に登録するので、BLE の動的割当 (c6_intr_alloc.cpp) は -2 で飛ばして別の line を使う。
constexpr uint32_t LED_TIMER_CPU_IRQ = 5;
constexpr uint32_t LED_TIMER_PRIORITY = 3; // PLIC threshold (=1) 以下だと通らないので 2 以上 (kernel timer と同じ 3)
constexpr uint32_t SYSTIMER_CONF_REG = c6::SYSTIMER_BASE + 0x0;
constexpr uint32_t SYSTIMER_TARGET1_HI = c6::SYSTIMER_BASE + 0x24;
constexpr uint32_t SYSTIMER_TARGET1_LO = c6::SYSTIMER_BASE + 0x28;
constexpr uint32_t SYSTIMER_TARGET1_CONF = c6::SYSTIMER_BASE + 0x38;
constexpr uint32_t SYSTIMER_COMP1_LOAD = c6::SYSTIMER_BASE + 0x54;
constexpr uint32_t SYSTIMER_INT_ENA_REG = c6::SYSTIMER_BASE + 0x64;
constexpr uint32_t SYSTIMER_INT_CLR_REG = c6::SYSTIMER_BASE + 0x6c;
constexpr uint32_t CONF_TARGET1_WORK_EN = 1u << 23;
constexpr uint32_t TARGET1_INT_BIT = 1u << 1;

inline void mmio_write(uint32_t address, uint32_t value) {
  *reinterpret_cast<volatile uint32_t *>(address) = value;
}
inline uint32_t mmio_read(uint32_t address) {
  return *reinterpret_cast<volatile uint32_t *>(address);
}

shizuku::boards::esp32_c6_heartbeat::state_t heartbeat_state{};
// 診断: [0] = 起動時の irq_register の戻り値 (0 が成功, 負は失敗で LED は点かない), [1] = 割込み回数。
volatile int32_t heartbeat_diag[2] = {};

void set_led(bool on) { shizuku::boards::esp32_c6_led::gpio_set(shizuku::boards::esp32_c6_led::direct_mmio{}, on); }

void heartbeat_led_gpio_init() { shizuku::boards::esp32_c6_led::gpio_init(shizuku::boards::esp32_c6_led::direct_mmio{}); }

// target1 を絶対 tick で装填する。COMP1_LOAD で比較器に取り込む (target0 と同じ手順)。
void heartbeat_arm(uint64_t target) {
  mmio_write(SYSTIMER_TARGET1_HI, static_cast<uint32_t>(target >> 32));
  mmio_write(SYSTIMER_TARGET1_LO, static_cast<uint32_t>(target));
  mmio_write(SYSTIMER_COMP1_LOAD, 1u);
}

// SYSTIMER target1 の割込みハンドラ。0.25 s ごとに呼ばれる。待たない・長居しない。
void heartbeat_isr(void *) {
  mmio_write(SYSTIMER_INT_CLR_REG, TARGET1_INT_BIT); // level 割込みを落としてから装填し直す
  heartbeat_diag[1] = heartbeat_diag[1] + 1;
  const uint64_t now = systimer_now();
  if (shizuku::boards::esp32_c6_heartbeat::step(heartbeat_state, now, true))
    set_led(heartbeat_state.led_on);
  heartbeat_arm(now + shizuku::boards::esp32_c6_heartbeat::PERIOD_TICKS);
}

// 割込みを先に登録し、成功したときだけ target1 を動かす。失敗時は何も動かさず LED 無しで続行する
// (点滅は補助機能なので、起動は止めない)。
void heartbeat_start() {
  heartbeat_led_gpio_init();
  const int rc = irq_register(LED_TIMER_SOURCE, LED_TIMER_CPU_IRQ, heartbeat_isr, nullptr, 0,
                              true, LED_TIMER_PRIORITY, false);
  heartbeat_diag[0] = rc;
  if (rc != 0) return;
  const uint64_t now = systimer_now();
  heartbeat_state = {now, false};
  mmio_write(SYSTIMER_CONF_REG, mmio_read(SYSTIMER_CONF_REG) | CONF_TARGET1_WORK_EN);
  mmio_write(SYSTIMER_TARGET1_CONF, 0); // UNIT0, 周期モードなし (ISR で毎回装填する)
  mmio_write(SYSTIMER_INT_CLR_REG, TARGET1_INT_BIT);
  mmio_write(SYSTIMER_INT_ENA_REG, mmio_read(SYSTIMER_INT_ENA_REG) | TARGET1_INT_BIT);
  heartbeat_arm(now + shizuku::boards::esp32_c6_heartbeat::PERIOD_TICKS);
}
} // namespace

namespace shizuku::boards {
void esp32_c6::init(uint32_t core) {
  if (core != 0) stop(0xE007);
  install_traps();
  interrupt_init();
#if SHIZUKU_C6_HEARTBEAT_LED
  heartbeat_start();
#endif
}
uint64_t esp32_c6::time_us() { return c6::us_from_ticks(systimer_now()); }
uint32_t esp32_c6::cycles_per_us() { return c6::TICKS_PER_US; }
uintptr_t esp32_c6::unprivileged_floor() {
  // Stage-2 privileged-only pilot: BSS bookkeeping is below this boundary.
  // This is not a PMP security boundary. U-mode return is refused above.
  return reinterpret_cast<uintptr_t>(&_bss_end);
}
[[noreturn]] void esp32_c6::panic(const char *) { stop(0xE008); }
} // namespace shizuku::boards
