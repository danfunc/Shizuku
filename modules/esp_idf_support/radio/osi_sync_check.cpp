// osi_sync.hpp を RV32 (--config=esp32c6) で明示実体化して、コンパイルが通ることだけを確かめる。
// 実機の PORT (object_api syscall を包むもの) ができたら、ここを本物の PORT に差し替える。
#include "osi_sync.hpp"

namespace {
struct check_port {
  static uint64_t now_us() { return 0; }
  static void sleep_us(uint32_t) {}
  static void yield() {}
  static uint32_t irq_save() { return 0; }
  static void irq_restore(uint32_t) {}
  static uint32_t self() { return 1; }
  static bool cas32(volatile uint32_t *p, uint32_t e, uint32_t d) {
    return __atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  }
};
} // namespace

namespace shizuku::esp_idf_support::osi {
template struct semaphore<check_port>;
template struct mutex<check_port>;
template struct queue<check_port>;
template struct event_group<check_port>;
} // namespace shizuku::esp_idf_support::osi
