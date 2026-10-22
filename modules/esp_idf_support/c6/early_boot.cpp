// Stage-2 / Stage-3 bootstrap for ESP32-C6. No IDF OS shim, heap or serial observer.
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/method_entry.hpp"
#include "chips/esp32_c6/chip.hpp"
#include "chips/esp32_c6/trap_api.hpp"
#include "chips/esp32_c6/selftest_record.h"

#ifndef SHIZUKU_IRQ_DELAY_US
#define SHIZUKU_IRQ_DELAY_US 0
#endif
static_assert(SHIZUKU_IRQ_DELAY_US >= 0 && SHIZUKU_IRQ_DELAY_US <= 100,
              "C6 IRQ handler delay must be 0..100 us");

extern "C" void shizuku_freertos_compat_isr_enter(void) __attribute__((weak));
extern "C" void shizuku_freertos_compat_isr_exit(void) __attribute__((weak));
extern "C" uint32_t shizuku_freertos_compat_current_task_id(void) __attribute__((weak));
extern "C" void shizuku_esp_ble_note_irq_dispatch(uint32_t, uint32_t, uint32_t) __attribute__((weak));
extern "C" void shizuku_esp_ble_note_irq_handler_boundary(uint32_t, uint32_t) __attribute__((weak));

extern "C" void shizuku_esp_ble_pend_compat_context_switch(void) {
  shizuku::chips::esp32_c6::plic_vector_mode::pend_context_switch();
}

namespace {
struct c6_idf_ble_freertos_policy {
  static void on_enter(uint32_t source, uint32_t mepc) {
    if (shizuku_freertos_compat_isr_enter) shizuku_freertos_compat_isr_enter();
    if (shizuku_esp_ble_note_irq_dispatch) {
      shizuku_esp_ble_note_irq_dispatch(
          source, mepc,
          shizuku_freertos_compat_current_task_id
              ? shizuku_freertos_compat_current_task_id()
              : 0u);
    }
    if ((source == 4u || source == 7u) &&
        shizuku_esp_ble_note_irq_handler_boundary) {
      shizuku_esp_ble_note_irq_handler_boundary(source, 0u);
    }
#if SHIZUKU_IRQ_DELAY_US > 0
    if (source == 4u || source == 7u) {
      const uint64_t delay_start = shizuku::chips::esp32_c6::systimer_now();
      const uint64_t delay_ticks =
          static_cast<uint64_t>(SHIZUKU_IRQ_DELAY_US) * 16u;
      while (shizuku::chips::esp32_c6::systimer_now() - delay_start < delay_ticks) {
      }
    }
#endif
  }

  static void on_exit(uint32_t source) {
    if ((source == 4u || source == 7u) &&
        shizuku_esp_ble_note_irq_handler_boundary) {
      shizuku_esp_ble_note_irq_handler_boundary(source, 1u);
    }
    if (shizuku_freertos_compat_isr_exit) shizuku_freertos_compat_isr_exit();
  }
};
} // namespace

#if defined(SHIZUKU_ESP_BLE)
extern "C" [[noreturn]] void shizuku_esp_ble_stage1_start(void);
#endif
extern "C" void shizuku_c6_mark_boot_phase(uint32_t);

extern "C" {
// entered, ECALL round-trip, live counter, observed error, observed value.
volatile uint32_t shizuku_c6_thread0_diag[5];
// entered marker, live counter, spawn error, spawned id, reserved.
volatile uint32_t shizuku_c6_thread1_diag[5];
// thread0 grant returns: [0]=EXPIRED count, [1]=YIELDED count, [2]=error count, [3]=last raw reply.
volatile uint32_t shizuku_c6_switch_diag[4];
}

