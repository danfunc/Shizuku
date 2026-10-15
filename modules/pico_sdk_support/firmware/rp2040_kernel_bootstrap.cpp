// SDK-independent RP2040 kernel bootstrap. This source belongs in the Bazel
// kernel archive; Pico SDK board/arch implementations remain CMake-side.
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"

namespace {
uintptr_t rp2040_kernel_object_handler(uintptr_t number, uintptr_t arg1,
                                      uintptr_t arg2, uintptr_t arg3,
                                      uintptr_t unwind_floor) {
  return shizuku::kernel_object_instance.handle(number, arg1, arg2, arg3,
                                                 unwind_floor);
}
} // namespace

extern "C" [[noreturn]] void shizuku_rp2040_kernel_bootstrap_root_thread(
    void (*root_thread_fn)(void)) {
  if (root_thread_fn == nullptr) {
    shizuku::KERNEL::BOARD::panic("RP2040 root thread is null");
  }

  auto &kernel = shizuku::kernel_instance;
  kernel.init();
  shizuku::kernel_object_instance.init();
  kernel.set_object_handler(
      reinterpret_cast<uintptr_t>(&rp2040_kernel_object_handler), 0);
  const auto boot = shizuku::kernel_object_instance.lend_boot_stack();
  kernel.bootstrap(root_thread_fn, boot.base, boot.bytes, boot.ledger,
                   boot.ledger_capacity);
  __builtin_unreachable();
}
