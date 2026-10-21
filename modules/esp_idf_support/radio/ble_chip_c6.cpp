#include "ble_chip.hpp"
#include "trap_api.hpp"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_chip_info.h"
#include "esp_private/phy.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "modem/modem_lpcon_reg.h"
#include "modem/modem_syscon_reg.h"
#include "soc/pmu_reg.h"
#include "systimer.hpp"
#include "soc/interrupt_matrix_reg.h"

#ifndef SHIZUKU_C6_ANTENNA_EXTERNAL
#define SHIZUKU_C6_ANTENNA_EXTERNAL 1
#endif

namespace shizuku::boards::esp32_c6 { uint64_t time_us(); }
extern "C" volatile uint32_t shizuku_c6_timer_diag[7];
extern "C" uint64_t timestamp_id[2];
extern "C" uint8_t r_ble_ll_adv_enabled(void);
extern "C" {
// OpenOCD words: magic "C6RF", phase (1..5 GPIO setup, 6 post-controller,
// 7 post advertising start),
// gpio_config rc, GPIO14 selection-write rc, GPIO3-low rc,
// MODEM_SYSCON CLK_CONF/CLK_CONF1,
// MODEM_LPCON CLK_CONF/LP_TIMER_CONF/COEX_LP_CLK_CONF, PHY modem flags,
// MODEM_SYSCON/LPCON CLK_CONF_POWER_ST mapping words, then PMU_PWR_STATE and
// PMU_CLK_STATE0 live state readbacks; [15..24] SYSTIMER CONF/UNIT0 update and
// count/target2 compare/config/interrupt state; [25..28] esp_timer's two next
// alarm timestamps in us; [29] LL advertising-enabled state; [30..31] PMU
// function/APB clock-gate state; [32..33] esp_timer current time in us;
// [34] PMU main FSM state; [35..36] HP_ACTIVE and HP_MODEM power configs.
#if defined(__riscv)
__attribute__((used, aligned(4), section(".noinit")))
#else
__attribute__((used, aligned(4)))
#endif
volatile uint32_t shizuku_c6_ble_radio_diag[37];
// Versioned phase snapshots: magic, version, sequence (odd while writing),
// phase, sample_count, valid, chip revision, controller HAL words +0x28/+0x44,
// modem clock registers, PMU power/clock state, MODEM_SYSCON_CLK_CONF1, PHY flags,
// raw INTMTX CORE0 source-status word 0, current PLIC pending, and CPU mie.
#if defined(__riscv)
__attribute__((used, aligned(4), section(".noinit")))
#else
__attribute__((used, aligned(4)))
#endif
volatile uint32_t shizuku_c6_ble_timer_diag[22];
// Separate execution-boundary counters so the existing CT6D and EBLE layouts
// remain stable for their current readers. [0..1] magic/version, [2..3]
// source-4 handler entry/return, [4..5] source-7 entry/return, [6] compat
// scheduler entries; [7..38] eight records of {task ID, RUN_FOR attempts,
// RUN_FOR completions, last RUN_FOR result (0/1)}.
__attribute__((used, aligned(4), section(".noinit")))
volatile uint32_t shizuku_c6_ble_exec_diag[39];
// Raw syscall evidence alongside C6EX: [0..2] magic/version/orphan RUN_FOR
// count; eight records [object ID, kernel thread ID returned by SPAWN,
// RUN_FOR a0 error, RUN_FOR a1 grant_end/value, RUN_FOR return count].
__attribute__((used, aligned(4), section(".noinit")))
volatile uint32_t shizuku_c6_ble_grant_diag[43];
// Minimal timestamp timeline for Form (2) stop diagnosis (safe store only):
// [0..1] magic "C6LT"/version, [2..3] src4 entry/return us, [4..5] src7 entry/return us,
// [6..7] ble_ll attempt/complete us, [8..9] nimble_host attempt/complete us,
// [10..11] queue_send us/handle, [12..13] queue_recv us/handle, [14..15] reserved.
__attribute__((used, aligned(4), section(".noinit")))
volatile uint32_t shizuku_c6_ble_timeline_diag[16];
}

