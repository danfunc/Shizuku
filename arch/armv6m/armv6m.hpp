#ifndef SHIZUKU_ARCHS_ARMV6M_HPP
#define SHIZUKU_ARCHS_ARMV6M_HPP
#include <cstddef>
#include <cstdint>
#include "arch/ppb.hpp"
#include "shizuku/concepts/arch.hpp"
#include "shizuku/kernel_abi.hpp"
#include "shizuku/service_profile.hpp"

extern "C" {
// atomic 用のロック。実体は modules/pico_sdk_support/rp2040 (SDK の spin lock)。
// 戻り値は割込み状態で、解放時にそのまま渡す。診断ビルドでは対象語と
// atomic 呼出し元も渡し、停止したロック操作を特定できるようにする。
uint32_t shizuku_armv6m_atomic_lock(uintptr_t address, uintptr_t caller_pc);
void shizuku_armv6m_atomic_unlock(uint32_t saved_irq, uintptr_t address,
                                 uintptr_t caller_pc);
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
// MpuRegionCount: PMSAv6 の region 数 (RP2040 は 8)。
// PrioBits: NVIC に実装された優先度 bit 数 (RP2040 は 2)。値は chips/ 側が渡す。
template <unsigned MpuRegionCount, unsigned PrioBits> class armv6m {
public:
  static constexpr unsigned MPU_REGION_COUNT = MpuRegionCount;
  static constexpr unsigned NVIC_PRIORITY_BITS = PrioBits;
  struct exception_frame_t { uint32_t r0,r1,r2,r3,r12,lr,pc,xPSR; };
  static constexpr uint32_t CONTROL_PRIV_PSP=2, CONTROL_UNPRIV_PSP=3;
  static constexpr uint32_t CALL_HEADROOM=512;
  static constexpr bool HAS_CYCLE_COUNTER=false; // M0+ に DWT CYCCNT は無い
  static constexpr bool IS_ARMV8M=false;
  struct context_t {
    uint32_t r4=0,r5=0,r6=0,r7=0,r8=0,r9=0,r10=0,r11=0;
    exception_frame_t *sp=nullptr;
    uint32_t exc_return=0xFFFFFFFD;
    uint32_t control=CONTROL_PRIV_PSP;
    uint32_t region_base=0, region_limit=0;
    uint32_t stack_limit_addr=0;
  };
  static constexpr uint32_t EXC_FRAME_MAX_BYTES=32;
  static constexpr uint32_t HANDLER_STACK_ARG_BYTES=8;
  static constexpr uint32_t FAULT_CONTEXT_BYTES=sizeof(context_t);
  // A PLAIN actor returns through a parent-handler dispatch, which allocates
  // another call frame. Reserve that frame when admitting the actor: otherwise
  // NO_STACK can also reject its return stub and leave it in the terminal loop.
  static bool call_entry_has_return_room(const context_t &c, uint32_t header_bytes) {
    if (c.stack_limit_addr == 0) return true;
    const uintptr_t frame_bytes = EXC_FRAME_MAX_BYTES;
    const uintptr_t unaligned = header_bytes + frame_bytes + HANDLER_STACK_ARG_BYTES;
    const uintptr_t total = (unaligned + 7u) & ~(uintptr_t)7u;
    const uintptr_t dispatch_reserve = unaligned + EXC_FRAME_MAX_BYTES +
                                       FAULT_CONTEXT_BYTES + CALL_HEADROOM;
    const uintptr_t sp = (uintptr_t)c.sp;
    const uintptr_t limit = c.stack_limit_addr;
    // One total is the new actor frame; the other is its return dispatch.
    return sp >= limit && sp - limit >= 2u * total + dispatch_reserve;
  }
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
  static void set_handler_args(exception_frame_t &f,const uintptr_t *a,
                               uintptr_t stack_arg_address){
    set_args(f,a);*reinterpret_cast<uint32_t *>(stack_arg_address)=(uint32_t)a[4];
  }
  static void set_result(exception_frame_t &f,uintptr_t e,uintptr_t v){f.r0=e;f.r1=v;}
  static void set_entry(exception_frame_t &f,uintptr_t pc,uintptr_t lr){f.pc=pc;f.lr=lr;}
  static uintptr_t return_stub(){return (uintptr_t)&shizuku_armv6m_return_stub;}
  static void prepare_thread_entry(context_t &c,uintptr_t top,uintptr_t pc,uintptr_t ret,uintptr_t arg){
    auto *f=(exception_frame_t *)((top-sizeof(exception_frame_t))&~(uintptr_t)7);
    f->r0=arg;f->r1=f->r2=f->r3=f->r12=0;f->lr=ret;f->pc=pc;f->xPSR=1u<<24;c.sp=f;c.exc_return=0xFFFFFFFD;
  }
  static constexpr uint32_t TIMER_MAX_CYCLES=0x00FFFFFF,TIMER_MIN_CYCLES=100;
  static void timer_oneshot(uint32_t n){ppb::systick->rvr=n;ppb::systick->cvr=0;ppb::systick->csr=7;}
  static void timer_cancel(){ppb::systick->csr=0;}
  static uint32_t timer_remaining(bool &wrapped){uint32_t csr=ppb::systick->csr;wrapped=(csr&(1u<<16))!=0;if(!(csr&1)){wrapped=false;return 0;}return ppb::systick->cvr&0x00FFFFFFu;}
  static void pend_context_switch(){*(volatile uint32_t *)0xE000ED04u=1u<<28;}

  // RP2040 PMSAv6: up to 8 MPU regions; each region is aligned 2^n (n>=8).
  // This helper rounds a requested range outward. Subregions can trim only the
  // first/last eighth; any remaining expansion is deliberately documented.
  static constexpr uint32_t ACCESS_RW_ALL=3, ACCESS_RO_ALL=6;
  static bool region_range_representable(uintptr_t b,uintptr_t e){
    if (!e) return true;
    uint32_t rb, ra;
    if (!region_encode(b, e, rb, ra, ACCESS_RO_ALL, true)) return false;
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
    uint32_t rb,ra;if(index>=MPU_REGION_COUNT||!region_encode(base,limit,rb,ra,access,xn))return false;
    volatile uint32_t *mpu=(volatile uint32_t *)0xE000ED90u;
    mpu[2]=index;mpu[3]=rb;mpu[4]=ra;__asm volatile("dsb\n\tisb":::"memory");return true;
  }
  static void region_set(uint32_t i,uintptr_t b,uintptr_t e,uint32_t a,bool xn,uint32_t){
    if(!region_try_set(i,b,e,a,xn)) { region_disable(i); }
  }
  static void region_disable(uint32_t i){volatile uint32_t *m=(volatile uint32_t *)0xE000ED90u;m[2]=i;m[4]=0;}
  static void protection_enable(){volatile uint32_t *m=(volatile uint32_t *)0xE000ED90u;m[1]=5;__asm volatile("dsb\n\tisb":::"memory");}
  static void protection_disable(){((volatile uint32_t *)0xE000ED90u)[1]=0;}
  static constexpr uint32_t GRANT_REGION_INDEX=5;
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
  static constexpr bool HAS_OBJECT_PROTECTION = true;
  static bool context_unprivileged(const context_t &c){return (c.control&1u)!=0;}
  [[gnu::always_inline]] static inline bool context_privileged(const context_t &c) {
    return (c.control & 1u) == 0;
  }
  [[gnu::always_inline]] static inline void set_method_argument(exception_frame_t &f, uintptr_t arg) {
    f.r0 = (uint32_t)arg;
  }
  [[gnu::always_inline]] static inline void move_descriptor_arguments(exception_frame_t &f) {
    f.r0 = f.r2;
  }
  struct inplace_payload_t {
    uint32_t r4_r11[8] = {};
  };

  // Region 配置 (PMSAv6, 8 本): 0 Flash コード (固定) / 1 空き / 2 スレッド stack 窓 /
  // 3 オブジェクト data 窓 / 4 オブジェクト MMIO 窓 / 5 貸与窓 / 6,7 空き。
  // SRAM 全域の窓は持たない。特権は PRIVDEFENA の背景領域で RAM に触れ、非特権は
  // 2・3・4・5 の窓の中だけに触れる。
  static constexpr uint32_t REGION_STACK=2, REGION_DATA=3, REGION_MMIO=4;
  // 窓は 2 の冪の大きさ・揃え、2048B 以上なら 1/8 (subregion) 単位で削れる。値は RASR。
  // thread の stack (THREAD_STACK_BYTES=4096) は 4096B 境界に置いて 4KB 領域でちょうど覆う。
  // 1KB や 2KB 境界では 8KB 領域をまたぐ置き方が生じ、より大きい領域に化けて隣を巻き込むので
  // この揃えが要る (代償: spawn 1 本につき最大 4KB の余白)。
  static constexpr uintptr_t STACK_WINDOW_ALIGN=4096;
  struct stack_window {
    uint32_t rbar = 0;
    uint32_t rlar = 0;  // v6-M では RASR (名前は thread の共通欄に合わせた)
  };
  using mpu_window_entry = stack_window;
  struct object_protection_window {
    uint32_t owner_id = 0;
    mpu_window_entry data{};
    mpu_window_entry mmio{};
  };
  static constexpr uint32_t kRasrAttrMask=(7u<<19)|(1u<<18)|(1u<<17)|(1u<<16);
  // 要求範囲が 2 の冪 + subregion で**ちょうど**表せるときだけ窓にする (外へはみ出す窓は作らない)。
  static mpu_window_entry encode_exact_window(uintptr_t base,uintptr_t limit,bool device){
    if(limit<=base||base==0||!region_range_representable(base,limit))return {};
    uint32_t rb,ra;if(!region_encode(base,limit,rb,ra,ACCESS_RW_ALL,true))return {};
    if(device)ra=(ra&~kRasrAttrMask)|(1u<<18)|(1u<<16); // Device, shareable
    return {rb,ra};
  }
  static mpu_window_entry encode_data_window(uintptr_t base,uintptr_t limit){return encode_exact_window(base,limit,false);}
  static mpu_window_entry encode_stack_window(uintptr_t base,uintptr_t limit){return encode_exact_window(base,limit,false);}
  // DMA / PIO / PPB (割込み・MPU 自身) は非特権に窓を出さない。MMIO は周辺 (0x40000000〜0x5FFFFFFF) の内側だけ。
  static constexpr bool is_dma_or_system_peripheral(uintptr_t base,uintptr_t limit){
    if(base<0x40000000u||limit>0x60000000u)return true;
    if(base<0x50010000u&&limit>0x50000000u)return true; // DMA
    if(base<0x50400000u&&limit>0x50200000u)return true; // PIO0/1
    return false;
  }
  static mpu_window_entry encode_mmio_window(uintptr_t base,uintptr_t limit){
    if(limit<=base||is_dma_or_system_peripheral(base,limit))return {};
    return encode_exact_window(base,limit,true);
  }

  static constexpr uint32_t kMaxObjectWindows=72;  // OBJECT_COUNT (configs/kit.bzl) 以上にする
  static inline object_protection_window s_object_windows[kMaxObjectWindows]={};
  // MPU は core ごとに持つので、region 3/4 に張ってある窓の持ち主も core ごとに覚える。
  static uint32_t core_slot(){return *(volatile const uint32_t *)0xD0000000u&1u;}
  static inline uint32_t s_programmed_object[2]={0xFFFFFFFFu,0xFFFFFFFFu};
  static inline uint32_t s_programmed_stack[2][2]={};
  static void invalidate_active_object(){s_programmed_object[core_slot()]=0xFFFFFFFFu;}
  static bool has_object_bounds(uint32_t id){return id<kMaxObjectWindows&&s_object_windows[id].owner_id!=0;}
  // 表現できない窓 (揃え・大きさ) は false で拒否し、登録しない。data/mmio のどちらかが 0 なら窓なし。
  static bool set_object_bounds(uint32_t id,uintptr_t db,uintptr_t dl,uintptr_t mb,uintptr_t ml){
    if(id>=kMaxObjectWindows)return false;
    const bool has_data=dl!=0,has_mmio=ml!=0;
    mpu_window_entry d{},m{};
    if(has_data){d=encode_data_window(db,dl);if(d.rlar==0)return false;}
    if(has_mmio){m=encode_mmio_window(mb,ml);if(m.rlar==0)return false;}
    s_object_windows[id].owner_id=id;s_object_windows[id].data=d;s_object_windows[id].mmio=m;
    return true;
  }
  static void region_write(uint32_t index,uint32_t rbar,uint32_t rasr){
    volatile uint32_t *m=(volatile uint32_t *)0xE000ED90u;m[2]=index;m[3]=rbar;m[4]=rasr;
  }
  static void apply_window_region(uint32_t index,const mpu_window_entry &w){
    if(w.rlar!=0)region_write(index,w.rbar,w.rlar);else region_disable(index);
  }
  // 窓の書込みは例外復帰の前にしか呼ばれない (復帰が文脈同期なので ISB は不要、DSB だけ置く)。
  static void apply_object_window(const object_protection_window &w){
    apply_window_region(REGION_DATA,w.data);apply_window_region(REGION_MMIO,w.mmio);
    __asm volatile("dsb":::"memory");
  }
  // 前回張った値と同じなら書かない (同じ thread へ戻る例外復帰は毎回ここを通る)。
  static void apply_stack_window(uint32_t rbar,uint32_t rasr){
    auto &cache=s_programmed_stack[core_slot()];
    if(cache[0]==rbar&&cache[1]==rasr)return;
    apply_window_region(REGION_STACK,mpu_window_entry{rbar,rasr});
    __asm volatile("dsb":::"memory");
    cache[0]=rbar;cache[1]=rasr;
  }
  // 非特権の窓 (region 3/4) を id のものにする。すでにそうなら何も書かない。特権は PRIVDEFENA で
  // 全域に触れ窓に制限されないので、特権の呼び先/呼び元のときは窓を触らない (呼ばない)。
  static void ensure_object_window(uint32_t id){
    uint32_t &programmed=s_programmed_object[core_slot()];
    if(programmed==id)return; // 同じ窓の組は書換えを省く
    if(id>=kMaxObjectWindows){region_disable(REGION_DATA);region_disable(REGION_MMIO);programmed=0xFFFFFFFFu;return;}
    apply_object_window(s_object_windows[id]);programmed=id;
  }
  template <class Frame>
  static void switch_callee_protection(context_t &,Frame &,uint32_t callee_id,bool privileged){
    if(!privileged)ensure_object_window(callee_id);
  }
  // 呼び元が非特権のときだけ呼ぶ (戻り先の窓を戻す)。
  static void switch_caller_protection(uint32_t caller_object){ensure_object_window(caller_object);}
  // ソフトウェア下限のみ。MPU ガード領域は未実装
  static void stack_limit_set(context_t &c, uintptr_t v){c.stack_limit_addr=(uint32_t)v;}
  static uintptr_t stack_limit(const context_t &c){return (uintptr_t)c.stack_limit_addr;}
  // RP2040 M0+ has no LDREX/STREX, so compare-exchange and release stores use
  // the Pico SDK atomic spinlock. Aligned acquire loads use LDR+DMB below and
  // do not need to serialize with other single-word accesses.
  [[gnu::always_inline]] static inline uint32_t atomic_profile_begin(){
#if SHIZUKU_KERNEL_SERVICE_PROFILE
    return shizuku_service_profile_now();
#else
    return 0;
#endif
  }
  [[gnu::always_inline]] static inline void atomic_profile_end(uint32_t start){
#if SHIZUKU_KERNEL_SERVICE_PROFILE
    shizuku_service_mask_span(start);
#else
    (void)start;
#endif
  }
  static bool cas32(volatile uint32_t *p,uint32_t expected,uint32_t desired){
    uint32_t start=atomic_profile_begin();
    const uintptr_t caller_pc=(uintptr_t)__builtin_return_address(0);
    uint32_t irq=shizuku_armv6m_atomic_lock((uintptr_t)p,caller_pc);
    bool ok=*p==expected;if(ok)*p=desired;
    shizuku_armv6m_atomic_unlock(irq,(uintptr_t)p,caller_pc);atomic_profile_end(start);return ok;
  }
  static void store_release32(volatile uint32_t *p,uint32_t v){
    uint32_t start=atomic_profile_begin();
    const uintptr_t caller_pc=(uintptr_t)__builtin_return_address(0);
    uint32_t irq=shizuku_armv6m_atomic_lock((uintptr_t)p,caller_pc);
    asm volatile("dmb":::"memory");*p=v;asm volatile("dmb":::"memory");
    shizuku_armv6m_atomic_unlock(irq,(uintptr_t)p,caller_pc);atomic_profile_end(start);
  }
  static uint32_t load_acquire32(volatile uint32_t *p){
    // All callers pass naturally aligned uint32_t words. RP2040's Cortex-M0+
    // performs an aligned word load as one indivisible bus access; acquire
    // ordering only needs the barrier after the load. Do not take the shared
    // Pico atomic spinlock here: loads are frequent in SVC/IRQ handlers, and a
    // blocked handler with PRIMASK set can starve watchdog and lockout IRQs.
    const uint32_t v=*p;
    asm volatile("dmb":::"memory");
    return v;
  }
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
// context_t のレイアウトは region 数に依らないので、RP2040 の実体で代表して検査する。
using armv6m_rp2040 = armv6m<8, 2>;
// These offsets describe the 32-bit RP2040 ABI; host argument-placement tests
// use a 64-bit uintptr_t and validate set_handler_args separately.
#if UINTPTR_MAX == UINT32_MAX
static_assert(offsetof(armv6m_rp2040::context_t,sp)==32);
static_assert(offsetof(armv6m_rp2040::context_t,exc_return)==36);
static_assert(offsetof(armv6m_rp2040::context_t,control)==40);
static_assert(offsetof(armv6m_rp2040::context_t,region_base)==44);
static_assert(offsetof(armv6m_rp2040::context_t,stack_limit_addr)==52);
#endif
static_assert((uintptr_t)shizuku::primitive::RETURN==2);
static_assert(shizuku::concepts::arch_requires<armv6m_rp2040>);
}
#endif
