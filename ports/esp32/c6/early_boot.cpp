// Stage-2 privileged bootstrap pilot. No IDF OS shim, heap or serial observer.
#include "shizuku/kernel.hpp"

extern "C" {
// entered, ECALL round-trip, live counter, observed error, observed value.
volatile uint32_t shizuku_c6_thread0_diag[5];
}
namespace {
alignas(16) shizuku::KERNEL::thread_record records[1];
alignas(16) shizuku::KERNEL::call_ledger_entry ledger[4];
alignas(16) unsigned char thread0_stack[4096];
uintptr_t diagnostic_handler(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return 0xC6000002;
}
[[noreturn]] void thread0() {
  shizuku_c6_thread0_diag[0] = 0xC6000001;
  auto result = shizuku::ARCH::syscall(0xC6);
  shizuku_c6_thread0_diag[3] = result.error;
  shizuku_c6_thread0_diag[4] = result.value;
  if (result.error == 0 && result.value == 0xC6000002 && !shizuku::ARCH::in_interrupt())
    shizuku_c6_thread0_diag[1] = 0xC6000002;
  for (;;) {
    shizuku_c6_thread0_diag[2] = shizuku_c6_thread0_diag[2] + 1;
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