namespace {
bool grant_diag_initialized = false;
void ensure_grant_diag_initialized() {
  if (grant_diag_initialized) return;
  shizuku_c6_ble_grant_diag[0] = 0u;
  shizuku_c6_ble_grant_diag[1] = 1u;
  shizuku_c6_ble_grant_diag[2] = 0u;
  for (size_t slot = 0; slot < 8; ++slot) {
    const size_t base = 3u + slot * 5u;
    for (size_t word = 0; word < 4u; ++word)
      shizuku_c6_ble_grant_diag[base + word] = UINT32_MAX;
    shizuku_c6_ble_grant_diag[base + 4u] = 0u;
  }
  shizuku_c6_ble_grant_diag[0] = 0x52473643u; // "C6GR"
  grant_diag_initialized = true;
}
}

namespace shizuku::esp_idf_support::ble_chip {
namespace {
uint64_t read_timestamp_id(size_t index) {
  auto *words = reinterpret_cast<volatile uint32_t *>(&timestamp_id[index]);
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    const uint32_t hi_before = words[1];
    const uint32_t lo = words[0];
    const uint32_t hi_after = words[1];
    if (hi_before == hi_after)
      return (static_cast<uint64_t>(hi_before) << 32) | lo;
  }
  return UINT64_MAX;
}
}

