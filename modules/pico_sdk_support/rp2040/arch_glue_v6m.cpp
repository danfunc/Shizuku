#if SHIZUKU_KERNEL_SERVICE_PROFILE
extern "C" void shizuku_service_isr_probe() __attribute__((weak));
#define SERVICE_ISR_PROBE() do { if(shizuku_service_isr_probe) shizuku_service_isr_probe(); } while(0)
#else
#define SERVICE_ISR_PROBE() ((void)0)
#endif
#include "arch/armv6m/armv6m.hpp"
#include "hardware/sync.h"
#if defined(SHIZUKU_CRASH_RECORD)
#include "hardware/structs/watchdog.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#endif
#include "chips/rp2040/chip.hpp"
#include "shizuku/kernel.hpp"
#if defined(SHIZUKU_CRASH_RECORD)
#include "shizuku/objects/usb_cdc.hpp"
#endif
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "pico/time.h"
#endif
// atomic 用ロック (arch/armv6m/armv6m.hpp の宣言の実体)。ID は SDK の
// PICO_SPINLOCK_ID_ATOMIC をそのまま使う (値を写さない)。
extern "C" uint32_t shizuku_armv6m_atomic_lock(uintptr_t address,
                                                 uintptr_t caller_pc){
#if defined(SHIZUKU_CRASH_RECORD)
  const uint32_t core = get_core_num();
  const uint32_t thread_id = shizuku::kernel_instance.current_thread_id();
  uint32_t ipsr;
  __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
  if (watchdog_hw->scratch[0] == 0x43524153u &&
      (watchdog_hw->scratch[7] & 0xf0000000u) != 0xf0000000u &&
      (watchdog_hw->scratch[7] & 0xf0000000u) != 0xe0000000u)
    watchdog_hw->scratch[7] = ((uint32_t)address & 0x7fffffffu) | (core << 31);
#if defined(SHIZUKU_BOOT_LOG_WAIT)
  shizuku::objects::usb_cdc_boot_diag_atomic_wait(
      core, address, caller_pc, thread_id, ipsr);
  shizuku::objects::usb_cdc_boot_diag_breadcrumb_at(
      shizuku::objects::boot_diag_breadcrumb::ATOMIC_LOCK_ENTRY, caller_pc);
#else
  (void)thread_id;
  (void)ipsr;
#endif
#endif
  const uint32_t saved_irq=spin_lock_blocking(spin_lock_instance(PICO_SPINLOCK_ID_ATOMIC));
#if defined(SHIZUKU_CRASH_RECORD)
  const uint32_t core_after = get_core_num();
  const uint32_t thread_id_after =
      shizuku::kernel_instance.current_thread_id();
  uint32_t ipsr_after;
  __asm volatile("mrs %0, ipsr" : "=r"(ipsr_after));
  uint32_t primask_after;
  __asm volatile("mrs %0, primask" : "=r"(primask_after));
#if defined(SHIZUKU_BOOT_LOG_WAIT)
  shizuku::objects::usb_cdc_boot_diag_atomic_acquired(
      core_after, address, caller_pc, thread_id_after, ipsr_after,
      primask_after);
  shizuku::objects::usb_cdc_boot_diag_breadcrumb_at(
      shizuku::objects::boot_diag_breadcrumb::ATOMIC_LOCK_EXIT, caller_pc);
#else
  (void)core_after;
  (void)thread_id_after;
  (void)ipsr_after;
  (void)primask_after;
  (void)address;
  (void)caller_pc;
#endif
#else
  (void)address;
  (void)caller_pc;
#endif
  return saved_irq;
}
extern "C" void shizuku_armv6m_atomic_unlock(uint32_t saved_irq,
                                              uintptr_t address,
                                              uintptr_t caller_pc){
#if defined(SHIZUKU_CRASH_RECORD)
  const uint32_t core = get_core_num();
  if (watchdog_hw->scratch[0] == 0x43524153u &&
      (watchdog_hw->scratch[7] & 0xf0000000u) != 0xf0000000u &&
      (watchdog_hw->scratch[7] & 0xf0000000u) != 0xe0000000u)
    watchdog_hw->scratch[7] = ((uint32_t)address & 0x7fffffffu) | (core << 31);
#if defined(SHIZUKU_BOOT_LOG_WAIT)
  shizuku::objects::usb_cdc_boot_diag_atomic_release_begin(core);
  shizuku::objects::usb_cdc_boot_diag_breadcrumb_at(
      shizuku::objects::boot_diag_breadcrumb::ATOMIC_UNLOCK_ENTRY, caller_pc);
#endif
#endif
  spin_unlock(spin_lock_instance(PICO_SPINLOCK_ID_ATOMIC),saved_irq);
#if defined(SHIZUKU_CRASH_RECORD)
#if defined(SHIZUKU_BOOT_LOG_WAIT)
  shizuku::objects::usb_cdc_boot_diag_atomic_released(get_core_num());
  shizuku::objects::usb_cdc_boot_diag_breadcrumb_at(
      shizuku::objects::boot_diag_breadcrumb::ATOMIC_UNLOCK_EXIT, caller_pc);
#else
  (void)address;
  (void)caller_pc;
#endif
#else
  (void)address;
  (void)caller_pc;
#endif
}
// SoC 既定 (chips/rp2040) と SDK の実値が一致することを固定する。SDK 側で上書きされたら止まる。
static_assert(PICO_SPINLOCK_ID_ATOMIC==shizuku::chips::rp2040::chip::kAtomicSpinlockId,"PICO_SPINLOCK_ID_ATOMIC differs from chips/rp2040 kAtomicSpinlockId");
extern "C" shizuku_armv6m_context *shizuku_current_context(){
  return reinterpret_cast<shizuku_armv6m_context *>(shizuku::kernel_instance.current_context());
}
#if defined(SHIZUKU_CRASH_RECORD)
extern "C" volatile uint32_t shizuku_boot_diag_stage_id;
extern "C" void __not_in_flash_func(shizuku_crash_record_fault)(
    const uint32_t *frame, uint32_t exc_return) {
  const uint32_t core = get_core_num();
  watchdog_hw->scratch[1] = shizuku_boot_diag_stage_id | (core << 8);
  watchdog_hw->scratch[4] = frame[6]; // stacked PC
  watchdog_hw->scratch[5] = frame[5]; // stacked LR
  watchdog_hw->scratch[6] = frame[7]; // stacked xPSR
  watchdog_hw->scratch[7] = exc_return;
  watchdog_hw->scratch[0] = 0x48465254u; // "HFRT"; watchdog resets after 3 s
}
#endif
extern "C" void shizuku_svc_dispatch(shizuku_armv6m_context *c){
#if defined(SHIZUKU_CRASH_RECORD)
  shizuku::objects::usb_cdc_boot_diag_breadcrumb(
      shizuku::objects::boot_diag_breadcrumb::SVC_DISPATCH_ENTRY);
#endif
  shizuku::kernel_instance.svc_dispatch(reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c));
