// Stage-2 / Stage-3 bootstrap for ESP32-C6. No IDF OS shim, heap or serial observer.
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/method_entry.hpp"
#include "chips/esp32_c6/chip.hpp"
#include "chips/esp32_c6/trap_api.hpp"
#include "chips/esp32_c6/selftest_record.h"
#include "shizuku/object_ids.hpp"
#include "boards/esp32_c6/esp32_c6.hpp"
#include "boards/esp32_c6/led_object.hpp"
#include "modules/led_blink/blink.hpp"

// LED 点滅の有無 (旧 heartbeat と同じスイッチ名)。0 で LED オブジェクトも点滅も組まない。
#ifndef SHIZUKU_C6_HEARTBEAT_LED
#define SHIZUKU_C6_HEARTBEAT_LED 1
#endif

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

// ---- LED 点滅 (led_blink) の組立 -------------------------------------------------
// 親は ROOT (thread0 の名乗り)。LED の WRITE は親の中継メソッドを親子呼出しで叩く (kernel_role は付けない)。
// 番号は tools/gen_object_ids.py の表から取る (pico_sdk → apps → selftest の順に 2 から振る)。
// led = 5 (pico_sdk の 4 番目)、blink = 26 (selftest の 13 番目)。C6 の ladder が名前で作るのは
// selftest の 14..68 (leaf..desc_relay_target 等) だけで、led・blink とは重ならない。
// 数値 2 は起点 thread1 の object タグで、表の gpio と同じ値。C6 では gpio を作らない。
// 段ごとの失敗は shizuku_c6_led_diag に残し、失敗しても起動は止めない (点滅は補助機能)。
volatile uint32_t shizuku_c6_led_diag[2]; // [0] = 失敗した段 (0 = 組めた), [1] = その error / 点滅スレッド番号

constexpr uintptr_t LED_OBJECT_ID = shizuku::object_id::led;
constexpr uintptr_t BLINK_OBJECT_ID = shizuku::object_id::blink;
constexpr uintptr_t ROOT_RELAY_METHOD = 1;   // ROOT が export する LED 中継の番号
constexpr uint64_t BLINK_PERIOD_US = shizuku::led_blink::DEFAULT_PERIOD_US; // 2 Hz 点滅
constexpr uint64_t BLINK_BUDGET_US = 500;    // 点滅スレッドの予算 (stress と同じ。取り上げられる最悪待ちを短く)

// led_blink の Env。時刻と syscall を C6 の実物へ束ねる。実機では止める理由が無いので running は常に真。
struct c6_blink_env {
  static auto syscall(uintptr_t number, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return shizuku::ARCH::syscall(number, a1, a2, a3);
  }
  static uint64_t now_us() { return shizuku::boards::esp32_c6::time_us(); }
  static bool running() { return true; }
  static void on_period(uint64_t) {}
};

// LED オブジェクトの main。GPIO を出力にして消灯し、WRITE の口を自分で export する。戻り値は export の失敗数。
uintptr_t led_object_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  shizuku::boards::esp32_c6_led::led_init();
  return shizuku::ARCH::syscall(
             (uintptr_t)shizuku::object_api::EXPORT_METHOD, shizuku::led_blink::LED_WRITE_METHOD,
             shizuku::method_entry<shizuku::ARCH, &shizuku::boards::esp32_c6_led::led_write>())
      .error;
}

// 段を順に組む。どこかで失敗したら、その段と error を diag に残して戻る (残りは作らない)。
void start_led_blink() {
  auto ok = [](uint32_t stage, uintptr_t error) {
    if (error != 0) {
      shizuku_c6_led_diag[0] = stage;
      shizuku_c6_led_diag[1] = static_cast<uint32_t>(error);
      return false;
    }
    return true;
  };
  // 1. LED オブジェクト (特権) を作り、main を 1 回呼んで WRITE を export させる。
  const auto led = shizuku::ARCH::syscall((uintptr_t)shizuku::object_api::CREATE_OBJECT, LED_OBJECT_ID,
                                          shizuku::method_entry<shizuku::ARCH, &led_object_main>(),
                                          shizuku::OBJECT_PRIVILEGED);
  if (!ok(1, led.error)) return;
  const auto led_main = shizuku::ARCH::syscall((uintptr_t)shizuku::object_api::CALL_METHOD, LED_OBJECT_ID, 0, 0);
  if (!ok(2, led_main.error) || !ok(3, led_main.value)) return;
  // 2. 親 (ROOT) が LED への中継を export する。点滅はこれを親子呼出しで叩く。
  const auto relay = shizuku::ARCH::syscall(
      (uintptr_t)shizuku::object_api::EXPORT_METHOD, ROOT_RELAY_METHOD,
      shizuku::method_entry<shizuku::ARCH, &shizuku::led_blink::led_relay_entry<c6_blink_env, LED_OBJECT_ID>>());
  if (!ok(4, relay.error)) return;
  // 3. 点滅スレッド (特権) を ROOT の子として作り、予算を短くして走らせる。
  const auto blink = shizuku::ARCH::syscall(
      (uintptr_t)shizuku::object_api::CREATE_OBJECT, BLINK_OBJECT_ID,
      shizuku::method_entry<shizuku::ARCH,
                            &shizuku::led_blink::blink_entry<c6_blink_env, shizuku::KERNEL_OBJECT::ROOT_OBJECT,
                                                             ROOT_RELAY_METHOD, BLINK_PERIOD_US>>(),
      shizuku::OBJECT_PRIVILEGED);
  if (!ok(5, blink.error)) return;
  const auto spawned = shizuku::ARCH::syscall((uintptr_t)shizuku::object_api::SPAWN, BLINK_OBJECT_ID, 0, 0);
  if (!ok(6, spawned.error)) return;
  const auto budget = shizuku::ARCH::syscall((uintptr_t)shizuku::object_api::SET_BUDGET, spawned.value,
                                             BLINK_BUDGET_US * shizuku::boards::esp32_c6::cycles_per_us());
  if (!ok(7, budget.error)) return;
  shizuku_c6_led_diag[1] = static_cast<uint32_t>(spawned.value);
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
  // BLE 側は selftest を走らせないので、点滅はここで組む (stage1 は戻らない)。
  if constexpr (SHIZUKU_C6_HEARTBEAT_LED != 0) start_led_blink();
  shizuku_esp_ble_stage1_start();
#else
  // 2. 実 boot 経路での selftest runner 実行 (RAM record への集中記録)
  shizuku_c6_run_selftests();
  // 点滅は selftest の後で組む (selftest の計測と期待値に点滅を混ぜない)。
  if constexpr (SHIZUKU_C6_HEARTBEAT_LED != 0) start_led_blink();
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
    // 点滅 (blink) に番を回す。grant 段の無い起点スレッドの YIELD は kobj の schedule() へ入り、
    // 締切を過ぎた点滅を選ぶ。これが無いと schedule() は一度も呼ばれず、点滅は SPAWN 後も走らない。
    shizuku::ARCH::syscall((uintptr_t)shizuku::object_api::YIELD);
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