uint64_t time_us() { return shizuku::boards::esp32_c6::time_us(); }
size_t irq_snapshot(irq_dispatch_snapshot *out, size_t capacity) {
  shizuku::chips::esp32_c6::irq_dispatch_snapshot rows[32]{};
  const size_t total = shizuku::chips::esp32_c6::irq_snapshot(rows, capacity < 32 ? capacity : 32);
  const size_t count = total < capacity ? total : capacity;
  for (size_t i = 0; out && i < count; ++i)
    out[i] = {rows[i].source, rows[i].dispatch_count};
  return total;
}
void install_traps() { shizuku::chips::esp32_c6::install_traps(); }
int prepare_radio() {
  auto *diag = shizuku_c6_ble_radio_diag;
  diag[0] = 0x46523643u; // "C6RF"
  diag[1] = 1u;
  shizuku_c6_ble_timer_diag[0] = 0x44365443u; // "CT6D"
  shizuku_c6_ble_timer_diag[1] = 3u;
  shizuku_c6_ble_timer_diag[2] = 0u;
  shizuku_c6_ble_timer_diag[3] = 0u;
  shizuku_c6_ble_timer_diag[4] = 0u;
  shizuku_c6_ble_timer_diag[5] = 0u;
  shizuku_c6_ble_exec_diag[0] = 0x58453643u; // "C6EX"
  shizuku_c6_ble_exec_diag[1] = 1u;
  for (size_t i = 2; i < 7; ++i) shizuku_c6_ble_exec_diag[i] = 0u;
  for (size_t slot = 0; slot < 8; ++slot) {
    const size_t base = 7u + slot * 4u;
    shizuku_c6_ble_exec_diag[base] = UINT32_MAX;
    shizuku_c6_ble_exec_diag[base + 1u] = 0u;
    shizuku_c6_ble_exec_diag[base + 2u] = 0u;
    shizuku_c6_ble_exec_diag[base + 3u] = UINT32_MAX;
  }
  ensure_grant_diag_initialized();
  shizuku_c6_ble_timeline_diag[0] = 0x544C3643u; // "C6LT"
  shizuku_c6_ble_timeline_diag[1] = 1u;
  for (size_t i = 2; i < 16; ++i) shizuku_c6_ble_timeline_diag[i] = 0u;
  for (size_t i = 15; i < 37; ++i) diag[i] = UINT32_MAX;
  gpio_config_t config{};
  config.pin_bit_mask = (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_14);
  config.mode = GPIO_MODE_OUTPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t config_rc = gpio_config(&config);
  diag[2] = static_cast<uint32_t>(config_rc);
  if (config_rc != ESP_OK) return config_rc;
  diag[1] = 2u;
  const int antenna_level = SHIZUKU_C6_ANTENNA_EXTERNAL ? 1 : 0;
  const esp_err_t antenna_rc = gpio_set_level(GPIO_NUM_14, antenna_level);
  diag[3] = static_cast<uint32_t>(antenna_rc);
  if (antenna_rc != ESP_OK) return antenna_rc;
  diag[1] = 3u;
  const esp_err_t switch_rc = gpio_set_level(GPIO_NUM_3, 0);
  diag[4] = static_cast<uint32_t>(switch_rc);
  if (switch_rc != ESP_OK) return switch_rc;
  diag[1] = 4u;
  esp_rom_delay_us(100000);
  diag[1] = 5u;
  return ESP_OK;
}
void capture_radio_state(uint32_t phase) {
  auto *diag = shizuku_c6_ble_radio_diag;
  auto *timer_diag = shizuku_c6_ble_timer_diag;
  uint32_t sequence = timer_diag[2];
  if (sequence & 1u) ++sequence;
  sequence += 1u;
  timer_diag[2] = sequence; // odd: this capture is in progress
  timer_diag[5] = 0u;
  diag[5] = *reinterpret_cast<volatile uint32_t *>(MODEM_SYSCON_CLK_CONF_REG);
  diag[6] = *reinterpret_cast<volatile uint32_t *>(MODEM_SYSCON_CLK_CONF1_REG);
  diag[7] = *reinterpret_cast<volatile uint32_t *>(MODEM_LPCON_CLK_CONF_REG);
  diag[8] = *reinterpret_cast<volatile uint32_t *>(MODEM_LPCON_LP_TIMER_CONF_REG);
  diag[9] = *reinterpret_cast<volatile uint32_t *>(MODEM_LPCON_COEX_LP_CLK_CONF_REG);
  diag[10] = static_cast<uint32_t>(phy_get_modem_flag());
  diag[11] = *reinterpret_cast<volatile uint32_t *>(MODEM_SYSCON_CLK_CONF_POWER_ST_REG);
  diag[12] = *reinterpret_cast<volatile uint32_t *>(MODEM_LPCON_CLK_CONF_POWER_ST_REG);
  diag[13] = *reinterpret_cast<volatile uint32_t *>(PMU_PWR_STATE_REG);
  diag[14] = *reinterpret_cast<volatile uint32_t *>(PMU_CLK_STATE0_REG);
  auto read_reg = [](uint32_t addr) {
    return *reinterpret_cast<volatile uint32_t *>(addr);
  };
  *reinterpret_cast<volatile uint32_t *>(shizuku::ports::c6::SYSTIMER_UNIT0_OP) =
      shizuku::ports::c6::UNIT0_OP_UPDATE;
  uint32_t unit0_op = 0;
  for (uint32_t attempt = 0; attempt < 1024; ++attempt) {
    unit0_op = read_reg(shizuku::ports::c6::SYSTIMER_UNIT0_OP);
    if (unit0_op & shizuku::ports::c6::UNIT0_OP_VALUE_VALID) break;
  }
  diag[15] = read_reg(shizuku::ports::c6::SYSTIMER_CONF);
  diag[16] = unit0_op;
  if (unit0_op & shizuku::ports::c6::UNIT0_OP_VALUE_VALID) {
    diag[17] = read_reg(shizuku::ports::c6::SYSTIMER_UNIT0_VALUE_LO);
    diag[18] = read_reg(shizuku::ports::c6::SYSTIMER_UNIT0_VALUE_HI);
  } else {
    diag[17] = UINT32_MAX;
    diag[18] = UINT32_MAX;
  }
  diag[19] = read_reg(shizuku::ports::c6::SYSTIMER_TARGET2_CONF);
  diag[20] = read_reg(shizuku::ports::c6::SYSTIMER_REAL_TARGET2_HI);
  diag[21] = read_reg(shizuku::ports::c6::SYSTIMER_REAL_TARGET2_LO);
  diag[22] = read_reg(shizuku::ports::c6::SYSTIMER_INT_ENA);
  diag[23] = read_reg(shizuku::ports::c6::SYSTIMER_INT_RAW);
  diag[24] = read_reg(shizuku::ports::c6::SYSTIMER_INT_ST);
  const uint64_t task_alarm = read_timestamp_id(0);
  const uint64_t isr_alarm = read_timestamp_id(1);
  diag[25] = static_cast<uint32_t>(task_alarm);
  diag[26] = static_cast<uint32_t>(task_alarm >> 32);
  diag[27] = static_cast<uint32_t>(isr_alarm);
  diag[28] = static_cast<uint32_t>(isr_alarm >> 32);
  diag[29] = r_ble_ll_adv_enabled();
  diag[30] = read_reg(shizuku::ports::c6::PMU_CLK_STATE1);
  diag[31] = read_reg(shizuku::ports::c6::PMU_CLK_STATE2);
  const uint64_t esp_timer_now = static_cast<uint64_t>(esp_timer_get_time());
  diag[32] = static_cast<uint32_t>(esp_timer_now);
  diag[33] = static_cast<uint32_t>(esp_timer_now >> 32);
  diag[34] = read_reg(shizuku::ports::c6::PMU_MAIN_STATE);
  diag[35] = read_reg(shizuku::ports::c6::PMU_HP_ACTIVE_DIG_POWER);
  diag[36] = read_reg(shizuku::ports::c6::PMU_HP_MODEM_DIG_POWER);
  esp_chip_info_t chip_info{};
  esp_chip_info(&chip_info);
  timer_diag[6] = chip_info.revision;
  // These two words are read by the pinned C6 controller archive's
  // r_hal_timer_read_tick/set_exp_tick code. Their vendor field names,
  // units, and read-side effects are not documented in the public headers.
  timer_diag[7] = read_reg(0x600AE028u);
  timer_diag[8] = read_reg(0x600AE044u);
  timer_diag[9] = diag[7];
  timer_diag[10] = diag[8];
  timer_diag[11] = diag[11];
  timer_diag[12] = diag[12];
  timer_diag[13] = diag[13];
  timer_diag[14] = diag[14];
  timer_diag[15] = diag[30];
  timer_diag[16] = diag[31];
  timer_diag[17] = diag[6];
  timer_diag[18] = diag[10];
  timer_diag[19] = read_reg(INTMTX_CORE0_INT_STATUS_REG_0_REG);
  timer_diag[20] = read_reg(shizuku::ports::c6::PLIC_MXINT_PENDING);
  uint32_t mie = 0;
  asm volatile("csrr %0, mie" : "=r"(mie));
  timer_diag[21] = mie;
  diag[1] = phase;
  timer_diag[3] = phase;
  timer_diag[4] = timer_diag[4] + 1u;
  timer_diag[2] = sequence + 1u; // even: all payload words have been written
  timer_diag[5] = 0x56414c36u; // "6LAV": valid sentinel, published last
}
uint32_t pend_switch_count() { return shizuku_c6_timer_diag[6]; }
uint32_t default_bt_lpclk_hz() { return 100000u; }
} // namespace shizuku::esp_idf_support::ble_chip