#if defined(SHIZUKU_CRASH_RECORD)
  shizuku::objects::usb_cdc_boot_diag_breadcrumb(
      shizuku::objects::boot_diag_breadcrumb::SVC_DISPATCH_EXIT);
#endif
}
extern "C" void shizuku_pendsv_dispatch(shizuku_armv6m_context *c){
#if defined(SHIZUKU_CRASH_RECORD)
  shizuku::objects::usb_cdc_boot_diag_breadcrumb(
      shizuku::objects::boot_diag_breadcrumb::PENDSV_DISPATCH_ENTRY);
#endif
  shizuku::kernel_instance.pendsv_dispatch(reinterpret_cast<shizuku::KERNEL::CONTEXT *>(c));
#if defined(SHIZUKU_CRASH_RECORD)
  shizuku::objects::usb_cdc_boot_diag_breadcrumb(
      shizuku::objects::boot_diag_breadcrumb::PENDSV_DISPATCH_EXIT);
#endif
}
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
  using A=shizuku::archs::armv6m<shizuku::chips::rp2040::chip::kMpuRegionCount,shizuku::chips::rp2040::chip::kNvicPriorityBits>;const auto*c=shizuku::kernel_instance.current_context();
  if(c->region_limit && A::region_try_set(A::GRANT_REGION_INDEX,c->region_base,c->region_limit,A::ACCESS_RO_ALL,true))return;
  A::region_disable(A::GRANT_REGION_INDEX);
}
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
// frame は armv6m_ctx.S の systick_entry が渡す、割り込まれた側の例外フレーム。
//  ★feed は割込み可で回っている間だけ続く。割込み禁止の停止では SysTick も死ぬので止まる。
//  ★割込み可の空回りは feed が続くので、610..615/700..708 の段階が 3 秒動かなければ
//    標本 (PC) を残して feed を止め、watchdog に落とす。
extern "C" void shizuku_armv6m_systick_dispatch_frame(const uint32_t *frame){
#if defined(SHIZUKU_CRASH_RECORD)
  shizuku::objects::usb_cdc_boot_diag_systick_tick(get_core_num());
#endif
  static uint32_t last_stage=0,since_us=0;
  const uint32_t raw_stage=watchdog_hw->scratch[1];
  const uint32_t stage=watchdog_hw->scratch[0] == 0x43524153u
      ? (raw_stage & 0xffu) : raw_stage;
  const uint32_t now=time_us_32();
  bool feed=true;
  if(stage!=last_stage){last_stage=stage;since_us=now;}
  else if((stage>=610&&stage<=615)||(stage>=700&&stage<=708)){
    shizuku_selftest_tick_sample(frame);
    if(now-since_us>3000000u)feed=false;
  }
  if(feed)watchdog_update();
  SERVICE_ISR_PROBE(); shizuku::kernel_instance.timer_expired();
}
#elif defined(SHIZUKU_CRASH_RECORD)
// RP2040 uses SysTick here as a one-shot grant timer, not a periodic heartbeat.
// Remember whether it happened to fire after crash capture was armed; a zero is
// not evidence by itself that interrupts were masked. The IRQ frame is in
// scratch[4]/[6], and scratch[5] holds core 1's last breadcrumb caller.
extern "C" void shizuku_armv6m_systick_dispatch_frame(const uint32_t *frame) {
  (void)frame;
  shizuku::objects::usb_cdc_boot_diag_systick_tick(get_core_num());
  SERVICE_ISR_PROBE();
  shizuku::kernel_instance.timer_expired();
}
#else
extern "C" void shizuku_armv6m_systick_dispatch(){SERVICE_ISR_PROBE(); shizuku::kernel_instance.timer_expired();}
#endif
extern "C" void shizuku_armv6m_systick_entry();
