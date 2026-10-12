#include "shizuku_port.hpp"
#include <atomic>

namespace {
// C5 is single-core. The active grant identifies the one task currently on CPU;
// avoid C++ thread_local/TLS because the bare Shizuku bootstrap does not install TLS blocks.
volatile uint32_t g_current_task_id=0;
volatile uint32_t g_isr_depth=0;
volatile uint32_t g_grant_parent_id=0;
volatile uint32_t g_grant_depth=0;
volatile uint32_t g_sleep_parent_ids[16]{};
volatile uint32_t g_sleep_depth=0;
uint64_t (*g_clock_us)()=nullptr;
std::atomic<bool> g_reschedule_pending{false};
shizuku::freertos_compat::shizuku_port::host_syscall_hook g_host_syscall_hook=nullptr;
}

namespace shizuku::freertos_compat {
namespace { domain *g_handler_domain=nullptr; }
bool shizuku_port::configure_route(uint32_t h,uint32_t m,uint32_t hz,void *d) {
  if(!h||!hz||!d||!g_clock_us)return false;
  handler_object_=h; dispatch_method_=m; tick_hz_=hz; domain_=static_cast<domain *>(d);
  g_handler_domain=domain_;
  task_start probe{kHandshake,0,0,0,nullptr,nullptr}; uintptr_t response=0;
  if(call(shizuku::object_api::CALL_METHOD,h,m,reinterpret_cast<uintptr_t>(&probe),&response)!=0||response!=kHandshakeReply) {
    handler_object_=dispatch_method_=0; domain_=nullptr; g_handler_domain=nullptr; return false;
  }
  return true;
}
void shizuku_port::set_clock(uint64_t (*clock_us)()) { g_clock_us=clock_us; }
void shizuku_port::set_host_syscall_hook(host_syscall_hook hook) { g_host_syscall_hook=hook; }
void shizuku_port::enter_isr() { g_isr_depth=g_isr_depth+1; }
void shizuku_port::leave_isr() { if(g_isr_depth)g_isr_depth=g_isr_depth-1; }
void shizuku_port::request_yield_from_isr() { g_reschedule_pending.store(true,std::memory_order_release); }
bool shizuku_port::pending_yield_from_isr() { return g_reschedule_pending.load(std::memory_order_acquire); }
bool shizuku_port::consume_reschedule_pending() { return g_reschedule_pending.exchange(false,std::memory_order_acq_rel); }
uintptr_t shizuku_port::call(shizuku::object_api api,uintptr_t a1,uintptr_t a2,uintptr_t a3,uintptr_t *value) {
#if defined(__riscv)
  register uintptr_t a0 asm("a0")=static_cast<uintptr_t>(api);
  register uintptr_t a1r asm("a1")=a1;
  register uintptr_t a2r asm("a2")=a2;
  register uintptr_t a3r asm("a3")=a3;
  asm volatile("ecall" : "+r"(a0), "+r"(a1r) : "r"(a2r), "r"(a3r) : "memory");
  if(value)*value=a1r;
  return a0;
#else
  if(g_host_syscall_hook)return g_host_syscall_hook(api,a1,a2,a3,value);
  (void)api;(void)a1;(void)a2;(void)a3;if(value)*value=0;return static_cast<uintptr_t>(shizuku::object_error::KERNEL_REFUSED);
#endif
}
uintptr_t shizuku_port::call_handler(task_start *packet) {
  uintptr_t result=0;
  const auto error=call(shizuku::object_api::CALL_METHOD,handler_object_,dispatch_method_,
                        reinterpret_cast<uintptr_t>(packet),&result);
  return error==0?result:0;
}
uint64_t shizuku_port::now_ticks() {
  return (now_us()*tick_hz_)/1000000u;
}
uint64_t shizuku_port::now_us() {
  task_start request{kReadClock,0,0,0,nullptr,nullptr};
  return call_handler(&request);
}
uint32_t shizuku_port::current_task() { return g_current_task_id; }
bool shizuku_port::in_isr() { return g_isr_depth!=0; }
void shizuku_port::yield() {
  if(in_isr())return;
  const uint32_t task=g_current_task_id;
  const bool lender_context=(g_grant_depth!=0)||(g_sleep_depth!=0);
  const uint32_t parent=g_grant_depth?g_grant_parent_id:
      (g_sleep_depth?g_sleep_parent_ids[g_sleep_depth-1]:task);
  if(lender_context)g_current_task_id=parent;
  call(shizuku::object_api::YIELD);
  g_current_task_id=task;
}
void shizuku_port::park_until(uint64_t deadline) {
  if(in_isr())return;
  uint64_t poll_us=100;
  const auto now=now_ticks();
  if(deadline!=UINT64_MAX&&deadline>now) {
    const uint64_t until=((deadline-now)*1000000u+tick_hz_-1)/tick_hz_;
    if(until<poll_us)poll_us=until;
  }
  if(poll_us==0)poll_us=1;
  sleep_us(poll_us);
  if(domain_)domain_->make_ready(current_task()); // timer poll or resource wake; next GRANT resumes the waiter.
  yield();
}
bool shizuku_port::wake(uint32_t id) {
  if(!domain_)return false;
  return domain_->task_priority(id)>domain_->task_priority(current_task());
}
bool shizuku_port::spawn(void (*entry)(void *),void *arg,uint32_t stack,uint32_t priority,uint32_t *id) {
  // Shizuku's current SPAWN API allocates a fixed 4096-byte thread stack.
  // IDF's RISC-V xTaskCreate stack depth is bytes; fail closed above capacity.
  if(!handler_object_||!entry||!id||!stack||stack>kKernelThreadStackBytes)return false;
  task_start *slot=nullptr;
  for(auto &candidate:starts_)if(candidate.operation==0){slot=&candidate;break;}
  if(!slot)return false;
  auto &packet=*slot; packet={kStartTask,0,priority,stack,entry,arg};
  uintptr_t spawned=0;
  const auto error=call(shizuku::object_api::SPAWN,handler_object_,dispatch_method_,reinterpret_cast<uintptr_t>(&packet),&spawned);
  if(error||!spawned){packet={};return false;}
  packet.id=static_cast<uint32_t>(spawned); *id=packet.id; return true;
}
bool shizuku_port::run_for(uint32_t task,uint32_t cycles) {
  const uint32_t previous=g_current_task_id,previous_parent=g_grant_parent_id,previous_depth=g_grant_depth;
  g_current_task_id=task; g_grant_parent_id=previous; g_grant_depth=previous_depth+1;
  uintptr_t result=0; const auto error=call(shizuku::object_api::RUN_FOR,task,cycles,0,&result);
  g_current_task_id=previous;g_grant_parent_id=previous_parent;g_grant_depth=previous_depth;return error==0;
}
void shizuku_port::sleep_us(uint64_t us) {
  if(in_isr())return;
  const uint32_t task=g_current_task_id;
  const uint32_t parent=g_grant_depth?g_grant_parent_id:task;
  const uint32_t depth=g_sleep_depth;
  if(depth>=16){yield();return;}
  g_sleep_parent_ids[depth]=parent;g_sleep_depth=depth+1;
  if(g_grant_depth)g_current_task_id=parent;
  call(shizuku::object_api::SLEEP_US,static_cast<uintptr_t>(us));
  g_current_task_id=task;
  g_sleep_depth=depth;
}
void shizuku_port::exit_thread() {
  if(g_grant_depth)g_current_task_id=g_grant_parent_id;
  else if(g_sleep_depth)g_current_task_id=g_sleep_parent_ids[g_sleep_depth-1];
  call(shizuku::object_api::EXIT_THREAD);
}
void shizuku_port::task_finished(uint32_t id) {
  for(auto &packet:starts_)if(packet.operation==kStartTask&&packet.id==id){packet={};return;}
}
uint32_t shizuku_port::irq_save() {
#if defined(__riscv)
  uintptr_t old=0,mask=8; asm volatile("csrrc %0, mstatus, %1":"=r"(old):"r"(mask):"memory"); return (old&8)?1u:0u;
#else
  return 0;
#endif
}
void shizuku_port::irq_restore(uint32_t state) {
#if defined(__riscv)
  if(state&1u){uintptr_t mask=8;asm volatile("csrs mstatus, %0"::"r"(mask):"memory");}
#else
  (void)state;
#endif
}

extern "C" uintptr_t shizuku_freertos_compat_dispatch(uintptr_t raw) {
  auto *packet=reinterpret_cast<shizuku_port::task_start *>(raw);
  if(!packet)return 0;
  if(packet->operation==shizuku_port::kReadClock)return g_clock_us?g_clock_us():0;
  if(packet->operation==shizuku_port::kHandshake)return shizuku_port::kHandshakeReply;
  if(packet->operation==shizuku_port::kStartTask) {
    const uint32_t previous=g_current_task_id; g_current_task_id=packet->id;
    if(packet->entry)packet->entry(packet->arg);
    // The dispatch method is spawned as a Shizuku task; return through the
    // object API so the caller's grant ledger is unwound by the HANDLER.
    if(g_handler_domain)g_handler_domain->finish_current_task();
    else shizuku_port::call(shizuku::object_api::EXIT_THREAD);
    g_current_task_id=previous;
  }
  return 0;
}
} // namespace shizuku::freertos_compat

extern "C" void shizuku_freertos_compat_set_clock(uint64_t (*clock_us)(void)) {
  shizuku::freertos_compat::shizuku_port::set_clock(clock_us);
}
extern "C" void shizuku_freertos_compat_isr_enter(void) { shizuku::freertos_compat::shizuku_port::enter_isr(); }
extern "C" void shizuku_freertos_compat_isr_exit(void) { shizuku::freertos_compat::shizuku_port::leave_isr(); }
extern "C" void shizuku_freertos_compat_yield_from_isr(int higher) {
  if(higher)shizuku::freertos_compat::shizuku_port::request_yield_from_isr();
}
extern "C" void shizuku_freertos_compat_run_scheduler(uint32_t cycles) {
  if(shizuku::freertos_compat::g_handler_domain)shizuku::freertos_compat::g_handler_domain->run_scheduler(cycles);
}
