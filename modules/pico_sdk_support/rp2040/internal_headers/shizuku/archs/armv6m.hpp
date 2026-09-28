#ifndef SHIZUKU_ARCHS_ARMV6M_HPP
#define SHIZUKU_ARCHS_ARMV6M_HPP
#include <cstddef>
#include <cstdint>
#include "hardware/structs/systick.h"
#include "hardware/sync.h"
#include "shizuku/concepts/arch.hpp"
#include "shizuku/kernel_abi.hpp"

extern "C" {
void shizuku_armv6m_svc_entry();
void shizuku_armv6m_pendsv_entry();
void shizuku_armv6m_systick_entry();
void shizuku_armv6m_fault_entry();
void shizuku_armv6m_debugmon_entry();
struct shizuku_armv6m_context;
void shizuku_fault_dispatch(shizuku_armv6m_context *);
void shizuku_svc_dispatch(shizuku_armv6m_context *);
void shizuku_pendsv_dispatch(shizuku_armv6m_context *);
void shizuku_debug_dispatch(shizuku_armv6m_context *);
void shizuku_arm_pending_step();
void shizuku_restore_region_window();
void shizuku_armv6m_return_stub();
[[noreturn]] void shizuku_armv6m_enter_thread_mode(uintptr_t, void (*)());
shizuku_armv6m_context *shizuku_current_context();
}

