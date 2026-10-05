#include "shizuku/archs/armv6m.hpp"
#include "shizuku/kernel.hpp"
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "pico/time.h"
#endif
extern "C" shizuku_armv6m_context *shizuku_current_context(){
  return reinterpret_cast<shizuku_armv6m_context *>(shizuku::kernel_instance.current_context());
}
extern "C" void shizuku_svc_dispatch(shizuku_armv6m_context *c){shizuku::kernel_instance.svc_dispatch(reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c));}
extern "C" void shizuku_pendsv_dispatch(shizuku_armv6m_context *c){shizuku::kernel_instance.pendsv_dispatch(reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c));}
extern "C" void shizuku_debug_dispatch(shizuku_armv6m_context *c){shizuku::kernel_instance.debug_dispatch(reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c));}
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
extern "C" void shizuku_selftest_fault_enter(const uint32_t *, uint32_t, uint32_t);
extern "C" void shizuku_selftest_fault_leave();
extern "C" void shizuku_selftest_tick_sample(const uint32_t *);
extern "C" void shizuku_fault_dispatch(shizuku_armv6m_context *c){
  auto *k=reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c);
  shizuku_selftest_fault_enter(reinterpret_cast<const uint32_t *>(k->sp),k->exc_return,k->control);
  shizuku::kernel_instance.fault_dispatch(k);
  shizuku_selftest_fault_leave();
}
#else
extern "C" void shizuku_fault_dispatch(shizuku_armv6m_context *c){shizuku::kernel_instance.fault_dispatch(reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c));}
#endif
extern "C" void shizuku_arm_pending_step(){}
extern "C" void shizuku_restore_region_window(){
  using A=shizuku::archs::armv6m;const auto*c=shizuku::kernel_instance.current_context();
  if(c->region_limit && A::region_try_set(A::GRANT_REGION_INDEX,c->region_base,c->region_limit,A::ACCESS_RO_ALL,true))return;
  A::region_disable(A::GRANT_REGION_INDEX);
}
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
// frame は armv6m_ctx.S の systick_entry が渡す、割り込まれた側の例外フレーム。
//  ★feed は割込み可で回っている間だけ続く。割込み禁止の停止では SysTick も死ぬので止まる。
//  ★割込み可の空回りは feed が続くので、610..615/700..708 の段階が 3 秒動かなければ
//    標本 (PC) を残して feed を止め、watchdog に落とす。
extern "C" void shizuku_armv6m_systick_dispatch_frame(const uint32_t *frame){
  static uint32_t last_stage=0,since_us=0;
  const uint32_t stage=watchdog_hw->scratch[1];
  const uint32_t now=time_us_32();
  bool feed=true;
  if(stage!=last_stage){last_stage=stage;since_us=now;}
  else if((stage>=610&&stage<=615)||(stage>=700&&stage<=708)){
    shizuku_selftest_tick_sample(frame);
    if(now-since_us>3000000u)feed=false;
  }
  if(feed)watchdog_update();
  shizuku::kernel_instance.timer_expired();
}
#else
extern "C" void shizuku_armv6m_systick_dispatch(){shizuku::kernel_instance.timer_expired();}
#endif
extern "C" void shizuku_armv6m_systick_entry();
