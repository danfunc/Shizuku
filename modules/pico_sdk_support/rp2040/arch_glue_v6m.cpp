#include "shizuku/archs/armv6m.hpp"
#include "shizuku/kernel.hpp"
extern "C" void shizuku_arm_pending_step(){}
extern "C" void shizuku_restore_region_window(){
  using A=shizuku::archs::armv6m;const auto*c=shizuku::kernel_instance.current_context();
  if(c->region_limit && A::region_try_set(A::GRANT_REGION_INDEX,c->region_base,c->region_limit,A::ACCESS_RO_ALL,true))return;
  A::region_disable(A::GRANT_REGION_INDEX);
}
