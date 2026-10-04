#include "shizuku/kernel.hpp"

struct shizuku_context;

extern "C" {

shizuku_context *shizuku_current_context() {
  return reinterpret_cast<shizuku_context *>(
      shizuku::kernel_instance.current_context());
}

void shizuku_svc_dispatch(shizuku_context *context) {
  shizuku::kernel_instance.svc_dispatch(
      reinterpret_cast<shizuku::KERNEL::CONTEXT *>(context));
}

void shizuku_pendsv_dispatch(shizuku_context *context) {
  shizuku::kernel_instance.pendsv_dispatch(
      reinterpret_cast<shizuku::KERNEL::CONTEXT *>(context));
}

void shizuku_debug_dispatch(shizuku_context *context) {
  shizuku::kernel_instance.debug_dispatch(
      reinterpret_cast<shizuku::KERNEL::CONTEXT *>(context));
}

void shizuku_fault_dispatch(shizuku_context *context) {
  shizuku::kernel_instance.fault_dispatch(
      reinterpret_cast<shizuku::KERNEL::CONTEXT *>(context));
}

void shizuku_armv6m_systick_dispatch() {
  shizuku::kernel_instance.timer_expired();
}

void shizuku_armv8m_systick_entry() {
  shizuku::kernel_instance.timer_expired();
}

} // extern "C"