namespace {
// Stage-2 invariant: minimal pilot thread record storage declaration (checked by test_trap_static.py)
alignas(16) shizuku::KERNEL::thread_record records[2];
alignas(16) shizuku::KERNEL::call_ledger_entry ledger1[4];
alignas(16) unsigned char thread1_stack[4096];
constexpr uintptr_t GRANT_TICKS = 160000;  // 10 ms of 16 MHz SYSTIMER ticks

// Routing調停ハンドラ:
// 0xC6 マーカーに対しては段2の diagnostic_handler として動作 (command=1 で GRANT プリミティブ発行、それ以外は 0xC6000002 返却)。
// それ以外のすべての SVC は KERNEL_OBJECT ハンドラへルーティングして selftest 等の通常オブジェクト API を実行。
uintptr_t c6_combined_handler(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                              uintptr_t a3, uintptr_t unwind_floor) {
  if (a0 == 0xC6) {
    if (a1 == 1) {
      // Runs as the kernel object, so GRANT is interpreted as a primitive.
      const auto reply = shizuku::ARCH::syscall(static_cast<uintptr_t>(shizuku::primitive::GRANT), 1, GRANT_TICKS);
      return 0xC6100000u | (static_cast<uint32_t>(reply.error) << 4) | static_cast<uint32_t>(reply.value & 0xf);
    }
    return 0xC6000002;
  }
  return shizuku::kernel_object_instance.handle(a0, a1, a2, a3, unwind_floor);
}

[[noreturn]] void thread1() {
  shizuku_c6_thread1_diag[0] = 0xC6100001;
  for (;;) {
    shizuku_c6_thread1_diag[1] = shizuku_c6_thread1_diag[1] + 1;
    asm volatile("fence rw,rw" ::: "memory");
  }
}

[[noreturn]] void thread0() {
  shizuku_c6_mark_boot_phase(0xC603u);
  // 1. 段2 diagnostic 初期化 & ECALL round-trip 検証
  shizuku_c6_thread0_diag[0] = 0xC6000001;
  auto result = shizuku::ARCH::syscall(0xC6);
  shizuku_c6_thread0_diag[3] = result.error;
  shizuku_c6_thread0_diag[4] = result.value;
  if (result.error == 0 && result.value == 0xC6000002 && !shizuku::ARCH::in_interrupt())
    shizuku_c6_thread0_diag[1] = 0xC6000002;

  // 2. BLE profile enters the shared ESP-IDF NimBLE stage after the kernel is live.
#if defined(SHIZUKU_ESP_BLE)
  shizuku_c6_mark_boot_phase(0xC610u);
  shizuku_esp_ble_stage1_start();
#else
  // 2. 実 boot 経路での selftest runner 実行 (RAM record への集中記録)
  shizuku_c6_run_selftests();
#endif

  // 3. 段2 thread1 スポーン (GRANT スイッチ試験用)
  shizuku::KERNEL::spawn_request request{};
  request.entry_pc = reinterpret_cast<uintptr_t>(&thread1);
  request.protection = shizuku::PROTECTION_PRIVILEGED;
  request.stack_base = reinterpret_cast<uintptr_t>(thread1_stack);
  request.stack_bytes = sizeof(thread1_stack);
  request.object_id = 2;
  request.parent_handler_object = 0; // KERNEL_OBJECT_ID
  request.parent_handler_entry = shizuku::method_entry<shizuku::ARCH, &c6_combined_handler>();
  request.ledger = ledger1;
  request.ledger_capacity = 4;
  const auto spawned = shizuku::kernel_instance.spawn(request);
  shizuku_c6_thread1_diag[2] = static_cast<uint32_t>(spawned.error);
  shizuku_c6_thread1_diag[3] = spawned.thread;

  // 4. 段2 スイッチループ (thread0/thread1 相互スイッチ & 生存カウンタ更新)
  for (;;) {
    shizuku_c6_thread0_diag[2] = shizuku_c6_thread0_diag[2] + 1;
    if (spawned.error == shizuku::kernel_error::OK) {
      const auto reply = shizuku::ARCH::syscall(0xC6, 1);
      shizuku_c6_switch_diag[3] = static_cast<uint32_t>(reply.value);
      if (reply.error != 0 || (reply.value & 0xFFFFFF00u) != 0xC6100000u)
        shizuku_c6_switch_diag[2] = shizuku_c6_switch_diag[2] + 1;
      else
        shizuku_c6_switch_diag[reply.value & 0xf] = shizuku_c6_switch_diag[reply.value & 0xf] + 1;
    }
    asm volatile("fence rw,rw" ::: "memory");
  }
}
} // namespace

extern "C" [[noreturn]] void shizuku_c6_early_boot() {
  shizuku::chips::esp32_c6::set_irq_dispatch_hooks(
      shizuku::chips::esp32_c6::make_irq_dispatch_hooks<c6_idf_ble_freertos_policy>());
  auto &kernel = shizuku::kernel_instance;
  kernel.init();
  shizuku::kernel_object_instance.init();
  kernel.set_object_handler(shizuku::method_entry<shizuku::ARCH, &c6_combined_handler>(), 0);
  shizuku_c6_mark_boot_phase(0xC602u);
  const auto boot = shizuku::kernel_object_instance.lend_boot_stack();
  kernel.bootstrap(thread0, boot.base, boot.bytes, boot.ledger, boot.ledger_capacity);
  __builtin_unreachable();
}
