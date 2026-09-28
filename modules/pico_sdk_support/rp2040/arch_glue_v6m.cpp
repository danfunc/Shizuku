#include "shizuku/archs/armv6m.hpp"
#include "shizuku/kernel.hpp"
extern "C" shizuku_armv6m_context *shizuku_current_context(){
  return reinterpret_cast<shizuku_armv6m_context *>(shizuku::kernel_instance.current_context());
}
extern "C" void shizuku_svc_dispatch(shizuku_armv6m_context *c){shizuku::kernel_instance.svc_dispatch(reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c));}
extern "C" void shizuku_pendsv_dispatch(shizuku_armv6m_context *c){shizuku::kernel_instance.pendsv_dispatch(reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c));}
extern "C" void shizuku_debug_dispatch(shizuku_armv6m_context *c){shizuku::kernel_instance.debug_dispatch(reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c));}
extern "C" void shizuku_fault_dispatch(shizuku_armv6m_context *c){shizuku::kernel_instance.fault_dispatch(reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c));}
extern "C" void shizuku_arm_pending_step(){}
extern "C" void shizuku_restore_region_window(){
  using A=shizuku::archs::armv6m;const auto*c=shizuku::kernel_instance.current_context();
  if(c->region_limit && A::region_try_set(A::GRANT_REGION_INDEX,c->region_base,c->region_limit,A::ACCESS_RO_ALL,true))return;
  A::region_disable(A::GRANT_REGION_INDEX);
}
extern "C" void shizuku_armv6m_systick_dispatch(){shizuku::kernel_instance.timer_expired();}
extern "C" void shizuku_armv6m_systick_entry();