extern "C" void shizuku_esp_ble_note_irq_handler_boundary(uint32_t source,
                                                            uint32_t event) {
  if (source == 4u) {
    const size_t index = event == 0u ? 2u : 3u;
    ::shizuku_c6_ble_exec_diag[index] += 1u;
    ::shizuku_c6_ble_timeline_diag[index] = static_cast<uint32_t>(shizuku::boards::esp32_c6::time_us());
  } else if (source == 7u) {
    const size_t index = event == 0u ? 4u : 5u;
    ::shizuku_c6_ble_exec_diag[index] += 1u;
    ::shizuku_c6_ble_timeline_diag[index] = static_cast<uint32_t>(shizuku::boards::esp32_c6::time_us());
  }
}

extern "C" void shizuku_esp_ble_note_compat_progress(uint32_t event,
                                                       uint32_t task_id,
                                                       uint32_t result) {
  if (event == 0u) {
    ::shizuku_c6_ble_exec_diag[6] += 1u;
    return;
  }
  if (event != 1u && event != 2u) return;
  // Bounded safe timestamp for ble_ll (task 2) and nimble_host (task 3).
  // Exclude task 1 (esp_timer) from systimer read to avoid 27 kHz overhead.
  if (task_id == 2u) {
    const size_t time_idx = event == 1u ? 6u : 7u;
    ::shizuku_c6_ble_timeline_diag[time_idx] = static_cast<uint32_t>(shizuku::boards::esp32_c6::time_us());
  } else if (task_id == 3u) {
    const size_t time_idx = event == 1u ? 8u : 9u;
    ::shizuku_c6_ble_timeline_diag[time_idx] = static_cast<uint32_t>(shizuku::boards::esp32_c6::time_us());
  }
  size_t slot = 8u;
  for (size_t i = 0; i < 8u; ++i) {
    const size_t base = 7u + i * 4u;
    if (::shizuku_c6_ble_exec_diag[base] == task_id) { slot = i; break; }
    if (slot == 8u && ::shizuku_c6_ble_exec_diag[base] == UINT32_MAX) slot = i;
  }
  if (slot == 8u) return;
  const size_t base = 7u + slot * 4u;
  ::shizuku_c6_ble_exec_diag[base] = task_id;
  if (event == 1u) ::shizuku_c6_ble_exec_diag[base + 1u] += 1u;
  else {
    ::shizuku_c6_ble_exec_diag[base + 2u] += 1u;
    ::shizuku_c6_ble_exec_diag[base + 3u] = result;
  }
}

