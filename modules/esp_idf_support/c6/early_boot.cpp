// Stage-2 / Stage-3 bootstrap for ESP32-C6. No IDF OS shim, heap or serial observer.
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "chips/esp32_c6/selftest_record.h"

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

// Composition firmware entry and legacy selftest runner declarations (weakly linked)
extern "C" {
[[gnu::weak]] extern bool shizuku_firmware_entry(void);
[[gnu::weak]] extern void shizuku_c6_run_selftests(void);
}

[[noreturn]] void thread0() {
  // 1. 段2 diagnostic 初期化 & ECALL round-trip 検証
  shizuku_c6_thread0_diag[0] = 0xC6000001;
  auto result = shizuku::ARCH::syscall(0xC6);
  shizuku_c6_thread0_diag[3] = result.error;
  shizuku_c6_thread0_diag[4] = result.value;
  if (result.error == 0 && result.value == 0xC6000002 && !shizuku::ARCH::in_interrupt())
    shizuku_c6_thread0_diag[1] = 0xC6000002;

  // 2. Composition Firmware Entry がリンクされている場合はそれを起動
  if (&shizuku_firmware_entry != nullptr) {
    bool ok = shizuku_firmware_entry();
    if (!ok) {
      // ローダー失敗: 診断マーカーを記録して halt
      shizuku_c6_thread0_diag[3] = 0xDEAD0001;
      for (;;) { asm volatile("fence rw,rw" ::: "memory"); }
    }
    // 成功: 協調ディスパッチループへ
    for (;;) {
      shizuku_c6_thread0_diag[2] = shizuku_c6_thread0_diag[2] + 1;
      shizuku::ARCH::syscall(static_cast<uintptr_t>(shizuku::object_api::YIELD));
      asm volatile("fence rw,rw" ::: "memory");
    }
  }

  // 3. レガシー単体 selftest 経路 (shizuku_firmware_entry が無く、かつ runner がリンクされている場合のみ実行)
  if (&shizuku_c6_run_selftests != nullptr) {
    shizuku_c6_run_selftests();
  }

  // 4. 段2 thread1 スポーン (GRANT スイッチ試験用)
  shizuku::KERNEL::spawn_request request{};
  request.entry_pc = reinterpret_cast<uintptr_t>(&thread1);
  request.protection = shizuku::PROTECTION_PRIVILEGED;
  request.stack_base = reinterpret_cast<uintptr_t>(thread1_stack);
  request.stack_bytes = sizeof(thread1_stack);
  request.object_id = 2;
  request.parent_handler_object = 0; // KERNEL_OBJECT_ID
  request.parent_handler_entry = reinterpret_cast<uintptr_t>(&c6_combined_handler);
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
  auto &kernel = shizuku::kernel_instance;
  kernel.init();
  shizuku::kernel_object_instance.init();
  kernel.set_object_handler(reinterpret_cast<uintptr_t>(&c6_combined_handler), 0);
  const auto boot = shizuku::kernel_object_instance.lend_boot_stack();
  kernel.bootstrap(thread0, boot.base, boot.bytes, boot.ledger, boot.ledger_capacity);
  __builtin_unreachable();
}
