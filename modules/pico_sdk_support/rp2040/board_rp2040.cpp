#include "core_notify.hpp"
#include <cstdarg>
#include <cstdio>
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/exception.h"
#include "hardware/irq.h"
#include "hardware/structs/systick.h"
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#endif
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/stdlib.h"
#include "shizuku/archs/armv6m.hpp"
#include "shizuku/boards/rp2040_pico_w.hpp"
#include "shizuku/kernel.hpp"
#include "tusb.h"
extern "C" char __end__[];
extern "C" char __flash_binary_end;
extern "C" char __data_start__[];
extern "C" char __data_end__[];
namespace shizuku::boards {
void rp2040_pico_w::notify_core(uint32_t core) { ports::core_notify(core); }

void rp2040_pico_w::init(uint32_t core){
  ports::core_notify_init(core, [] { shizuku::archs::armv6m::pend_context_switch(); });

  if(core==0){
    exception_set_exclusive_handler(SVCALL_EXCEPTION,shizuku_armv6m_svc_entry);
    exception_set_exclusive_handler(PENDSV_EXCEPTION,shizuku_armv6m_pendsv_entry);
    exception_set_exclusive_handler(SYSTICK_EXCEPTION,shizuku_armv6m_systick_entry);
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION,shizuku_armv6m_fault_entry);
  }
  multicore_lockout_victim_init();
  // RP2040 implements two configurable priority bits: four levels only.
  // Preserve syscall > SysTick > PendSV. USB is below SysTick; HardFault has
  // fixed priority -1 on ARMv6-M and cannot be placed below USB.
  exception_set_priority(SVCALL_EXCEPTION,0x00);
  exception_set_priority(SYSTICK_EXCEPTION,0x40);
  exception_set_priority(PENDSV_EXCEPTION,0xC0);
  irq_set_priority(USBCTRL_IRQ,0x80);
  protection_init();
}
void rp2040_pico_w::protection_init(){
  using A=shizuku::archs::armv6m;
  constexpr uint32_t RAM_EXEC_REGION=3;
  static_assert(RAM_EXEC_REGION>1 && RAM_EXEC_REGION!=A::GRANT_REGION_INDEX && RAM_EXEC_REGION<8);
  const uintptr_t end=((uintptr_t)&__flash_binary_end+31u)&~(uintptr_t)31u;
  // PMSAv6 rounds out to a power-of-two region. Any surrounding flash can
  // become readable/executable; the exact excess depends on linked image size.
  if(!A::region_try_set(0,0x10000000u,end,A::ACCESS_RO_ALL,false))
    panic("RP2040 code range cannot be represented by PMSAv6 MPU");
  const uintptr_t heap=((uintptr_t)__end__+31u)&~(uintptr_t)31u;
  // Likewise SRAM padding around the region is included; stack guard work is Phase 2.
  if(!A::region_try_set(1,heap,0x20041FE0u,A::ACCESS_RW_ALL,true))
    panic("RP2040 heap range cannot be represented by PMSAv6 MPU");
  for(uint32_t i=2;i<8;i++)A::region_disable(i);
  const uintptr_t data_start=(uintptr_t)__data_start__;
  const uintptr_t data_end=(uintptr_t)__data_end__;
  if(data_start<0x20000000u || data_end<=data_start || data_end>0x20040000u)
    panic("RP2040 RAM code bounds are invalid");
  uint32_t ram_rbar,ram_rasr;
  if(!A::region_encode(data_start,data_end,ram_rbar,ram_rasr,A::ACCESS_RW_ALL,false))
    panic("RP2040 RAM code range cannot be represented by PMSAv6 MPU");
  const uintptr_t ram_region_base=ram_rbar;
  const uint32_t ram_region_log=((ram_rasr>>1)&0x1fu)+1u;
  const uintptr_t ram_region_size=(uintptr_t)1u<<ram_region_log;
  const uintptr_t ram_subregion_size=ram_region_size/8u;
  const uint32_t ram_subregion_disable=(ram_rasr>>8)&0xffu;
  auto region_covers=[&](uintptr_t address){
    if(address<ram_region_base || address>=ram_region_base+ram_region_size)return false;
    if(ram_region_size>=2048u){
      const uint32_t subregion=(uint32_t)((address-ram_region_base)/ram_subregion_size);
      if(ram_subregion_disable&(1u<<subregion))return false;
    }
    return true;
  };
  if(!region_covers(data_start) || !region_covers(data_end-1u) ||
     !A::region_try_set(RAM_EXEC_REGION,data_start,data_end,A::ACCESS_RW_ALL,false))
    panic("RP2040 RAM code overlay does not cover linker data");
  A::protection_enable();
}
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
void rp2040_pico_w::boot_trace_stage(uint32_t stage){
  watchdog_hw->scratch[0]=0x53480000u;
  watchdog_hw->scratch[1]=stage&0xffffu;
  watchdog_hw->scratch[2]=0;
  watchdog_hw->scratch[3]=0;
  watchdog_hw->scratch[5]=0;
  watchdog_hw->scratch[6]=0;
  watchdog_hw->scratch[7]=0;
  watchdog_update();
}
#endif
static bool g_parked = false;

