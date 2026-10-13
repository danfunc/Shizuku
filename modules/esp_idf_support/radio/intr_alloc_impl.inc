// Shared ESP-IDF interrupt adapter body. Chip-specific wrappers provide the
// trap API, CPU-line range, reserved line, and exported C entrypoint names.
#include "osi_sync.hpp"
#include "esp_intr_alloc.h"
#include "soc/interrupts.h"
#include <cstdint>

extern "C" uint32_t shizuku_freertos_compat_current_task_id(void) __attribute__((weak));
extern "C" void shizuku_esp_ble_irq_mask_begin(uint32_t, uint32_t, uint32_t) __attribute__((weak));
extern "C" void shizuku_esp_ble_irq_mask_end(uint32_t) __attribute__((weak));
extern "C" void shizuku_esp_ble_note_irq_registered(uint32_t, uint32_t, uint32_t)
    __attribute__((weak));
extern "C" void shizuku_esp_ble_note_irq_unregistered(uint32_t)
    __attribute__((weak));

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
allocation allocations[SHIZUKU_INTR_SLOT_COUNT];
uint32_t next_irq = SHIZUKU_INTR_FIRST_IRQ;
volatile uint32_t allocation_lock = 0;

struct interrupt_port {
  static uint64_t now_us() { return 0; }
  static void sleep_us(uint32_t) {}
  static void yield() {}
  static uint32_t irq_save() {
#if defined(__riscv)
    uintptr_t old = 0;
    asm volatile("csrrc %0, mstatus, %1" : "=r"(old) : "r"(uintptr_t{8}) : "memory");
#if defined(SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG) && SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
    if ((old & 8u) && shizuku_esp_ble_irq_mask_begin)
      shizuku_esp_ble_irq_mask_begin(1u,
          static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))),
          shizuku_freertos_compat_current_task_id
              ? shizuku_freertos_compat_current_task_id() : 0u);
#endif
    return (old & 8u) ? 1u : 0u;
#else
    return 0;
#endif
  }
  static void irq_restore(uint32_t key) {
#if defined(__riscv)
    if (key & 1u) {
#if defined(SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG) && SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
      if (shizuku_esp_ble_irq_mask_end) shizuku_esp_ble_irq_mask_end(1u);
#endif
      asm volatile("csrsi mstatus, 8" ::: "memory");
    }
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
  if (source < 0 || source >= ETS_MAX_INTR_SOURCE ||
      !SHIZUKU_INTR_SOURCE_AVAILABLE(source) || !handler) return false;
  int level = esp_intr_flags_to_level(flags);
  if (level == 0) level = 1;
  if (level > SHIZUKU_INTR_MAX_LEVEL) return false;
  const bool edge = (flags & ESP_INTR_FLAG_EDGE) != 0;
  const bool enabled = (flags & ESP_INTR_FLAG_INTRDISABLED) == 0;
  shizuku::esp_idf_support::osi::spin_guard<interrupt_port> guard(allocation_lock);
  allocation *slot = nullptr;
  for (auto &entry : allocations) if (!entry.used) { slot = &entry; break; }
  if (!slot) return false;
  for (uint32_t n = 0; n < SHIZUKU_INTR_LINE_COUNT; ++n) {
    const uint32_t cpu_irq = SHIZUKU_INTR_FIRST_IRQ +
        ((next_irq - SHIZUKU_INTR_FIRST_IRQ + n) % SHIZUKU_INTR_LINE_COUNT);
    if (!SHIZUKU_INTR_LINE_AVAILABLE(cpu_irq)) continue;
    const int rc = SHIZUKU_INTR_CHIP::irq_register(
        static_cast<uint32_t>(source), cpu_irq, handler, arg,
        static_cast<uint32_t>(flags), enabled, static_cast<uint32_t>(level), edge);
    if (rc == -2) continue;
    if (rc != 0) return false;
    next_irq = (cpu_irq == SHIZUKU_INTR_LAST_IRQ)
        ? SHIZUKU_INTR_FIRST_IRQ : cpu_irq + 1;
    *slot = {static_cast<uint32_t>(source), cpu_irq, handler, arg, enabled, true};
    if (shizuku_esp_ble_note_irq_registered)
      shizuku_esp_ble_note_irq_registered(static_cast<uint32_t>(source), cpu_irq,
                                          static_cast<uint32_t>(flags));
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
    const int rc = SHIZUKU_INTR_CHIP::irq_set_enabled(
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
    const int rc = SHIZUKU_INTR_CHIP::irq_unregister(
        entry.source, entry.cpu_irq, entry.handler, entry.arg);
    if (rc != 0) return false;
    if (shizuku_esp_ble_note_irq_unregistered)
      shizuku_esp_ble_note_irq_unregistered(entry.source);
    entry = {};
    return true;
  }
  return false;
}
} // namespace

extern "C" int SHIZUKU_INTR_ALLOC_NAME(int source, int flags, handler_t handler,
                                        void *arg, void **ret_handle) {
  if (ret_handle) *ret_handle = nullptr;
  return allocate(source, flags, handler, arg, ret_handle) ? 0 : -1;
}
extern "C" int SHIZUKU_INTR_FREE_NAME(void *handle) {
  return release(handle) ? 0 : -1;
}
extern "C" int __wrap_esp_intr_alloc(int source, int flags, handler_t handler,
                                     void *arg, void **ret_handle) {
  return SHIZUKU_INTR_ALLOC_NAME(source, flags, handler, arg, ret_handle);
}
extern "C" int __wrap_esp_intr_free(void *handle) { return SHIZUKU_INTR_FREE_NAME(handle); }
extern "C" int __wrap_esp_intr_enable(void *handle) { return set_enabled(handle, true) ? 0 : -1; }
extern "C" int __wrap_esp_intr_disable(void *handle) { return set_enabled(handle, false) ? 0 : -1; }
