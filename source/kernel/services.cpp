#include "shizuku/kernel_services.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/service_profile.hpp"

#if defined(__arm__)
namespace shizuku::kernel_services {
using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;
bool in_isr() {
#if defined(__riscv)
  return shizuku_c6_in_isr();
#else
  uint32_t ipsr;
  __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
  return ipsr != 0;
#endif
}
// One protected resume PC identifies the internal V2 scheduling gate. Public
// raw SVC numbers, including those of a custom parent, remain uninterpreted.
[[gnu::noinline, gnu::noclone]] static result invoke(object_api api, uintptr_t a1 = 0,
                                                    uintptr_t a2 = 0) {
  if (in_isr()) return {status::bad_context, 0, 0};
  if (!ARCH::current_priv()) return {status::not_privileged, 0, 0};
  register uintptr_t r0 asm("r0") = static_cast<uintptr_t>(api);
  register uintptr_t r1 asm("r1") = a1;
  register uintptr_t r2 asm("r2") = a2;
  register uintptr_t r3 asm("r3") = 0;
  register uintptr_t r12 asm("r12") = 0;
  __asm__ volatile("svc 0\n.global shizuku_v2_service_resume\nshizuku_v2_service_resume:"
                   : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3), "r"(r12) : "memory");
  return {r0 == 0 ? status::ok : status::operation_failed, r1, r0};
}
result yield() { return invoke(object_api::YIELD); }
result grant(uint32_t target, uint32_t cycles) { return invoke(object_api::RUN_FOR, target, cycles); }
result sleep_us(uintptr_t us) { return invoke(object_api::SLEEP_US, us); }
uint64_t time_us() { return BOARD::time_us(); }
bool cas(volatile uint32_t *p, uint32_t expected, uint32_t desired) {
  if (!in_isr() && !ARCH::current_priv()) return false;
  return ARCH::cas32(p, expected, desired);
}
irq_state irq_save() {
  uint32_t started = 0;
#if SHIZUKU_KERNEL_SERVICE_PROFILE
  started = shizuku_service_profile_now();
#endif
  uint32_t mask;
  __asm__ volatile("mrs %0, primask\ncpsid i" : "=r"(mask) :: "memory");
  return {mask, started};
}
void irq_restore(irq_state state) {
  __asm__ volatile("msr primask, %0" :: "r"(state.primask) : "memory");
#if SHIZUKU_KERNEL_SERVICE_PROFILE
  // Clock reads bracket the full interval; an IRQ after unmask can inflate it.
  if ((state.primask & 1u) == 0) shizuku_service_mask_span(state.started);
#endif
}
} // namespace shizuku::kernel_services

#endif // ARM V2 backend; C6 definitions belong to its port