namespace shizuku::archs {
class armv6m {
public:
  struct exception_frame_t { uint32_t r0,r1,r2,r3,r12,lr,pc,xPSR; };
  static constexpr uint32_t CONTROL_PRIV_PSP=2, CONTROL_UNPRIV_PSP=3;
  static constexpr uint32_t CALL_HEADROOM=512;
  struct context_t {
    uint32_t r4=0,r5=0,r6=0,r7=0,r8=0,r9=0,r10=0,r11=0;
    exception_frame_t *sp=nullptr;
    uint32_t exc_return=0xFFFFFFFD;
    uint32_t control=CONTROL_PRIV_PSP;
    uint32_t region_base=0, region_limit=0;
  };
  using method_t=uintptr_t (*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
  static uint32_t exc_frame_bytes(const context_t &) { return 32; }
  static uintptr_t psp_after_return(const context_t &c) {
    return (uintptr_t)c.sp+32+((c.sp->xPSR&(1u<<9))?4u:0u);
  }
  static void normalize_frame(exception_frame_t &f) { f.xPSR &= ~(1u<<9); }
  static uintptr_t arg(const exception_frame_t &f,unsigned n) {
    switch(n){case 0:return f.r0;case 1:return f.r1;case 2:return f.r2;case 3:return f.r3;default:return f.r12;}
  }
  static void set_args(exception_frame_t &f,const uintptr_t *a){f.r0=a[0];f.r1=a[1];f.r2=a[2];f.r3=a[3];}
  static void set_result(exception_frame_t &f,uintptr_t e,uintptr_t v){f.r0=e;f.r1=v;}
  static void set_entry(exception_frame_t &f,uintptr_t pc,uintptr_t lr){f.pc=pc;f.lr=lr;}
  static uintptr_t return_stub(){return (uintptr_t)&shizuku_armv6m_return_stub;}
  static void prepare_thread_entry(context_t &c,uintptr_t top,uintptr_t pc,uintptr_t ret,uintptr_t arg){
    auto *f=(exception_frame_t *)((top-sizeof(exception_frame_t))&~(uintptr_t)7);
    f->r0=arg;f->r1=f->r2=f->r3=f->r12=0;f->lr=ret;f->pc=pc;f->xPSR=1u<<24;c.sp=f;c.exc_return=0xFFFFFFFD;
  }
  static constexpr uint32_t TIMER_MAX_CYCLES=0x00FFFFFF,TIMER_MIN_CYCLES=100;
  static void timer_oneshot(uint32_t n){systick_hw->rvr=n;systick_hw->cvr=0;systick_hw->csr=7;}
  static void timer_cancel(){systick_hw->csr=0;}
  static uint32_t timer_remaining(bool &wrapped){uint32_t csr=systick_hw->csr;wrapped=(csr&(1u<<16))!=0;if(!(csr&1)){wrapped=false;return 0;}return systick_hw->cvr&0x00FFFFFFu;}
  static void pend_context_switch(){*(volatile uint32_t *)0xE000ED04u=1u<<28;}

  // RP2040 PMSAv6: up to 8 MPU regions; each region is aligned 2^n (n>=8).
  // This helper rounds a requested range outward. Subregions can trim only the
  // first/last eighth; any remaining expansion is deliberately documented.
  static constexpr uint32_t ACCESS_RW_ALL=3, ACCESS_RO_ALL=6;
  static bool region_range_representable(uintptr_t b,uintptr_t e){
    if(!e)return true;uint32_t rb,ra;if(!region_encode(b,e,rb,ra,ACCESS_RO_ALL,true))return false;
    const unsigned log=((ra>>1)&0x1Fu)+1u;const uintptr_t size=(uintptr_t)1<<log,lo=rb;
    return size<2048?(b==lo&&e==lo+size):(((b-lo)%(size/8)==0)&&((e-lo)%(size/8)==0));
  }
  static bool region_encode(uintptr_t base,uintptr_t limit,uint32_t &rbar,uint32_t &rasr,uint32_t access,bool xn){
    if(limit<=base)return false;
    uintptr_t lo=base,hi=limit;
    unsigned log=8;
    for(;log<32;log++){uintptr_t size=(uintptr_t)1<<log;lo=base&~(size-1);hi=(limit+size-1)&~(size-1);if(hi-lo==size)break;}
    if(log>=32)return false;
    const uintptr_t size=(uintptr_t)1<<log, sub=size/8;
    uint8_t disable=0;
    uintptr_t covered_lo=lo,covered_hi=hi;
    if(size>=2048){
      while(covered_lo+sub<=base){disable|=1u<<((covered_lo-lo)/sub);covered_lo+=sub;}
      while(covered_hi-sub>=limit){disable|=1u<<((covered_hi-lo)/sub-1);covered_hi-=sub;}
    }
    // Normal, shareable, non-cacheable memory (TEX=001, S=1); AP [26:24],
    // XN [28], SIZE [5:1], ENABLE [0]. RP2040 has no data cache.
    rbar=(uint32_t)lo;
    rasr=(1u<<19)|(1u<<18)|(xn?1u<<28:0u)|((uint32_t)access<<24)|(disable<<8)|((log-1)<<1)|1u;
    return true;
  }
  static bool region_try_set(uint32_t index,uintptr_t base,uintptr_t limit,uint32_t access,bool xn){
    uint32_t rb,ra;if(index>=8||!region_encode(base,limit,rb,ra,access,xn))return false;
    volatile uint32_t *mpu=(volatile uint32_t *)0xE000ED90u;
    mpu[2]=index;mpu[3]=rb;mpu[4]=ra;__asm volatile("dsb\n\tisb":::"memory");return true;
  }
  static void region_set(uint32_t i,uintptr_t b,uintptr_t e,uint32_t a,bool xn,uint32_t){
    if(!region_try_set(i,b,e,a,xn)) { region_disable(i); }
  }
  static void region_disable(uint32_t i){volatile uint32_t *m=(volatile uint32_t *)0xE000ED90u;m[2]=i;m[4]=0;}
  static void protection_enable(){volatile uint32_t *m=(volatile uint32_t *)0xE000ED90u;m[1]=5;__asm volatile("dsb\n\tisb":::"memory");}
  static void protection_disable(){((volatile uint32_t *)0xE000ED90u)[1]=0;}
  static constexpr uint32_t GRANT_REGION_INDEX=2;
  static bool grant_set(context_t &c,uintptr_t b,uintptr_t e){
    uint32_t rb,ra;if(!region_encode(b,e,rb,ra,ACCESS_RO_ALL,true))return false;
    const unsigned log=((ra>>1)&0x1Fu)+1u;const uintptr_t size=(uintptr_t)1<<log;
    const uintptr_t lo=rb,hi=lo+size;
    if(size<2048){if(b!=lo||e!=hi)return false;}
    else {const uintptr_t sub=size/8;if((b-lo)%sub||(e-lo)%sub)return false;}
    c.region_base=b;c.region_limit=e;return true;
  }
  static bool faulted_in_thread_mode(const context_t &c){return (c.exc_return&4u)!=0;}
  static uintptr_t frame_pc(const exception_frame_t &f){return f.pc;}
  static uintptr_t frame_lr(const exception_frame_t &f){return f.lr;}
  static uintptr_t fault_address(){return 0;}
  // Cortex-M0+ exposes no CFSR/MMFAR; MPU violations arrive as HardFault.
  static uint32_t fault_status(){return 0;}
  static void fault_status_clear(){}
  static uint32_t control_register(){uint32_t v;asm volatile("mrs %0, CONTROL":"=r"(v));return v;}
  static bool current_priv(){return (control_register()&1u)==0;}
  static void set_priv(context_t &c,bool p){c.control=p?CONTROL_PRIV_PSP:CONTROL_UNPRIV_PSP;}
  static bool set_region_window(context_t &c,uintptr_t b,uintptr_t e){return e==0?(c.region_base=c.region_limit=0,true):grant_set(c,b,e);}
  // Phase 1 software fallback. Phase 2 will dedicate MPU guard regions per stack.
  static void stack_limit_set(context_t &,uintptr_t) {/* TODO: PMSAv6 stack guard region */}
  static uintptr_t stack_limit(const context_t &){return 0;}
  // RP2040 M0+ has no LDREX/STREX. Reuse the Pico SDK's lock reserved for
  // atomic operations; lock 31 belongs to the claim-free allocator.
  static spin_lock_t *atomic_lock(){return spin_lock_instance(PICO_SPINLOCK_ID_ATOMIC);}
  static bool cas32(volatile uint32_t *p,uint32_t expected,uint32_t desired){auto *l=atomic_lock();uint32_t irq=spin_lock_blocking(l);bool ok=*p==expected;if(ok)*p=desired;spin_unlock(l,irq);return ok;}
  static void store_release32(volatile uint32_t *p,uint32_t v){auto *l=atomic_lock();uint32_t irq=spin_lock_blocking(l);asm volatile("dmb":::"memory");*p=v;asm volatile("dmb":::"memory");spin_unlock(l,irq);}
  static uint32_t load_acquire32(volatile uint32_t *p){auto *l=atomic_lock();uint32_t irq=spin_lock_blocking(l);asm volatile("dmb":::"memory");uint32_t v=*p;spin_unlock(l,irq);return v;}
  static constexpr uint32_t DFSR_HALTED=1,DFSR_BKPT=2;
  // M0+ has no DebugMonitor. These API-compatible stubs keep phase-1 builds
  // possible; self-hosted debug is unavailable until a separate design exists.
  static constexpr uintptr_t DFSR_ADDRESS=0xE000ED30u;
  static void debug_enable(bool){} static bool debug_enabled(){return false;} static void debug_step(bool){}
  static uint32_t debug_reason_take(){return 0;} static uint32_t breakpoint_count(){return 0;}
  static void breakpoint_enable(bool){} static void breakpoint_set(uint32_t,uintptr_t){} static void breakpoint_clear(uint32_t){}
  struct syscall_result{uintptr_t error,value;};
  static inline syscall_result syscall(uintptr_t n,uintptr_t a1=0,uintptr_t a2=0,uintptr_t a3=0,uintptr_t a4=0){
    register uintptr_t r0 asm("r0")=n;register uintptr_t r1 asm("r1")=a1;register uintptr_t r2 asm("r2")=a2;register uintptr_t r3 asm("r3")=a3;register uintptr_t r12 asm("r12")=a4;
    asm volatile("svc 0":"+r"(r0),"+r"(r1):"r"(r2),"r"(r3),"r"(r12):"memory");return {r0,r1};
  }
  [[noreturn]] static void enter_thread_mode(uintptr_t top,uintptr_t,void(*entry)()){shizuku_armv6m_enter_thread_mode(top,entry);}
};
static_assert(offsetof(armv6m::context_t,sp)==32);
static_assert(offsetof(armv6m::context_t,exc_return)==36);
static_assert(offsetof(armv6m::context_t,control)==40);
static_assert(offsetof(armv6m::context_t,region_base)==44);
static_assert((uintptr_t)shizuku::primitive::RETURN==2);
static_assert(shizuku::concepts::arch_requires<armv6m>);
}
#endif
