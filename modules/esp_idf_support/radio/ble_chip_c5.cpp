#include "ble_chip.hpp"
#include "trap_api.hpp"

namespace shizuku::boards::esp32_c5 { uint64_t time_us(); }
extern "C" volatile uint32_t shizuku_c5_timer_diag[7];

namespace shizuku::esp_idf_support::ble_chip {
uint64_t time_us() { return shizuku::boards::esp32_c5::time_us(); }
size_t irq_snapshot(irq_dispatch_snapshot *out, size_t capacity) {
  shizuku::chips::esp32_c5::irq_dispatch_snapshot rows[16]{};
  const size_t total = shizuku::chips::esp32_c5::irq_snapshot(rows, capacity < 16 ? capacity : 16);
  const size_t count = total < capacity ? total : capacity;
  for (size_t i = 0; out && i < count; ++i)
    out[i] = {rows[i].source, rows[i].dispatch_count};
  return total;
}
void install_traps() { shizuku::chips::esp32_c5::install_traps(); }
int prepare_radio() { return 0; }
void capture_radio_state(uint32_t) {}
uint32_t pend_switch_count() { return shizuku_c5_timer_diag[6]; }
uint32_t default_bt_lpclk_hz() { return 100000u; }
} // namespace shizuku::esp_idf_support::ble_chip
