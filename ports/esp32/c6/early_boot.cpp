// ESP32-C6 の pre-init 起動表示。
// この段階では KERNEL::init、kernel_instance、C++ ctor、libc、heap、RTOS に触れない。
#include <stdint.h>

extern "C" bool shizuku_c6_usb_console_write_and_flush(const char *text, unsigned length);
extern "C" void shizuku_c6_early_diag_writer_result(uint32_t result);

extern "C" [[noreturn]] void shizuku_c6_early_boot(void) {
  static constexpr char marker[] = "[SHIZUKU] boot on esp32c6\n";
  const bool writer_ok = shizuku_c6_usb_console_write_and_flush(marker, sizeof(marker) - 1);
  shizuku_c6_early_diag_writer_result(writer_ok ? 1U : 2U);

  // DIAGNOSTIC CANDIDATE: keep the CPU active after either writer result to isolate WFI effects.
  // This is intentionally a busy loop; it is not the production idle policy.
  for (;;) {
    __asm__ volatile("nop" ::: "memory");
  }
}
