#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/error.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "shizuku/kernel.hpp"
#include "shizuku/objects/flash_ops.hpp"

extern "C" bool shizuku_flash_operation_allowed()
    __attribute__((weak));

namespace shizuku::objects::flash_ops {
namespace {
// Acquire before parking: two cores must never simultaneously wait for each
// other in BOARD::park_other_cores(). The flag is released only after unpark.
volatile uint32_t g_busy = 0;

class operation_guard {
public:
  operation_guard() {
    while (!KERNEL::ARCH::cas32(&g_busy, kFree, kBusy)) {
      tight_loop_contents();
    }
    // Recheck after obtaining the global reservation. This closes the window
    // where a CYW43 poll started while this caller waited for another writer.
    if (shizuku_flash_operation_allowed != nullptr &&
        !shizuku_flash_operation_allowed()) {
      KERNEL::ARCH::store_release32(&g_busy, kFree);
      return;
    }
    KERNEL::BOARD::park_other_cores();
    irq_state_ = save_and_disable_interrupts();
    KERNEL::ARCH::protection_disable();
    ready_ = true;
  }
  ~operation_guard() {
    if (!ready_)
      return;
    KERNEL::ARCH::protection_enable();
    restore_interrupts(irq_state_);
    KERNEL::BOARD::resume_other_cores();
    KERNEL::ARCH::store_release32(&g_busy, kFree);
  }
  operation_guard(const operation_guard &) = delete;
  operation_guard &operator=(const operation_guard &) = delete;
  bool ready() const { return ready_; }

private:
  static constexpr uint32_t kFree = 0;
  static constexpr uint32_t kBusy = 1;
  uint32_t irq_state_ = 0;
  bool ready_ = false;
};

bool aligned_range(uint32_t offset, uint32_t bytes, uint32_t alignment) {
  return bytes != 0 && offset % alignment == 0 && bytes % alignment == 0 &&
         offset <= PICO_FLASH_SIZE_BYTES &&
         bytes <= PICO_FLASH_SIZE_BYTES - offset;
}
} // namespace

int erase(uint32_t offset, uint32_t bytes, timing *measurement) {
  if (!aligned_range(offset, bytes, FLASH_SECTOR_SIZE))
    return PICO_ERROR_INVALID_ARG;
  if (shizuku_flash_operation_allowed != nullptr &&
      !shizuku_flash_operation_allowed())
    return PICO_ERROR_RESOURCE_IN_USE;
  operation_guard guard;
  if (!guard.ready())
    return PICO_ERROR_RESOURCE_IN_USE;
  const uint64_t began = time_us_64();
  ::flash_range_erase(offset, bytes);
  if (measurement != nullptr)
    measurement->elapsed_us = (uint32_t)(time_us_64() - began);
  return PICO_OK;
}

int program(uint32_t offset, const uint8_t *data, uint32_t bytes,
            timing *measurement) {
  if (data == nullptr || !aligned_range(offset, bytes, FLASH_PAGE_SIZE))
    return PICO_ERROR_INVALID_ARG;
  if (shizuku_flash_operation_allowed != nullptr &&
      !shizuku_flash_operation_allowed())
    return PICO_ERROR_RESOURCE_IN_USE;
  operation_guard guard;
  if (!guard.ready())
    return PICO_ERROR_RESOURCE_IN_USE;
  const uint64_t began = time_us_64();
  ::flash_range_program(offset, data, bytes);
  if (measurement != nullptr)
    measurement->elapsed_us = (uint32_t)(time_us_64() - began);
  return PICO_OK;
}

} // namespace shizuku::objects::flash_ops