namespace {
size_t grant_diag_object_slot(uint32_t object_id) {
  size_t free_slot = 8u;
  for (size_t slot = 0; slot < 8u; ++slot) {
    const size_t base = 3u + slot * 5u;
    if (::shizuku_c6_ble_grant_diag[base] == object_id) return slot;
    if (free_slot == 8u && ::shizuku_c6_ble_grant_diag[base] == UINT32_MAX)
      free_slot = slot;
  }
  if (free_slot < 8u)
    ::shizuku_c6_ble_grant_diag[3u + free_slot * 5u] = object_id;
  return free_slot;
}
size_t grant_diag_thread_slot(uint32_t thread_id) {
  for (size_t slot = 0; slot < 8u; ++slot) {
    const size_t base = 3u + slot * 5u;
    if (::shizuku_c6_ble_grant_diag[base] != UINT32_MAX &&
        ::shizuku_c6_ble_grant_diag[base + 1u] == thread_id)
      return slot;
  }
  return 8u;
}
}

extern "C" void shizuku_esp_ble_note_plain_spawn(uint32_t object_id,
                                                  uint32_t thread_id) {
  ensure_grant_diag_initialized();
  const size_t slot = grant_diag_object_slot(object_id);
  if (slot < 8u)
    ::shizuku_c6_ble_grant_diag[4u + slot * 5u] = thread_id;
}

extern "C" void shizuku_esp_ble_note_run_for_result(uint32_t task_id,
                                                      uint32_t a0_error,
                                                      uint32_t a1_value) {
  ensure_grant_diag_initialized();
  const size_t slot = grant_diag_thread_slot(task_id);
  if (slot >= 8u) {
    ::shizuku_c6_ble_grant_diag[2] += 1u;
    return;
  }
  const size_t base = 3u + slot * 5u;
  ::shizuku_c6_ble_grant_diag[base + 2u] = a0_error;
  ::shizuku_c6_ble_grant_diag[base + 3u] = a1_value;
  ::shizuku_c6_ble_grant_diag[base + 4u] += 1u;
}
