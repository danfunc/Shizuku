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
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
#include "hardware/structs/watchdog.h"
#include "pico/bootrom.h"
#include "shizuku/objects/usb_cdc.hpp"
// 停止箇所の最後の進捗を、watchdog リセットをまたいで読めるように scratch レジスタへ
// 焼く (SRAM ではなく、USB/割込みが死んでいても書ける独立したレジスタ)。
extern "C" void shizuku_selftest_progress_mark(uint32_t stage, uint32_t a, uint32_t b) {
  watchdog_hw->scratch[0] = 0x53505247u; // "SPRG"
  watchdog_hw->scratch[1] = stage;
  watchdog_hw->scratch[2] = a;
  watchdog_hw->scratch[3] = b;
}
namespace {
// fault と SysTick 標本の記録。watchdog リセットをまたいで残す (scratch は満杯)。
struct selftest_fault_t {
  uint32_t magic, phase, count;
  uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
  uint32_t exc_return, control, ipsr;
  uint32_t tick_pc, tick_lr, tick_xpsr, tick_hits, tick_stage, arg;
};
volatile selftest_fault_t __uninitialized_ram(g_st_fault);
constexpr uint32_t kStFaultMagic = 0x53544654u; // "STFT"
void selftest_progress_panic_pump(uint32_t ms) {
  fflush(stdout);
  shizuku::objects::usb_cdc_isolate_for_panic();
  __asm volatile("cpsie i" : : : "memory");
  const uint64_t start = time_us_64();
  const uint64_t dur = (uint64_t)ms * 1000ull;
  while (time_us_64() - start < dur) {
    tud_task();
  }
  printf("[SELFTEST_PROG] panic pump complete, resetting to BOOTSEL\n");
  fflush(stdout);
  sleep_ms(100);
  reset_usb_boot(0, 0);
}
} // namespace
extern "C" void shizuku_selftest_fault_enter(const uint32_t *frame, uint32_t exc_return, uint32_t control) {
  if (g_st_fault.magic != kStFaultMagic) {
    g_st_fault.magic = kStFaultMagic;
    g_st_fault.count = 0;
    g_st_fault.tick_hits = 0;
  }
  uint32_t ipsr;
  __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
  g_st_fault.phase = 1;
  g_st_fault.count = g_st_fault.count + 1u;
  g_st_fault.r0 = frame[0]; g_st_fault.r1 = frame[1]; g_st_fault.r2 = frame[2];
  g_st_fault.r3 = frame[3]; g_st_fault.r12 = frame[4]; g_st_fault.lr = frame[5];
  g_st_fault.pc = frame[6]; g_st_fault.xpsr = frame[7];
  g_st_fault.exc_return = exc_return; g_st_fault.control = control; g_st_fault.ipsr = ipsr;
}
extern "C" void shizuku_selftest_fault_leave() { g_st_fault.phase = 2; }
extern "C" void shizuku_selftest_fault_phase(uint32_t phase, uint32_t arg) {
  g_st_fault.phase = phase;
  g_st_fault.arg = arg;
}
extern "C" void shizuku_selftest_tick_sample(const uint32_t *frame) {
  if (g_st_fault.magic != kStFaultMagic) {
    g_st_fault.magic = kStFaultMagic;
    g_st_fault.count = 0;
    g_st_fault.phase = 0;
    g_st_fault.tick_hits = 0;
  }
  g_st_fault.tick_pc = frame[6];
  g_st_fault.tick_lr = frame[5];
  g_st_fault.tick_xpsr = frame[7];
  g_st_fault.tick_stage = watchdog_hw->scratch[1];
  g_st_fault.tick_hits = g_st_fault.tick_hits + 1u;
}
extern "C" bool shizuku_selftest_fault_read(uint32_t *out) {
  if (g_st_fault.magic != kStFaultMagic) return false;
  out[0] = g_st_fault.phase; out[1] = g_st_fault.count;
  out[2] = g_st_fault.pc; out[3] = g_st_fault.lr; out[4] = g_st_fault.xpsr;
  out[5] = g_st_fault.r0; out[6] = g_st_fault.r1; out[7] = g_st_fault.r2; out[8] = g_st_fault.r3;
  out[9] = g_st_fault.exc_return; out[10] = g_st_fault.control; out[11] = g_st_fault.ipsr;
  out[12] = g_st_fault.tick_pc; out[13] = g_st_fault.tick_lr; out[14] = g_st_fault.tick_xpsr;
  out[15] = g_st_fault.tick_hits; out[16] = g_st_fault.tick_stage; out[17] = g_st_fault.arg;
  g_st_fault.magic = 0;
  return true;
}
#endif
namespace shizuku::boards {
void rp2040_pico_w::init(uint32_t core){
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
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_progress_mark(720, other, multicore_lockout_victim_is_initialized(other));
#endif
  if (!multicore_lockout_victim_is_initialized(other))
    return;
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_progress_mark(721, other, 0);
#endif
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
  printf("PANIC: %s\n",m);
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  selftest_progress_panic_pump(3000);
#endif
  save_and_disable_interrupts();for(;;)__wfi();}
}
// Referenced by PICO_PANIC_FUNCTION from pico_config_extra_headers.h.
extern "C" [[noreturn]] void shizuku_panic_dump(const char *format,...){
  va_list args;va_start(args,format);vprintf(format,args);va_end(args);
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  selftest_progress_panic_pump(3000);
#endif
  save_and_disable_interrupts();for(;;)__wfi();
}
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
namespace {
constexpr uint32_t BOOT_FAULT_REGS_MAGIC=0x53465231u;
constexpr uint32_t BOOT_DIAG_MAGIC=0x53484449u; // "SHDI"
constexpr uintptr_t ADDR_POOLS=0x2000c97cu;
constexpr uintptr_t ADDR_DEFAULT_ALARM_POOL=0x20003db0u;

struct boot_fault_registers_t {
  uint32_t magic;
  uint32_t r0,r1,r2,r3,r12;
  uint32_t stack_base,stack_top;
};
volatile boot_fault_registers_t __uninitialized_ram(g_boot_fault_registers);

struct diag_ring_entry_t {
  uint32_t thread_id;
  uint32_t depth;
  uint32_t current_object;
  uint32_t current_kind;
  uint32_t current_handler_object;
  uint32_t callee_frame;
};

struct boot_diagnostic_data_t {
  uint32_t magic;
  uint32_t min_sp;
  uint32_t last_callee_frame;
  uint32_t near_bottom_hits;
  uint32_t last_primitive;
  uint32_t last_sp_before;
  uint32_t last_sp_after;
  uint32_t pools_raw[4];
  uint32_t default_pool_raw[6];
  uint32_t max_depth;
  uint32_t max_depth_sp;
  uint32_t ring_head;
  uint32_t ring_count;
  diag_ring_entry_t ring[24];
};
volatile boot_diagnostic_data_t __uninitialized_ram(g_boot_diag_data);
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
extern "C" void shizuku_boot_trace_push_context(uint32_t callee_frame,
                                                uint32_t thread_id,
                                                uint32_t depth,
                                                uint32_t obj,
                                                uint32_t kind,
                                                uint32_t hobj){
  if(g_boot_diag_data.magic != BOOT_DIAG_MAGIC){
    g_boot_diag_data.magic = BOOT_DIAG_MAGIC;
    g_boot_diag_data.min_sp = callee_frame;
    g_boot_diag_data.last_callee_frame = callee_frame;
    g_boot_diag_data.near_bottom_hits = 0;
    g_boot_diag_data.last_primitive = 0;
    g_boot_diag_data.last_sp_before = 0;
    g_boot_diag_data.last_sp_after = 0;
    g_boot_diag_data.max_depth = depth;
    g_boot_diag_data.max_depth_sp = callee_frame;
    g_boot_diag_data.ring_head = 0;
    g_boot_diag_data.ring_count = 0;
    for(int i=0; i<24; ++i){
      g_boot_diag_data.ring[i].thread_id = 0;
      g_boot_diag_data.ring[i].depth = 0;
      g_boot_diag_data.ring[i].current_object = 0;
      g_boot_diag_data.ring[i].current_kind = 0;
      g_boot_diag_data.ring[i].current_handler_object = 0;
      g_boot_diag_data.ring[i].callee_frame = 0;
    }
  } else {
    if(callee_frame < g_boot_diag_data.min_sp)
      g_boot_diag_data.min_sp = callee_frame;
    g_boot_diag_data.last_callee_frame = callee_frame;
    if(depth > g_boot_diag_data.max_depth){
      g_boot_diag_data.max_depth = depth;
      g_boot_diag_data.max_depth_sp = callee_frame;
    }
  }

  uint32_t idx = g_boot_diag_data.ring_head;
  g_boot_diag_data.ring[idx].thread_id = thread_id;
  g_boot_diag_data.ring[idx].depth = depth;
  g_boot_diag_data.ring[idx].current_object = obj;
  g_boot_diag_data.ring[idx].current_kind = kind;
  g_boot_diag_data.ring[idx].current_handler_object = hobj;
  g_boot_diag_data.ring[idx].callee_frame = callee_frame;
  g_boot_diag_data.ring_head = (idx + 1u) % 24u;
  g_boot_diag_data.ring_count = g_boot_diag_data.ring_count + 1u;

  uint32_t base = g_boot_fault_registers.stack_base;
  if(base == 0) base = 0x2000c728u;
  if(callee_frame <= base + 256u){
    g_boot_diag_data.near_bottom_hits = g_boot_diag_data.near_bottom_hits + 1u;
    watchdog_hw->scratch[1] = 0xDEADu;
  }
}
extern "C" void shizuku_boot_trace_push(uint32_t callee_frame){
  shizuku_boot_trace_push_context(callee_frame, 0, 0, 0, 0, 0);
}
extern "C" void shizuku_boot_trace_primitive_number(uint32_t number){
  g_boot_diag_data.last_primitive = number;
}
extern "C" void shizuku_boot_trace_call_enter(uint32_t sp, uint32_t frame){
  (void)frame;
  g_boot_diag_data.last_primitive = 1;
  g_boot_diag_data.last_sp_before = sp;
  watchdog_hw->scratch[1] = 415u;
}
extern "C" void shizuku_boot_trace_call_leave(uint32_t sp, uint32_t frame){
  (void)frame;
  g_boot_diag_data.last_sp_after = sp;
}
extern "C" void shizuku_boot_trace_return_enter(uint32_t sp, uint32_t frame){
  (void)frame;
  g_boot_diag_data.last_primitive = 2;
  g_boot_diag_data.last_sp_before = sp;
  watchdog_hw->scratch[1] = 416u;
}
extern "C" void shizuku_boot_trace_return_leave(uint32_t sp, uint32_t frame){
  (void)frame;
  g_boot_diag_data.last_sp_after = sp;
}
extern "C" void shizuku_boot_trace_primitive_stage(uint32_t stage){
  watchdog_hw->scratch[1] = stage & 0xffffu;
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

  const volatile uint32_t *p_pools = (const volatile uint32_t *)ADDR_POOLS;
  for(int i=0; i<4; ++i) g_boot_diag_data.pools_raw[i] = p_pools[i];
  const volatile uint32_t *p_def = (const volatile uint32_t *)ADDR_DEFAULT_ALARM_POOL;
  for(int i=0; i<6; ++i) g_boot_diag_data.default_pool_raw[i] = p_def[i];

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
extern "C" bool shizuku_boot_diag_data(uint32_t *out, uint32_t max_words){
  if(!out || max_words < 16)return false;
  const bool valid=(g_boot_diag_data.magic==BOOT_DIAG_MAGIC);
  if(valid){
    out[0]=g_boot_diag_data.min_sp;
    out[1]=g_boot_diag_data.last_callee_frame;
    out[2]=g_boot_diag_data.near_bottom_hits;
    out[3]=g_boot_diag_data.last_primitive;
    out[4]=g_boot_diag_data.last_sp_before;
    out[5]=g_boot_diag_data.last_sp_after;
    for(int i=0; i<4; ++i) out[6+i]=g_boot_diag_data.pools_raw[i];
    for(int i=0; i<6; ++i) out[10+i]=g_boot_diag_data.default_pool_raw[i];
  }
  return valid;
}
extern "C" void shizuku_boot_diag_get_summary(uint32_t *max_depth,
                                              uint32_t *max_depth_sp,
                                              uint32_t *ring_count){
  if(g_boot_diag_data.magic != BOOT_DIAG_MAGIC){
    if(max_depth) *max_depth = 0;
    if(max_depth_sp) *max_depth_sp = 0;
    if(ring_count) *ring_count = 0;
    return;
  }
  if(max_depth) *max_depth = g_boot_diag_data.max_depth;
  if(max_depth_sp) *max_depth_sp = g_boot_diag_data.max_depth_sp;
  if(ring_count) *ring_count = g_boot_diag_data.ring_count;
}
extern "C" bool shizuku_boot_diag_get_ring_entry(uint32_t seq,
                                                 uint32_t *thr,
                                                 uint32_t *depth,
                                                 uint32_t *obj,
                                                 uint32_t *kind,
                                                 uint32_t *hobj,
                                                 uint32_t *sp){
  if(g_boot_diag_data.magic != BOOT_DIAG_MAGIC || seq >= 24u) return false;
  uint32_t total = g_boot_diag_data.ring_count;
  uint32_t start = (total < 24u) ? 0u : g_boot_diag_data.ring_head;
  uint32_t slot = (start + seq) % 24u;
  if(thr) *thr = g_boot_diag_data.ring[slot].thread_id;
  if(depth) *depth = g_boot_diag_data.ring[slot].depth;
  if(obj) *obj = g_boot_diag_data.ring[slot].current_object;
  if(kind) *kind = g_boot_diag_data.ring[slot].current_kind;
  if(hobj) *hobj = g_boot_diag_data.ring[slot].current_handler_object;
  if(sp) *sp = g_boot_diag_data.ring[slot].callee_frame;
  return true;
}
#endif
