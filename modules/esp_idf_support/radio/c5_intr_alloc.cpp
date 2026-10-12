// Narrow ESP-IDF C5 interrupt-allocation adapter for the BLE controller.
// This is not a replacement for the general esp_intr_alloc API.
#include "trap_api.hpp"
#include "osi_sync.hpp"
#include "esp_intr_alloc.h"

#include <cstdint>

namespace {
using handler_t = void (*)(void *);
struct allocation {
  uint32_t source = UINT32_MAX;
  uint32_t cpu_irq = UINT32_MAX;
  handler_t handler = nullptr;
  void *arg = nullptr;
  bool enabled = false;
  bool used = false;
};
allocation allocations[14];
uint32_t next_irq = 18; // 17 is reserved by Shizuku's SYSTIMER.
volatile uint32_t allocation_lock = 0;

struct interrupt_port {
  static uint64_t now_us() { return 0; }
  static void sleep_us(uint32_t) {}
  static void yield() {}
  static uint32_t irq_save() {
#if defined(__riscv)
    uintptr_t old = 0;
    asm volatile("csrrc %0, mstatus, %1" : "=r"(old) : "r"(uintptr_t{8}) : "memory");
    return (old & 8u) ? 1u : 0u;
#else
    return 0;
#endif
  }
  static void irq_restore(uint32_t key) {
#if defined(__riscv)
    if (key & 1u) asm volatile("csrsi mstatus, 8" ::: "memory");
#else
    (void)key;
#endif
  }
  static uint32_t self() { return 1; }
  static bool cas32(volatile uint32_t *p, uint32_t expected, uint32_t desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  }
};

bool allocate(int source, int flags, handler_t handler, void *arg, void **out) {
  if (source < 0 || source >= 128 || !handler) return false;
  int level = esp_intr_flags_to_level(flags);
  if (level == 0) level = 1; // IDF's allocator defaults an unspecified level to level 1.
  if (level > 3) return false; // C-level Shizuku trap dispatch does not support high-priority assembly ISRs.
  const bool edge = (flags & ESP_INTR_FLAG_EDGE) != 0;
  const bool enabled = (flags & ESP_INTR_FLAG_INTRDISABLED) == 0;
  shizuku::esp_idf_support::osi::spin_guard<interrupt_port> guard(allocation_lock);
  allocation *slot = nullptr;
  for (auto &entry : allocations) if (!entry.used) { slot = &entry; break; }
  if (!slot) return false;
  for (uint32_t n = 0; n < 14; ++n) {
    const uint32_t cpu_irq = 18 + ((next_irq - 18 + n) % 14);
    const int rc = shizuku::chips::esp32_c5::irq_register(
        static_cast<uint32_t>(source), cpu_irq, handler, arg, static_cast<uint32_t>(flags), enabled,
        static_cast<uint32_t>(level), edge);
    if (rc == -2) continue; // source/CPU line already assigned; try next free line.
    if (rc != 0) return false;
    next_irq = (cpu_irq == 31) ? 18 : cpu_irq + 1;
    *slot = {static_cast<uint32_t>(source), cpu_irq, handler, arg, enabled, true};
    if (out) *out = slot;
    return true;
  }
  return false;
}

bool set_enabled(void *opaque, bool enabled) {
  if (!opaque) return false;
  shizuku::esp_idf_support::osi::spin_guard<interrupt_port> guard(allocation_lock);
  for (auto &entry : allocations) {
    if (&entry != opaque || !entry.used) continue;
    if (entry.enabled == enabled) return true;
    const int rc = shizuku::chips::esp32_c5::irq_set_enabled(
        entry.source, entry.cpu_irq, entry.handler, entry.arg, enabled);
    if (rc != 0) return false;
    entry.enabled = enabled;
    return true;
  }
  return false;
}

bool release(void *opaque) {
  if (!opaque) return false;
  shizuku::esp_idf_support::osi::spin_guard<interrupt_port> guard(allocation_lock);
  auto *slot = static_cast<allocation *>(opaque);
  for (auto &entry : allocations) {
    if (&entry != slot || !entry.used) continue;
    const int rc = shizuku::chips::esp32_c5::irq_unregister(
        entry.source, entry.cpu_irq, entry.handler, entry.arg);
    if (rc != 0) return false;
    entry = {};
    return true;
  }
  return false;
}
} // namespace

extern "C" int shizuku_c5_intr_alloc(int source, int flags, handler_t handler,
                                      void *arg, void **ret_handle) {
  if (ret_handle) *ret_handle = nullptr;
  return allocate(source, flags, handler, arg, ret_handle) ? 0 : -1;
}

extern "C" int shizuku_c5_intr_free(void *handle) {
  return release(handle) ? 0 : -1;
}

// BT ext_funcs and esp_timer use IDF's interrupt allocation API. BLE-only
// firmware routes allocation and enable-state changes through the C5 table
// instead of IDF's FreeRTOS-backed global allocator.
extern "C" int __wrap_esp_intr_alloc(int source, int flags, handler_t handler,
                                     void *arg, void **ret_handle) {
  return shizuku_c5_intr_alloc(source, flags, handler, arg, ret_handle);
}
extern "C" int __wrap_esp_intr_free(void *handle) {
  return shizuku_c5_intr_free(handle);
}
extern "C" int __wrap_esp_intr_enable(void *handle) {
  return set_enabled(handle, true) ? 0 : -1;
}
extern "C" int __wrap_esp_intr_disable(void *handle) {
  return set_enabled(handle, false) ? 0 : -1;
}
