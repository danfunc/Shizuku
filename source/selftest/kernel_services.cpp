#include "shizuku/kernel_services.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/svc_attributes.hpp"
#include "shizuku/service_profile.hpp"
namespace shizuku::selftest {
namespace {
void check(const char *name, bool ok, uintptr_t got, uintptr_t want) {
  if (ok) ++passed; else { ++failed; record_fail(name,got,want); }
  KERNEL::BOARD::diag_printf("[SELFTEST] %s %s (%lu / %lu)\n",ok?"PASS":"FAIL",name,(unsigned long)got,(unsigned long)want);
}
volatile uint32_t isr_hits[2]{}, isr_bad[2]{};
uint32_t probe_enabled = 0;
}
void kernel_services_probe() {
  using namespace kernel_services;
  check("services Thread is not ISR",!in_isr(),in_isr(),0);
  volatile uint32_t word=7;
  check("services CAS mismatch leaves word",!cas(&word,8,9)&&word==7,word,7);
  check("services CAS match changes word",cas(&word,7,9)&&word==9,word,9);
  uint32_t before,masked,after_inner,after;
  asm volatile("mrs %0, primask":"=r"(before));
  auto outer=irq_save();auto inner=irq_save();
  asm volatile("mrs %0, primask":"=r"(masked));
  irq_restore(inner);asm volatile("mrs %0, primask":"=r"(after_inner));
  irq_restore(outer);asm volatile("mrs %0, primask":"=r"(after));
  check("services nested IRQ restore keeps outer mask",masked==1 && after_inner==1,after_inner,1);
  check("services IRQ restore preserves previous mask",after==before,after,before);
  const auto depth=kernel_instance.current_depth();
  const auto yielded=yield();
  check("services yield native gate returns",yielded.code==status::ok,(uintptr_t)yielded.code,0);
  check("services yield restores call depth",kernel_instance.current_depth()==depth,kernel_instance.current_depth(),depth);
  const auto t=time_us();const auto slept=sleep_us(10);
  check("services sleep returns and time is monotonic",slept.code==status::ok && time_us()>=t,(uintptr_t)slept.code,0);
  const auto denied=grant(0xffffffffu,1); // out-of-range thread id is rejected
  check("services invalid grant returns error",denied.code==status::operation_failed,(uintptr_t)denied.code,(uintptr_t)status::operation_failed);
  check("attributes GRANT denies unprivileged",!svc_permission_allows(23,false),svc_permission_allows(23,false),0);
  check("attributes forwarding requires ledger",!svc_permission_allows(27,true,false,false),svc_permission_allows(27,true,false,false),0);
  __atomic_store_n(&probe_enabled,1u,__ATOMIC_RELEASE);
}
void kernel_services_isr_result() {
#if SHIZUKU_KERNEL_SERVICE_PROFILE
  const auto core=KERNEL::BOARD::core_num();
  check("services ISR probe actually ran",isr_hits[core]>0,isr_hits[core],1);
  check("services ISR rejects scheduling",isr_bad[core]==0,isr_bad[core],0);
  const auto p=service_profile::read();
  KERNEL::BOARD::diag_printf("[V3GW] post-thread SVC max=%lu samples=%lu mask_envelope_max=%lu mask_samples=%lu unit=%s\n",
      (unsigned long)p.handler_max,(unsigned long)p.handler_samples,
      (unsigned long)p.mask_max,(unsigned long)p.mask_samples,
      service_profile::cycles()?"cyc":"us");
#endif
}
} // namespace shizuku::selftest
extern "C" void shizuku_service_isr_probe() {
#if SHIZUKU_KERNEL_SERVICE_PROFILE
  using namespace shizuku; using namespace selftest;
  if(!__atomic_load_n(&probe_enabled,__ATOMIC_ACQUIRE))return;
  const auto c=KERNEL::BOARD::core_num();
  volatile uint32_t word=1;
  const bool ok=kernel_services::in_isr() &&
      kernel_services::cas(&word,1,2) && word==2 &&
      kernel_services::yield().code==kernel_services::status::bad_context &&
      kernel_services::grant(0,1).code==kernel_services::status::bad_context &&
      kernel_services::sleep_us(1).code==kernel_services::status::bad_context;
  isr_hits[c]=isr_hits[c]+1;if(!ok)isr_bad[c]=isr_bad[c]+1;
#endif
}
