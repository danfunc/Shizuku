#include "shizuku/service_profile.hpp"
#include "shizuku/kernel.hpp"
namespace shizuku::service_profile {
namespace {
using A = KERNEL::ARCH;
using B = KERNEL::BOARD;
struct state { maxima values; uint32_t started = 0; bool active = false, ready = false; };
state states[KERNEL::CORE_COUNT];
uint32_t mask_save() { uint32_t v; asm volatile("mrs %0, primask\ncpsid i":"=r"(v)::"memory");return v; }
void mask_restore(uint32_t v) { asm volatile("msr primask, %0"::"r"(v):"memory"); }
template<class Arch> bool init() {
  if constexpr (Arch::HAS_CYCLE_COUNTER) return Arch::cycle_counter_enable();
  return true;
}
template<class Arch> uint32_t clock() {
  if constexpr (Arch::HAS_CYCLE_COUNTER) return Arch::cycle_counter_read();
  else return static_cast<uint32_t>(B::time_us());
}
state &current() { return states[B::core_num()]; }
}
bool cycles() { return A::HAS_CYCLE_COUNTER; }
bool available() { return current().ready; }
void reset() {
#if SHIZUKU_KERNEL_SERVICE_PROFILE
  const auto irq = mask_save();
  auto &s = current(); s = {}; s.ready = init<A>();
  mask_restore(irq);
#endif
}
maxima read() { const auto irq = mask_save(); auto v=current().values;mask_restore(irq);return v; }
} // namespace shizuku::service_profile
extern "C" uint32_t shizuku_service_profile_now() {
#if SHIZUKU_KERNEL_SERVICE_PROFILE
  using namespace shizuku::service_profile;
  auto &s=current(); if (!s.ready) s.ready=init<A>();
  return s.ready ? clock<A>() : 0;
#else
  return 0;
#endif
}
extern "C" void shizuku_service_handler_begin() {
#if SHIZUKU_KERNEL_SERVICE_PROFILE
  using namespace shizuku::service_profile;
  auto &s=current(); const auto now=shizuku_service_profile_now();
  if (s.ready) { s.started=now; s.active=true; }
#endif
}
extern "C" void shizuku_service_handler_end() {
#if SHIZUKU_KERNEL_SERVICE_PROFILE
  using namespace shizuku::service_profile;
  const auto ended=shizuku_service_profile_now();const auto irq=mask_save();auto &s=current();
  if(s.ready && s.active) { s.values.handler(s.started,ended);s.active=false; }
  mask_restore(irq);
#endif
}
extern "C" void shizuku_service_mask_span(uint32_t started) {
#if SHIZUKU_KERNEL_SERVICE_PROFILE
  using namespace shizuku::service_profile;
  const auto ended=shizuku_service_profile_now();const auto irq=mask_save();auto &s=current();
  if(s.ready) s.values.mask(started,ended);
  mask_restore(irq);
#else
  (void)started;
#endif
}
