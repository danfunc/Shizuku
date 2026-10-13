#ifndef SHIZUKU_ESP_IDF_SUPPORT_BLE_CHIP_HPP
#define SHIZUKU_ESP_IDF_SUPPORT_BLE_CHIP_HPP

#include <cstddef>
#include <cstdint>

namespace shizuku::esp_idf_support::ble_chip {
struct irq_dispatch_snapshot {
  uint32_t source;
  uint32_t dispatch_count;
};

uint64_t time_us();
size_t irq_snapshot(irq_dispatch_snapshot *out, size_t capacity);
void install_traps();
int prepare_radio();
void capture_radio_state(uint32_t phase);
uint32_t pend_switch_count();
uint32_t default_bt_lpclk_hz();
} // namespace shizuku::esp_idf_support::ble_chip

#endif
