#pragma once
#include <cstdlib>
#include <chrono>
#include <cstring>
#include "shizuku/templates/kernel_object.hpp"
#include "shizuku/archs/dummy.hpp"
#include "shizuku/templates/cpu_manager.hpp"
#include "shizuku/templates/kernel.hpp"

namespace shizuku {
struct TestArch : archs::dummy {
  static inline thread_local uint32_t remaining = 0;
  static inline thread_local bool pending = false;
  static bool cas32(volatile uint32_t *p, uint32_t expected, uint32_t desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  }
  static uint32_t load_acquire32(volatile uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
  static void store_release32(volatile uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
  static constexpr uint32_t TIMER_MIN_CYCLES = 10;
  static uint32_t timer_remaining(bool &wrapped) { wrapped = false; return remaining; }
  static void timer_oneshot(uint32_t cycles) { remaining = cycles; }
  static void timer_cancel() { remaining = 0; }
  static void pend_context_switch() { pending = true; }
  static void prepare_thread_entry(context_t &ctx, uintptr_t top, uintptr_t, uintptr_t, uintptr_t) {
    ctx.sp = (exception_frame_t *)(top - sizeof(exception_frame_t));
    *ctx.sp = {};
  }
  static void set_region_window(context_t &, uintptr_t, uintptr_t) {}
  static void debug_step(bool) {}
  static uint32_t debug_reason_take() { return 0; }
  static bool faulted_in_thread_mode(const context_t &) { return true; }
  static uint32_t fault_status() { return 0; }
  static uintptr_t fault_address() { return 0; }
  static void fault_status_clear() {}
};
struct TestBoard {
  static inline thread_local uint32_t core = 0;
  static inline void (*notify)(uint32_t) = nullptr;
  static void notify_core(uint32_t core) { if (notify) notify(core); }
  static uint32_t core_num() { return core; }
  static void init(uint32_t) {}
  static void launch_core(void (*)()) {}
  static void park_other_cores() {}
  static void resume_other_cores() {}
  static void diag_mute(bool) {}
  static int dma_claim() { return -1; }
  static inline bool dma_active = false;
  static bool dma_busy(int) { return dma_active; }
  static void dma_copy(int, const void *from, void *to, uint32_t bytes) { std::memcpy(to, from, bytes); }
  static void dma_release(int) {}
  static uint64_t time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
  static uint32_t cycles_per_us() { return 150; }
  static uintptr_t unprivileged_floor() { return UINTPTR_MAX; }
  static void diag_printf(const char *, ...) {}
  [[noreturn]] static void panic(const char *) { std::abort(); }
};
struct TestMemory {
  alignas(8) static inline uint8_t heap[128 * 1024];
  struct allocation { explicit operator bool() const { return true; } void *value() const { return heap; } };
  allocation kernel_malloc(uintptr_t) { return {}; }
  bool init() { return true; }
};
using CPU_MANAGER = templates::cpu_manager<TestArch, TestBoard, 2>;
using KERNEL = templates::kernel<CPU_MANAGER, TestMemory>;
using KERNEL_OBJECT = templates::kernel_object<KERNEL, 16, 8, 8, 16>;
}
