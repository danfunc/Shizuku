// BLE-stage-only observers for ESP-IDF esp_libc lock entrypoints.
// This file does not provide libc locking; wrappers record the lock handle and
// forward to esp_libc's original FreeRTOS-backed implementation.
#include <cstdint>
#include <sys/lock.h>

extern "C" void shizuku_c5_ble_note_libc_lock(uint32_t lock_address,
                                               uint32_t lock_value,
                                               uint32_t api_kind,
                                               uint32_t caller_pc);

extern "C" void __real__lock_acquire(_lock_t *lock);
extern "C" void __real__lock_acquire_recursive(_lock_t *lock);
extern "C" int __real__lock_try_acquire(_lock_t *lock);
extern "C" int __real__lock_try_acquire_recursive(_lock_t *lock);
extern "C" void __real___retarget_lock_acquire(_LOCK_T lock);
extern "C" void __real___retarget_lock_acquire_recursive(_LOCK_T lock);
extern "C" int __real___retarget_lock_try_acquire(_LOCK_T lock);
extern "C" int __real___retarget_lock_try_acquire_recursive(_LOCK_T lock);

namespace {
void record_lock(void *lock_address, uint32_t kind, bool dereference, uint32_t pc) {
  const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lock_address));
  uint32_t value = 0;
  if (dereference && lock_address)
    value = *static_cast<volatile uint32_t *>(lock_address);
  shizuku_c5_ble_note_libc_lock(address, value, kind, pc);
}
} // namespace

extern "C" void __wrap__lock_acquire(_lock_t *lock) {
  record_lock(lock, 1, true, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  __real__lock_acquire(lock);
}
extern "C" void __wrap__lock_acquire_recursive(_lock_t *lock) {
  record_lock(lock, 2, true, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  __real__lock_acquire_recursive(lock);
}
extern "C" int __wrap__lock_try_acquire(_lock_t *lock) {
  record_lock(lock, 3, true, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  return __real__lock_try_acquire(lock);
}
extern "C" int __wrap__lock_try_acquire_recursive(_lock_t *lock) {
  record_lock(lock, 4, true, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  return __real__lock_try_acquire_recursive(lock);
}
extern "C" void __wrap___retarget_lock_acquire(_LOCK_T lock) {
  record_lock(lock, 5, false, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  __real___retarget_lock_acquire(lock);
}
extern "C" void __wrap___retarget_lock_acquire_recursive(_LOCK_T lock) {
  record_lock(lock, 6, false, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  __real___retarget_lock_acquire_recursive(lock);
}
extern "C" int __wrap___retarget_lock_try_acquire(_LOCK_T lock) {
  record_lock(lock, 7, false, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  return __real___retarget_lock_try_acquire(lock);
}
extern "C" int __wrap___retarget_lock_try_acquire_recursive(_LOCK_T lock) {
  record_lock(lock, 8, false, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  return __real___retarget_lock_try_acquire_recursive(lock);
}