void rp2040_pico_w::launch_core(void(*entry)()){multicore_launch_core1(entry);}
void rp2040_pico_w::park_other_cores(){
  const uint32_t other = get_core_num() == 0 ? 1u : 0u;
  if (!multicore_lockout_victim_is_initialized(other))
    return;
  multicore_lockout_start_blocking();
  g_parked = true;
}
void rp2040_pico_w::resume_other_cores(){
  if (!g_parked)
    return;
  g_parked = false;
  multicore_lockout_end_blocking();
}
void rp2040_pico_w::diag_mute(bool){}
int rp2040_pico_w::dma_claim(){return dma_claim_unused_channel(false);}
void rp2040_pico_w::dma_copy(int ch,const void*src,void*dst,uint32_t n){
  dma_channel_config cfg=dma_channel_get_default_config((uint)ch);
  channel_config_set_transfer_data_size(&cfg,DMA_SIZE_8);
  channel_config_set_read_increment(&cfg,true);channel_config_set_write_increment(&cfg,true);
  dma_channel_configure((uint)ch,&cfg,dst,src,n,true);
  dma_channel_wait_for_finish_blocking((uint)ch);
}
bool rp2040_pico_w::dma_busy(int ch){return dma_channel_is_busy((uint)ch);}
void rp2040_pico_w::dma_release(int ch){dma_channel_unclaim((uint)ch);}
uint32_t rp2040_pico_w::cycles_per_us(){return clock_get_hz(clk_sys)/1000000u;}
uintptr_t rp2040_pico_w::unprivileged_floor(){return ((uintptr_t)__end__+31u)&~(uintptr_t)31u;}
void rp2040_pico_w::diag_printf(const char*f,...){va_list a;va_start(a,f);vprintf(f,a);va_end(a);}
[[noreturn]] void rp2040_pico_w::panic(const char*m){
  printf("PANIC: %s\n",m);save_and_disable_interrupts();for(;;)__wfi();}
}
// Referenced by PICO_PANIC_FUNCTION from pico_config_extra_headers.h.
extern "C" [[noreturn]] void shizuku_panic_dump(const char *format,...){
  va_list args;va_start(args,format);vprintf(format,args);va_end(args);
  save_and_disable_interrupts();for(;;)__wfi();
}
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
namespace {
constexpr uint32_t BOOT_FAULT_REGS_MAGIC=0x53465231u;
struct boot_fault_registers_t {
  uint32_t magic;
  uint32_t r0,r1,r2,r3,r12;
  uint32_t stack_base,stack_top;
};
volatile boot_fault_registers_t __uninitialized_ram(g_boot_fault_registers);
}
extern "C" void shizuku_boot_trace_stack(uint32_t base,uint32_t top){
  g_boot_fault_registers.magic=0;
  g_boot_fault_registers.stack_base=base;
  g_boot_fault_registers.stack_top=top;
}
extern "C" void shizuku_boot_trace_phase(uint32_t stage){
  watchdog_hw->scratch[0]=0x53480000u;
  watchdog_hw->scratch[1]=stage&0xffffu;
  watchdog_hw->scratch[5]=0;
  watchdog_hw->scratch[6]=0;
  watchdog_hw->scratch[7]=0;
}
extern "C" void shizuku_boot_hardfault_snapshot(const uint32_t *frame,
                                                   uint32_t exc_return){
  if(!frame)return;
  g_boot_fault_registers.magic=0;
  g_boot_fault_registers.r0=frame[0];
  g_boot_fault_registers.r1=frame[1];
  g_boot_fault_registers.r2=frame[2];
  g_boot_fault_registers.r3=frame[3];
  g_boot_fault_registers.r12=frame[4];
  g_boot_fault_registers.magic=BOOT_FAULT_REGS_MAGIC;
  const uint32_t stage=watchdog_hw->scratch[1]&0xffffu;
  const uint32_t cause=0x80u|((exc_return&8u)?2u:0u)|((exc_return&4u)?1u:0u);
  watchdog_hw->scratch[0]=0x53480000u;
  watchdog_hw->scratch[1]=stage|(cause<<16);
  watchdog_hw->scratch[2]=frame[6];
  watchdog_hw->scratch[3]=frame[5];
  watchdog_hw->scratch[5]=frame[7];
  uint32_t primask;
  __asm volatile("mrs %0, primask" : "=r"(primask));
  // Stacked xPSR holds the interrupted IPSR; MRS IPSR here would report
  // HardFault itself (exception 3), which is not the interrupted context.
  watchdog_hw->scratch[6]=frame[7]&0x1ffu;
  watchdog_hw->scratch[7]=primask;
  watchdog_reboot(0,0,1);
  for(;;)__wfi();
}
extern "C" bool shizuku_boot_fault_registers(uint32_t *out){
  if(!out)return false;
  const bool valid=g_boot_fault_registers.magic==BOOT_FAULT_REGS_MAGIC;
  if(valid){
    out[0]=g_boot_fault_registers.r0;
    out[1]=g_boot_fault_registers.r1;
    out[2]=g_boot_fault_registers.r2;
    out[3]=g_boot_fault_registers.r3;
    out[4]=g_boot_fault_registers.r12;
    out[5]=g_boot_fault_registers.stack_base;
    out[6]=g_boot_fault_registers.stack_top;
  }
  g_boot_fault_registers.magic=0;
  return valid;
}
#endif
