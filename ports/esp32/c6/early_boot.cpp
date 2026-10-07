// Stage-2 privileged bootstrap pilot. No IDF OS shim, heap or serial observer.
#include "shizuku/kernel.hpp"

extern "C" {
// entered, ECALL round-trip, live counter, observed error, observed value.
volatile uint32_t shizuku_c6_thread0_diag[5];
// entered marker, live counter, spawn error, spawned id, reserved.
volatile uint32_t shizuku_c6_thread1_diag[5];
// thread0 grant returns: [0]=EXPIRED count, [1]=YIELDED count, [2]=error count, [3]=last raw reply.
volatile uint32_t shizuku_c6_switch_diag[4];
}
namespace {
alignas(16) shizuku::KERNEL::thread_record records[2];
alignas(16) shizuku::KERNEL::call_ledger_entry ledger[4];
alignas(16) shizuku::KERNEL::call_ledger_entry ledger1[4];
alignas(16) unsigned char thread0_stack[4096];
alignas(16) unsigned char thread1_stack[4096];
constexpr uintptr_t GRANT_TICKS = 160000;  // 10 ms of 16 MHz SYSTIMER ticks
uintptr_t diagnostic_handler(uintptr_t, uintptr_t command, uintptr_t, uintptr_t) {
  if (command == 1) {
    // Runs as the kernel object, so GRANT is interpreted as a primitive.
    const auto reply = shizuku::ARCH::syscall(static_cast<uintptr_t>(shizuku::primitive::GRANT), 1, GRANT_TICKS);
    return 0xC6100000u | (static_cast<uint32_t>(reply.error) << 4) | static_cast<uint32_t>(reply.value & 0xf);
  }
  return 0xC6000002;
}
[[noreturn]] void thread1() {
  shizuku_c6_thread1_diag[0] = 0xC6100001;
  for (;;) {
    shizuku_c6_thread1_diag[1] = shizuku_c6_thread1_diag[1] + 1;
    asm volatile("fence rw,rw" ::: "memory");
  }
}
[[noreturn]] void thread0() {
  shizuku_c6_thread0_diag[0] = 0xC6000001;
  auto result = shizuku::ARCH::syscall(0xC6);
  shizuku_c6_thread0_diag[3] = result.error;
  shizuku_c6_thread0_diag[4] = result.value;
  if (result.error == 0 && result.value == 0xC6000002 && !shizuku::ARCH::in_interrupt())
    shizuku_c6_thread0_diag[1] = 0xC6000002;
  shizuku::KERNEL::spawn_request request{};
  request.entry_pc = reinterpret_cast<uintptr_t>(&thread1);
  request.protection = shizuku::PROTECTION_PRIVILEGED;
  request.stack_base = reinterpret_cast<uintptr_t>(thread1_stack);
  request.stack_bytes = sizeof(thread1_stack);
  request.object_id = 2;
  request.parent_handler_object = 1;
  request.parent_handler_entry = reinterpret_cast<uintptr_t>(&diagnostic_handler);
  request.ledger = ledger1;
  request.ledger_capacity = 4;
  const auto spawned = shizuku::kernel_instance.spawn(request);
  shizuku_c6_thread1_diag[2] = static_cast<uint32_t>(spawned.error);
  shizuku_c6_thread1_diag[3] = spawned.thread;
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
}
extern "C" [[noreturn]] void shizuku_c6_early_boot() {
  auto &kernel = shizuku::kernel_instance;
  kernel.init();
  kernel.set_thread_storage(records, sizeof(records));
  kernel.set_object_handler(reinterpret_cast<uintptr_t>(&diagnostic_handler), 1);
  kernel.bootstrap(thread0, reinterpret_cast<uintptr_t>(thread0_stack),
                   sizeof(thread0_stack), ledger, 4);
  __builtin_unreachable();
}
