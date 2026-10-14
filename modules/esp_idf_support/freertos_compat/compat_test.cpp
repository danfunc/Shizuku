#include "compat.hpp"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "shizuku_port.hpp"
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

struct run_for_raw_observation {
  uint32_t task_id = 0;
  uint32_t a0_error = 0;
  uint32_t a1_value = 0;
  uint32_t count = 0;
};
run_for_raw_observation g_run_for_raw{};
uint32_t g_task_started_id = 0;
#if defined(SHIZUKU_ESP32_C6)
uint32_t g_c6_stack_probe_count = 0;
uint32_t g_c6_stack_probe_id = 0;
#endif
uint32_t g_plain_kernel_thread_id = 48;
uint32_t g_plain_spawn_result = 78;
uintptr_t g_mock_run_for_error = 0;
uintptr_t g_mock_run_for_value = 0;

// The host API test links the same wrappers as BLE firmware. Firmware-only
// diagnostics and deferred C5 context switching are inert in this test.
extern "C" {
volatile uint32_t shizuku_esp_ble_timeout_shim_calls = 0;
volatile uint32_t shizuku_esp_ble_queue_sem_shim_calls = 0;
volatile uint32_t shizuku_esp_ble_critical_shim_calls = 0;
void shizuku_esp_ble_pend_compat_context_switch(void) {}
void shizuku_esp_ble_note_unimplemented(uint32_t) {}
void shizuku_esp_ble_note_sync_alloc(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void shizuku_esp_ble_note_queue_shape(uint32_t, uint32_t) {}
void shizuku_esp_ble_note_notification(uint32_t, bool) {}
void shizuku_esp_ble_note_task_create(uint32_t, const char *, uint32_t, uint32_t) {}
void shizuku_esp_ble_note_wait(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void shizuku_esp_ble_note_wait_callsite(uint32_t) {}
void shizuku_esp_ble_note_scheduler(uint32_t, uint32_t) {}
void shizuku_esp_ble_note_task_started(uint32_t task_id) { g_task_started_id = task_id; }
#if defined(SHIZUKU_ESP32_C6)
void shizuku_c6_note_plain_task_stack_start(uint32_t, uintptr_t) {}
void shizuku_c6_note_plain_task_stack_probe(uint32_t task_id) {
  g_c6_stack_probe_id = task_id;
  ++g_c6_stack_probe_count;
}
#endif
void shizuku_esp_ble_note_run_for_result(uint32_t task_id, uint32_t a0_error,
                                         uint32_t a1_value) {
  g_run_for_raw = {task_id, a0_error, a1_value, g_run_for_raw.count + 1u};
}
void shizuku_esp_ble_note_sync_object(uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t) {}
void shizuku_esp_ble_note_queue_send(uint32_t, uint32_t, uint32_t) {}
void shizuku_esp_ble_note_queue_receive(uint32_t, uint32_t, uint32_t) {}
}

using namespace shizuku::freertos_compat;

struct test_port final : port {
  uint64_t now=0; uint32_t awakened=0;
  std::function<void()> on_park;
  std::function<void(uint32_t,uint32_t)> on_run_for;
  std::mutex m; std::condition_variable cv; bool parked=false, token=false;
  std::set<uint32_t> parked_ids, tokens;
  uint32_t priorities[32]{};
  uint32_t route_handler=0,route_method=0,route_hz=0;
  domain *route_domain=nullptr;
  std::function<void()> on_irq_save;
  size_t irq_save_calls=0,irq_save_hook_at=0;
  static thread_local uint32_t task;
  static thread_local bool isr;
  static std::atomic<uint32_t> next_task;
  void set_task(uint32_t id){task=id;}
  bool configure_route(uint32_t h,uint32_t method,uint32_t hz,void *d) override{route_handler=h;route_method=method;route_hz=hz;route_domain=static_cast<domain*>(d);return d&&hz;}
  uint64_t now_ticks() override{return now;}
  uint64_t now_us() override{return now*10000u;}
  uint32_t block_calls=0;
  size_t current_task_countdown=0; std::function<void()> on_current_task;
  void block_task(uint32_t,uint64_t) override{++block_calls;}
  uint32_t current_task() override{
    if(on_current_task&&current_task_countdown&&--current_task_countdown==0){auto cb=on_current_task;on_current_task={};cb();}
    return task;
  }
  bool in_isr() override{return isr;}
  void yield() override{}
  void park_until(uint64_t deadline) override {
    if(on_park){auto cb=on_park;on_park={};cb();std::lock_guard<std::mutex> lock(m);token=false;return;}
    std::unique_lock<std::mutex> lock(m); parked=true; parked_ids.insert(task); cv.notify_all();
    bool woke;
    auto ready=[&]{return tokens.contains(task);};
    if(deadline==UINT64_MAX){cv.wait(lock,ready);woke=true;}
    else woke=cv.wait_for(lock,std::chrono::milliseconds(deadline>now?deadline-now:0),ready);
    if(woke)tokens.erase(task);
    if(!woke)now=deadline;
    parked_ids.erase(task); parked=!parked_ids.empty();
  }
  bool wake(uint32_t id) override{std::lock_guard<std::mutex> lock(m);awakened=id;tokens.insert(id);token=true;cv.notify_all();return priorities[id]>priorities[task];}
  bool spawn(void(*)(void*),void*,uint32_t,uint32_t priority,uint32_t *id) override{*id=++next_task;if(*id<32)priorities[*id]=priority;return true;}
  bool run_for(uint32_t id,uint32_t cycles) override{if(on_run_for)on_run_for(id,cycles);return true;}
  uint32_t irq_save() override{
    ++irq_save_calls;
    if(on_irq_save&&irq_save_calls==irq_save_hook_at){auto cb=on_irq_save;on_irq_save={};cb();}
    return 9;
  }
  void irq_restore(uint32_t) override{}
};
thread_local uint32_t test_port::task=1;
thread_local bool test_port::isr=false;
std::atomic<uint32_t> test_port::next_task{1};
uint64_t test_clock_us(){return 1234567;}
struct task_entry_probe { shizuku::freertos_compat::shizuku_port *port; uint32_t expected; bool started=false; };
void mark_task_started(void *arg){auto *p=static_cast<task_entry_probe*>(arg);const bool before=p->port->current_task()==p->expected;p->port->yield();p->started=before&&p->port->current_task()==p->expected;}
struct syscall_observation { shizuku::object_api calls[32]{}; size_t count=0; };
syscall_observation *g_syscall_observation=nullptr;
shizuku::freertos_compat::shizuku_port::task_start *g_spawn_packet=nullptr;
shizuku::freertos_compat::shizuku_port *g_hook_port=nullptr;
bool g_yield_context_restored=false;
bool g_plain_task_route=false;
uint32_t g_plain_object_id=0;
struct stop_scheduler_test {};
uint32_t g_timer_callback_count=0;
void *g_expected_timer_id=nullptr;
void timer_callback(TimerHandle_t timer) {
  assert(pvTimerGetTimerID(timer)==g_expected_timer_id);
  ++g_timer_callback_count;
}
void task_yield_probe(void*) {
  const auto id=g_hook_port->current_task();
  g_hook_port->yield();
  const bool after_yield=g_hook_port->current_task()==id;
  g_hook_port->sleep_us(11);
  g_yield_context_restored=after_yield&&g_hook_port->current_task()==id;
}
namespace {
struct non_lifo_sleep_test {
  enum state_t {
    START_A,
    A_IN_HOOK,
    B_IN_HOOK,
    A_RETURNED,
    ROUND_DONE,
    ABORTED
  };
  std::mutex m;
  std::condition_variable cv;
  state_t state = START_A;
  uint32_t a_calls = 0;
  uint32_t b_calls = 0;
  bool failed = false;

  void reset() {
    state = START_A;
    a_calls = 0;
    b_calls = 0;
    failed = false;
  }
} g_non_lifo_test;

uintptr_t non_lifo_sleep_syscall_hook(shizuku::object_api api, uintptr_t, uintptr_t, uintptr_t, uintptr_t *value) {
  if (api == shizuku::object_api::SLEEP_US) {
    std::unique_lock<std::mutex> lock(g_non_lifo_test.m);
    if (g_non_lifo_test.state == non_lifo_sleep_test::START_A) {
      ++g_non_lifo_test.a_calls;
      g_non_lifo_test.state = non_lifo_sleep_test::A_IN_HOOK;
      g_non_lifo_test.cv.notify_all();
      g_non_lifo_test.cv.wait_for(lock, std::chrono::seconds(2), [&]{
        return g_non_lifo_test.state == non_lifo_sleep_test::B_IN_HOOK || g_non_lifo_test.failed;
      });
    } else if (g_non_lifo_test.state == non_lifo_sleep_test::A_IN_HOOK) {
      ++g_non_lifo_test.b_calls;
      g_non_lifo_test.state = non_lifo_sleep_test::B_IN_HOOK;
      g_non_lifo_test.cv.notify_all();
      g_non_lifo_test.cv.wait_for(lock, std::chrono::seconds(2), [&]{
        return g_non_lifo_test.state == non_lifo_sleep_test::A_RETURNED || g_non_lifo_test.failed;
      });
    } else {
      g_non_lifo_test.failed = true;
      g_non_lifo_test.cv.notify_all();
    }
  }
  if (value) *value = 0;
  return 0;
}
} // namespace
namespace {
// Thread-identity regression: while thread 2 is inside SLEEP_US (lender context rewrote
// the global id to its parent, 0), the kernel runs thread 3, which takes a mutex.
struct identity_probe {
  shizuku::freertos_compat::shizuku_port *port=nullptr;
  domain *dom=nullptr; int mutex=-1;
  uintptr_t sp=0; bool took=false; bool ran=false;
} g_identity;
uintptr_t identity_sp() { return g_identity.sp; }
uintptr_t identity_syscall(shizuku::object_api api,uintptr_t,uintptr_t,uintptr_t,uintptr_t *value) {
  if(value)*value=0;
  if(api==shizuku::object_api::RUN_FOR) {
    g_identity.sp=0x20800;                       // running on thread 2's stack
    g_identity.port->sleep_us(10);               // lender context: global id becomes the parent (0)
    g_identity.sp=0x20800;
  } else if(api==shizuku::object_api::SLEEP_US) {
    g_identity.sp=0x21800;                       // kernel schedules thread 3 meanwhile
    g_identity.ran=true;
    g_identity.took=g_identity.dom->mutex_take(g_identity.mutex,0);
  }
  return 0;
}
} // namespace
uintptr_t host_shizuku_syscall(shizuku::object_api api,uintptr_t a1,uintptr_t a2,uintptr_t a3,uintptr_t *value) {
  assert(g_syscall_observation&&g_syscall_observation->count<32);
  g_syscall_observation->calls[g_syscall_observation->count++]=api;
  if(api==shizuku::object_api::CALL_METHOD) {
    assert(a1==41&&a2==0);
    if(value)*value=shizuku_freertos_compat_dispatch(a3);
  } else if(g_plain_task_route&&api==shizuku::object_api::CREATE_OBJECT) {
    assert(a1>=48&&a1<=63&&a2!=0&&a3==0);
    g_plain_object_id=static_cast<uint32_t>(a1);
    if(value)*value=a1;
  } else if(g_plain_task_route&&api==shizuku::object_api::SPAWN) {
    assert(a1==g_plain_object_id&&a2==0);
    auto *packet=reinterpret_cast<shizuku::freertos_compat::shizuku_port::task_start *>(a3);
    assert(packet->operation==shizuku::freertos_compat::shizuku_port::kStartTask);
    g_spawn_packet=packet;
    if(value)*value=g_plain_spawn_result;
  } else if(g_plain_task_route&&api==shizuku::object_api::RUN_FOR) {
    assert(a1==g_plain_kernel_thread_id&&a2==16000);
    assert(g_spawn_packet&&g_spawn_packet->id==a1);
    if(value)*value=0; // grant_end::EXPIRED with object_error::OK
    assert(shizuku_freertos_compat_dispatch(reinterpret_cast<uintptr_t>(g_spawn_packet))==0);
    return 0;
  } else if(api==shizuku::object_api::SPAWN) {
    assert(a1==41&&a2==0);
    auto *packet=reinterpret_cast<shizuku::freertos_compat::shizuku_port::task_start *>(a3);
    assert(packet->operation==shizuku::freertos_compat::shizuku_port::kStartTask);
    g_spawn_packet=packet;
    if(value)*value=77;
  } else if(api==shizuku::object_api::RUN_FOR) {
    assert(a1==77&&a2==160000);
    assert(g_spawn_packet&&shizuku_freertos_compat_dispatch(reinterpret_cast<uintptr_t>(g_spawn_packet))==0);
    if(value)*value=0;
  } else if(value)*value=0;
  return 0;
}
uintptr_t raw_run_for_syscall(shizuku::object_api api, uintptr_t task_id,
                              uintptr_t cycles, uintptr_t, uintptr_t *value) {
  assert(api==shizuku::object_api::RUN_FOR&&task_id==12&&cycles==1600);
  if(value)*value=g_mock_run_for_value;
  return g_mock_run_for_error;
}
int main(){
 test_port p; domain d(p); uint32_t tid=0;
 assert(d.task_create([](void*){},"worker",256,nullptr,3,&tid)&&tid==2);
 assert(!d.task_create(nullptr,"bad",256,nullptr,1,nullptr));
 p.set_task(tid);
 uint32_t previous=0, notified=0;
 assert(d.task_notify(tid,0,0x5,static_cast<uint32_t>(eSetBits),&previous));
 assert(previous==0&&d.task_notify_wait(0,0x1,0x4,&notified,0));
 assert(notified==0x5&&d.task_notify_take(0,true,0)==1);
 p.on_park=[&]{p.set_task(3);assert(d.task_notify(tid,0,0x20,static_cast<uint32_t>(eSetBits),nullptr));p.set_task(tid);};
 assert(d.task_notify_wait(0,0,0,&notified,20)&&notified==0x20);
 assert(d.task_notify(tid,0,7,static_cast<uint32_t>(eSetValueWithOverwrite),&previous));
 assert(!d.task_notify(tid,0,9,static_cast<uint32_t>(eSetValueWithoutOverwrite),&previous));
 assert(previous==7&&d.task_notify_take(0,true,0)==7);
 p.priorities[tid]=5; p.priorities[3]=1;
 bool notification_higher=false;
 p.on_park=[&]{p.set_task(3);test_port::isr=true;
   assert(d.task_notify(tid,0,0,static_cast<uint32_t>(eIncrement),nullptr,true,&notification_higher));
   test_port::isr=false;p.set_task(tid);};
 assert(d.task_notify_take(0,true,20)==1&&notification_higher);
 assert(d.task_notify_take(0,true,0)==0);
 p.set_task(1);
 const int q=d.create_queue(1,sizeof(uint32_t)); uint32_t in=42,out=0;
 assert(q>=0&&d.queue_send(q,&in,0)); assert(d.queue_messages(q)==1);
 assert(!d.queue_send(q,&in,0)); assert(d.queue_receive(q,&out,0)&&out==42);
 assert(!d.queue_receive(q,&out,3)&&p.now==3);
 p.set_task(tid); p.on_park=[&]{p.set_task(3);assert(d.queue_send(q,&in,0));p.set_task(tid);};
 assert(d.queue_receive(q,&out,20)&&out==in); assert(p.awakened==tid);
 const int s=d.create_semaphore(2,0); assert(s>=0);
 assert(!d.semaphore_take(s,2)&&p.now==5);
 p.on_park=[&]{p.set_task(3);assert(d.semaphore_give(s));p.set_task(tid);};
 assert(d.semaphore_take(s,20)); assert(p.awakened==tid);
 assert(d.semaphore_give(s)); assert(d.semaphore_take(s,0));
 test_port::isr=true; assert(!d.semaphore_take(s,1,true)); assert(d.semaphore_give(s,true));
 assert(!d.queue_send(q,&in,1,false,true)); test_port::isr=false;
 const int nonrecursive=d.create_mutex(false); assert(nonrecursive>=0);
 assert(d.object_set_creator(nonrecursive,0x12345678));
 p.set_task(tid); assert(d.mutex_take(nonrecursive,0));
 shizuku::freertos_compat::sync_snapshot mutex_state{};
 assert(d.object_snapshot(nonrecursive,&mutex_state));
 assert(mutex_state.kind==3&&mutex_state.creator_pc==0x12345678);
 assert(mutex_state.owner==tid&&mutex_state.recursion==1&&mutex_state.take_count==1);
 const auto before_self_retake=p.now;
 assert(!d.mutex_take(nonrecursive,2,false,false));
 assert(p.now==before_self_retake+2&&d.mutex_owner(nonrecursive)==tid);
 assert(d.mutex_give(nonrecursive));
 assert(d.object_snapshot(nonrecursive,&mutex_state));
 assert(mutex_state.owner==UINT32_MAX&&mutex_state.last_giver_task==tid);
 assert(mutex_state.take_count==2&&mutex_state.give_count==1);

 // A waiter must be made runnable by the holder's give and then acquire ownership.
 p.set_task(tid); assert(d.mutex_take(nonrecursive,0));
 p.set_task(3);
 p.on_park=[&]{p.set_task(tid);assert(d.mutex_give(nonrecursive));p.set_task(3);};
 assert(d.mutex_take(nonrecursive,20,false,false));
 assert(d.mutex_owner(nonrecursive)==3&&p.awakened==3);
 assert(d.mutex_give(nonrecursive));

 // Lost wakeup: a give that lands after the waiter registered but before it is
 // BLOCKED must not leave the waiter asleep on a free mutex (nimble_host hang).
 {
   uint32_t lw=0; assert(d.task_create([](void*){},"lw",256,nullptr,5,&lw));
   const int lm=d.create_mutex(false); assert(lm>=0);
   p.set_task(tid); assert(d.mutex_take(lm,0));
   p.set_task(lw); p.block_calls=0; p.tokens.clear();
   const auto lw_start=p.now;
   // 3rd current_task() call = block_current() entry (take, add_waiter, block_current).
   p.current_task_countdown=3;
   p.on_current_task=[&]{p.set_task(tid);assert(d.mutex_give(lm));p.set_task(lw);};
   assert(d.mutex_take(lm,20,false,false));
   assert(d.mutex_owner(lm)==lw);
   assert(p.block_calls==0&&p.now==lw_start); // must not have slept on a free mutex
   assert(d.mutex_give(lm)); p.tokens.clear();
   // Same window for task notifications.
   p.block_calls=0; p.current_task_countdown=2; // task_notify_take: current_task, block_current
   p.on_current_task=[&]{p.set_task(tid);assert(d.task_notify(lw,0,1,2,nullptr));p.set_task(lw);};
   assert(d.task_notify_take(0,true,20)==1);
   assert(p.block_calls==0&&p.now==lw_start); p.tokens.clear();
   // A forever wait is bounded: block_current() is handed a finite deadline.
   p.set_task(tid); assert(d.mutex_take(lm,0)); p.set_task(lw);
   p.on_park=[&]{p.set_task(tid);assert(d.mutex_give(lm));p.set_task(lw);};
   assert(d.mutex_take(lm,kForever,false,false)&&d.mutex_owner(lm)==lw);
   assert(d.mutex_give(lm)); p.tokens.clear(); p.set_task(tid);
 }

 // Priority inheritance raises a low-priority holder while a higher-priority task waits.
 uint32_t low_mutex_task=0, high_mutex_task=0;
 assert(d.task_create([](void*){},"mutex_low",256,nullptr,2,&low_mutex_task));
 assert(d.task_create([](void*){},"mutex_high",256,nullptr,9,&high_mutex_task));
 p.priorities[low_mutex_task]=2; p.priorities[high_mutex_task]=9;
 const int inherited_mutex=d.create_mutex(false); assert(inherited_mutex>=0);
 p.set_task(low_mutex_task); assert(d.mutex_take(inherited_mutex,0));
 p.set_task(high_mutex_task);
 p.on_park=[&]{
   assert(d.task_priority(low_mutex_task)==9);
   p.set_task(low_mutex_task); assert(d.mutex_give(inherited_mutex));
   assert(d.task_priority(low_mutex_task)==2);
   p.set_task(high_mutex_task);
 };
 assert(d.mutex_take(inherited_mutex,20,false,false));
 assert(d.mutex_owner(inherited_mutex)==high_mutex_task);
 assert(d.mutex_give(inherited_mutex));
 // Timing out must hand the inherited priority back, and a bounded forever-wait
 // recheck must keep the waiter queued (no priority flapping / FIFO loss).
 p.tokens.clear();
 p.set_task(low_mutex_task); assert(d.mutex_take(inherited_mutex,0));
 p.set_task(high_mutex_task); assert(!d.mutex_take(inherited_mutex,3,false,false));
 assert(d.task_priority(low_mutex_task)==2);
 p.set_task(low_mutex_task); assert(d.mutex_give(inherited_mutex)); p.set_task(high_mutex_task); p.tokens.clear();


 // A task that exits while holding a mutex remains observable as the owner.
 uint32_t abandoned_mutex_task=0;
 assert(d.task_create([](void*){},"mutex_abandoned",256,nullptr,4,&abandoned_mutex_task));
 const int abandoned_mutex=d.create_mutex(false); assert(abandoned_mutex>=0);
 p.set_task(abandoned_mutex_task); assert(d.mutex_take(abandoned_mutex,0));
 assert(d.task_delete(abandoned_mutex_task));
 p.set_task(1);
 assert(d.object_snapshot(abandoned_mutex,&mutex_state));
 assert(mutex_state.owner==abandoned_mutex_task&&
        mutex_state.owner_state==static_cast<uint32_t>(shizuku::freertos_compat::state::done));

 const int m=d.create_mutex(true); assert(m>=0);
 assert(d.object_snapshot(m,&mutex_state)&&mutex_state.recursive==1);
 p.set_task(tid); assert(d.mutex_take(m,0)); assert(d.mutex_take(m,0));
 assert(d.mutex_give(m)); assert(d.mutex_give(m)); assert(!d.mutex_give(m));
 assert(d.task_suspend(tid)); assert(d.task_resume(tid)); assert(p.awakened==tid);
 assert(d.critical_enter()==9); d.critical_exit(9);
 assert(shizuku_freertos_compat_bind(&d,1,0,100)==pdPASS);
 assert(p.route_handler==1&&p.route_method==0&&p.route_hz==100&&p.route_domain==&d);
 assert(xTaskGetSchedulerState()==taskSCHEDULER_NOT_STARTED);
 shizuku_freertos_compat_set_scheduler_state(taskSCHEDULER_RUNNING);
 assert(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING);
 shizuku_freertos_compat_set_clock(&test_clock_us);
 shizuku::freertos_compat::shizuku_port::task_start clock_req{
   shizuku::freertos_compat::shizuku_port::kReadClock,0,0,0,nullptr,nullptr};
 assert(shizuku_freertos_compat_dispatch(reinterpret_cast<uintptr_t>(&clock_req))==1234567);
 TaskHandle_t api_task=nullptr;
 assert(xTaskCreate([](void*){},"abi_task",128,nullptr,4,&api_task)==pdPASS);
 assert(api_task&&uxTaskPriorityGet(api_task)==4);
 const uint32_t api_task_id=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(api_task));
 p.set_task(api_task_id);
 uint32_t api_previous=0,api_notification=0;
 assert(xTaskGenericNotify(api_task,0,0x5,eSetBits,&api_previous)==pdPASS&&api_previous==0);
 assert(xTaskGenericNotifyWait(0,0x1,0x4,&api_notification,0)==pdPASS&&api_notification==0x5);
 assert(xTaskGenericNotify(api_task,0,9,eSetValueWithOverwrite,nullptr)==pdPASS);
 assert(xTaskGenericNotifyStateClear(api_task,0)==pdPASS);
 assert(xTaskGenericNotifyWait(0,0,0,&api_notification,0)==pdFAIL);
 p.set_task(1);
 test_port::isr=true; BaseType_t notify_woken=pdFALSE;
 assert(xTaskGenericNotifyFromISR(api_task,0,0,eIncrement,nullptr,&notify_woken)==pdPASS);
 assert(notify_woken==pdFALSE);
 vTaskGenericNotifyGiveFromISR(api_task,0,&notify_woken);
 test_port::isr=false; p.set_task(api_task_id);
 assert(ulTaskGenericNotifyTake(0,pdTRUE,0)==11);
 assert(xTaskGenericNotify(api_task,0,0xf,eSetValueWithOverwrite,nullptr)==pdPASS);
 assert(ulTaskGenericNotifyValueClear(api_task,0,0x3)==0xf);
 assert(ulTaskGenericNotifyTake(0,pdTRUE,0)==0xc);
 QueueHandle_t cq=xQueueCreate(2,sizeof(in)); assert(cq);
 assert(pdFALSE==errQUEUE_EMPTY);
 assert(xQueueIsQueueEmptyFromISR(cq)==pdTRUE);
 assert(uxQueueMessagesWaiting(cq)==0);
 test_port::isr=true;
 assert(xQueueIsQueueEmptyFromISR(cq)==pdTRUE);
 test_port::isr=false;
 out=0xa55aa55au;
 assert(xQueueReceive(cq,&out,0)==errQUEUE_EMPTY);
 assert(out==0xa55aa55au);
 assert(xQueueIsQueueEmptyFromISR(cq)==pdTRUE&&uxQueueMessagesWaiting(cq)==0);
 assert(xQueueSendToBack(cq,&in,0)==pdTRUE); out=0;
 assert(xQueueIsQueueEmptyFromISR(cq)==pdFALSE&&uxQueueMessagesWaiting(cq)==1);
 assert(xQueueReceive(cq,&out,0)==pdTRUE&&out==in);
 assert(xQueueIsQueueEmptyFromISR(cq)==pdTRUE&&uxQueueMessagesWaiting(cq)==0);
 QueueHandle_t event_queue=xQueueCreate(2,sizeof(void *)); assert(event_queue);
 void *event_in=&in,*event_out=nullptr;
 assert(xQueueSendToBack(event_queue,&event_in,0)==pdTRUE);
 assert(xQueueReceive(event_queue,&event_out,0)==pdTRUE&&event_out==event_in);
 // If an ISR fills the last slot after wait() observes space but before the
 // sender enters its critical section, the sender must retry/fail rather than
 // overrun the ring or overwrite the queued pointer.
 const int raced_queue=d.create_queue(1,sizeof(in)); assert(raced_queue>=0);
 uint32_t racing_item=77,retained_item=0;
 p.irq_save_calls=0; p.irq_save_hook_at=2;
 p.on_irq_save=[&]{assert(d.queue_send(raced_queue,&racing_item,0));};
 assert(!d.queue_send(raced_queue,&in,0));
 p.irq_save_hook_at=0; p.irq_save_calls=0;
 assert(d.queue_receive(raced_queue,&retained_item,0)&&retained_item==racing_item);
 SemaphoreHandle_t cs=xSemaphoreCreateBinary(); assert(cs);
 sync_snapshot semaphore_state{};
 const int cs_id=static_cast<int>(reinterpret_cast<uintptr_t>(cs)-1u);
 assert(d.object_snapshot(cs_id,&semaphore_state));
 assert(semaphore_state.kind==2&&semaphore_state.raw_handle==static_cast<uint32_t>(reinterpret_cast<uintptr_t>(cs)));
 assert(xSemaphoreGive(cs)==pdTRUE&&xSemaphoreTake(cs,0)==pdTRUE);
 // IDF v6.1 expands xSemaphoreGive to xQueueGenericSend; verify the wrapped
 // generic path releases both nonrecursive mutex and counting semaphore.
 QueueHandle_t libc_mutex=xQueueCreateMutex(queueQUEUE_TYPE_MUTEX); assert(libc_mutex);
 int libc_mutex_id=-1;
 for(size_t i=0;i<kMaxObjects;i++) {
   sync_snapshot candidate{};
   if(d.object_snapshot(static_cast<int>(i),&candidate)&&
      candidate.raw_handle==static_cast<uint32_t>(reinterpret_cast<uintptr_t>(libc_mutex)))
     libc_mutex_id=static_cast<int>(i);
 }
 assert(libc_mutex_id>=0);
 assert(xQueueSemaphoreTake(libc_mutex,0)==pdTRUE);
 assert(xQueueGenericSend(libc_mutex,nullptr,0,queueSEND_TO_BACK)==pdTRUE);
 sync_snapshot libc_mutex_state{};
 assert(d.object_snapshot(libc_mutex_id,&libc_mutex_state));
 assert(libc_mutex_state.recursive==0&&libc_mutex_state.owner==UINT32_MAX&&
        libc_mutex_state.take_count==1&&libc_mutex_state.give_count==1);
 QueueHandle_t libc_recursive=xQueueCreateMutex(queueQUEUE_TYPE_RECURSIVE_MUTEX); assert(libc_recursive);
 int libc_recursive_id=-1;
 for(size_t i=0;i<kMaxObjects;i++) {
   sync_snapshot candidate{};
   if(d.object_snapshot(static_cast<int>(i),&candidate)&&
      candidate.raw_handle==static_cast<uint32_t>(reinterpret_cast<uintptr_t>(libc_recursive)))
     libc_recursive_id=static_cast<int>(i);
 }
 assert(libc_recursive_id>=0);
 assert(d.object_snapshot(libc_recursive_id,&libc_mutex_state)&&libc_mutex_state.recursive==1);
 assert(xQueueTakeMutexRecursive(libc_recursive,0)==pdTRUE);
 assert(xQueueGiveMutexRecursive(libc_recursive)==pdTRUE);
 assert(d.object_snapshot(libc_recursive_id,&libc_mutex_state));
 assert(libc_mutex_state.owner==UINT32_MAX&&libc_mutex_state.give_count==1);
 assert(xQueueGenericSend(cs,nullptr,0,queueSEND_TO_BACK)==pdTRUE);
 assert(xSemaphoreTake(cs,0)==pdTRUE);
 SemaphoreHandle_t rec_api=xSemaphoreCreateRecursiveMutex(); assert(rec_api);
 assert(xSemaphoreTakeRecursive(rec_api,0)==pdTRUE);
 assert(xSemaphoreTake(rec_api,2)==pdFALSE); // nonrecursive API does not recurse
 assert(xSemaphoreTakeRecursive(rec_api,0)==pdTRUE);
 assert(xSemaphoreGiveRecursive(rec_api)==pdTRUE);
 assert(xSemaphoreGiveRecursive(rec_api)==pdTRUE);
 test_port::isr=true; assert(xPortInIsrContext()==pdTRUE);
 BaseType_t woken=pdFALSE; assert(xSemaphoreGiveFromISR(cs,&woken)==pdTRUE);
 assert(woken==pdFALSE&&xSemaphoreTakeFromISR(cs,&woken)==pdTRUE);
 assert(xQueueSendFromISR(cq,&in,&woken)==pdTRUE);
 assert(xQueueReceiveFromISR(cq,&out,&woken)==pdTRUE&&out==in);
 assert(woken==pdFALSE);
 test_port::isr=false;
 portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
 portENTER_CRITICAL(&mux); portEXIT_CRITICAL(&mux);
 EventGroupHandle_t eg=xEventGroupCreate(); assert(eg);
 assert(xEventGroupSetBits(eg,0x3)==0x3);
 assert(xEventGroupWaitBits(eg,0x1,pdFALSE,pdTRUE,0)==0x3);
 assert(xEventGroupWaitBits(eg,0x6,pdFALSE,pdTRUE,0)==0x3); // returns observed bits on timeout
 assert(xEventGroupWaitBits(eg,0x3,pdTRUE,pdTRUE,0)==0x3);
 assert(xEventGroupClearBits(eg,0x1)==0);
 p.set_task(tid); p.on_park=[&]{p.set_task(3);assert(xEventGroupSetBits(eg,0x4)==0x4);p.set_task(tid);};
 assert(xEventGroupWaitBits(eg,0x4,pdTRUE,pdTRUE,20)==0x4);
 g_expected_timer_id=reinterpret_cast<void *>(uintptr_t{0x1234});
 g_timer_callback_count=0; TimerHandle_t timer=xTimerCreate("npl",3,pdTRUE,g_expected_timer_id,&timer_callback); assert(timer);
 assert(!xTimerIsTimerActive(timer)&&xTimerStart(timer,0)==pdPASS);
 p.now+=3; (void)xTaskGetTickCount(); assert(g_timer_callback_count==1&&xTimerIsTimerActive(timer));
 assert(xTimerGetExpiryTime(timer)==p.now+3);
 assert(xTimerChangePeriod(timer,2,0)==pdPASS&&xTimerGetExpiryTime(timer)==p.now+2);
 p.now+=2; (void)xTaskGetTickCount(); assert(g_timer_callback_count==2);
 assert(xTimerStop(timer,0)==pdPASS&&!xTimerIsTimerActive(timer));
 assert(xTimerStart(timer,0)==pdPASS); p.now+=3; (void)xTaskGetTickCount();
 assert(g_timer_callback_count==3&&xTimerIsTimerActive(timer));
 assert(xTimerDelete(timer,0)==pdPASS&&!xTimerIsTimerActive(timer));
 TimerHandle_t one_shot=xTimerCreate("one",4,pdFALSE,g_expected_timer_id,&timer_callback); assert(one_shot);
 test_port::isr=true; BaseType_t timer_woken=pdTRUE;
 assert(xTimerStartFromISR(one_shot,&timer_woken)==pdPASS&&timer_woken==pdFALSE);
 assert(xTimerStopFromISR(one_shot,&timer_woken)==pdPASS);
 assert(xTimerChangePeriodFromISR(one_shot,2,&timer_woken)==pdPASS);
 assert(xTimerResetFromISR(one_shot,&timer_woken)==pdPASS);
 test_port::isr=false; p.now+=2; (void)xTaskGetTickCount();
 assert(g_timer_callback_count==4&&!xTimerIsTimerActive(one_shot));
 assert(xTimerDelete(one_shot,0)==pdPASS);
 std::atomic<bool> received{false};
 std::thread consumer([&]{p.set_task(tid);received=d.queue_receive(q,&out,1000);});
 { std::unique_lock<std::mutex> lock(p.m); assert(p.cv.wait_for(lock,std::chrono::seconds(1),[&]{return p.parked;})); }
 p.set_task(4); assert(d.queue_send(q,&in,0)); consumer.join();
 assert(received&&out==in&&p.awakened==tid);
 uint32_t low=0,high=0;
 assert(d.task_create([](void*){},"low",256,nullptr,2,&low));
 assert(d.task_create([](void*){},"high",256,nullptr,8,&high));
 std::atomic<bool> low_done{false},high_done{false}; uint32_t low_value=0,high_value=0;
 std::thread low_thread([&]{p.set_task(low);assert(d.queue_receive(q,&low_value,kForever));low_done=true;});
 std::thread high_thread([&]{p.set_task(high);assert(d.queue_receive(q,&high_value,kForever));high_done=true;});
 { std::unique_lock<std::mutex> lock(p.m); assert(p.cv.wait_for(lock,std::chrono::seconds(1),[&]{return p.parked_ids.size()==2;})); }
 p.set_task(5); assert(d.queue_send(q,&in,0));
 while(!high_done.load())std::this_thread::yield();
 assert(p.awakened==high); assert(d.queue_send(q,&in,0));
 low_thread.join();high_thread.join(); assert(low_done&&low_value==in&&high_value==in);
 uint32_t isr_high=0; assert(d.task_create([](void*){},"isr_high",256,nullptr,9,&isr_high));
 QueueHandle_t isr_queue=xQueueCreate(1,sizeof(in)); assert(isr_queue);
 std::atomic<bool> isr_received{false}; uint32_t isr_value=0;
 std::thread isr_consumer([&]{p.set_task(isr_high);isr_received=xQueueReceive(isr_queue,&isr_value,kForever)==pdTRUE;});
 { std::unique_lock<std::mutex> lock(p.m); assert(p.cv.wait_for(lock,std::chrono::seconds(1),[&]{return p.parked_ids.contains(isr_high);})); }
 p.set_task(tid); test_port::isr=true; BaseType_t isr_woken=pdFALSE;
 assert(xQueueSendFromISR(isr_queue,&in,&isr_woken)==pdTRUE&&isr_woken==pdTRUE);
 portYIELD_FROM_ISR(isr_woken);
 assert(shizuku::freertos_compat::shizuku_port::pending_yield_from_isr());
 test_port::isr=false; isr_consumer.join(); assert(isr_received&&isr_value==in);
 shizuku::freertos_compat::shizuku_port syscall_port;
 domain syscall_domain(syscall_port); syscall_observation observed; g_syscall_observation=&observed;
 g_hook_port=&syscall_port;
 shizuku::freertos_compat::shizuku_port::set_clock(&test_clock_us);
 shizuku::freertos_compat::shizuku_port::set_host_syscall_hook(&host_shizuku_syscall);
 assert(syscall_domain.configure_backend(41,0,100));
 uint32_t syscall_task=0;
 assert(syscall_domain.task_create(&task_yield_probe,"syscall_task",128,nullptr,9,&syscall_task));
 assert(syscall_task==77&&syscall_port.now_ticks()==123);
 assert(syscall_port.run_for(syscall_task,160000));
 assert(g_yield_context_restored&&syscall_domain.task_priority(syscall_task)==9);
 syscall_port.park_until(124); syscall_port.yield(); syscall_port.sleep_us(17);
 bool saw_call=false,saw_spawn=false,saw_run=false,saw_sleep=false,saw_yield=false;
 for(size_t i=0;i<observed.count;i++) {
   saw_call|=observed.calls[i]==shizuku::object_api::CALL_METHOD;
   saw_spawn|=observed.calls[i]==shizuku::object_api::SPAWN;
   saw_run|=observed.calls[i]==shizuku::object_api::RUN_FOR;
   saw_sleep|=observed.calls[i]==shizuku::object_api::SLEEP_US;
   saw_yield|=observed.calls[i]==shizuku::object_api::YIELD;
 }
 assert(saw_call&&saw_spawn&&saw_run&&saw_sleep&&saw_yield);
 // Non-LIFO wakeup regression test for sleep_us saturation bug (form (2)).
 // Two threads (A, B) interleave sleep_us calls such that A enters SLEEP_US,
 // then B enters SLEEP_US, then A exits, then B exits. In the buggy code,
 // restoring stale local depth to g_sleep_depth ratchets depth up by 1 per round,
 // saturating at 16 and causing subsequent sleep_us calls to be skipped via yield().
 // In the fixed code, all 20 rounds issue SLEEP_US for both threads without skipping.
 g_non_lifo_test.reset();
 shizuku::freertos_compat::shizuku_port::set_host_syscall_hook(&non_lifo_sleep_syscall_hook);

 std::thread th_a([&]{
   for (uint32_t i = 0; i < 20; ++i) {
     {
       std::unique_lock<std::mutex> lock(g_non_lifo_test.m);
       g_non_lifo_test.cv.wait_for(lock, std::chrono::seconds(2), [&]{
         return g_non_lifo_test.state == non_lifo_sleep_test::START_A || g_non_lifo_test.failed;
       });
       if (g_non_lifo_test.failed) break;
     }
     syscall_port.sleep_us(10);
     {
       std::unique_lock<std::mutex> lock(g_non_lifo_test.m);
       if (g_non_lifo_test.failed || g_non_lifo_test.state != non_lifo_sleep_test::B_IN_HOOK) {
         // Buggy behavior: SLEEP_US syscall hook skipped or aborted
         g_non_lifo_test.failed = true;
         g_non_lifo_test.state = non_lifo_sleep_test::ABORTED;
         g_non_lifo_test.cv.notify_all();
         break;
       }
       g_non_lifo_test.state = non_lifo_sleep_test::A_RETURNED;
       g_non_lifo_test.cv.notify_all();
     }
   }
 });

 std::thread th_b([&]{
   for (uint32_t i = 0; i < 20; ++i) {
     {
       std::unique_lock<std::mutex> lock(g_non_lifo_test.m);
       g_non_lifo_test.cv.wait_for(lock, std::chrono::seconds(2), [&]{
         return g_non_lifo_test.state == non_lifo_sleep_test::A_IN_HOOK || g_non_lifo_test.failed;
       });
       if (g_non_lifo_test.failed) break;
     }
     syscall_port.sleep_us(10);
     {
       std::unique_lock<std::mutex> lock(g_non_lifo_test.m);
       if (g_non_lifo_test.failed || g_non_lifo_test.state != non_lifo_sleep_test::A_RETURNED) {
         // Buggy behavior: SLEEP_US syscall hook skipped or aborted
         g_non_lifo_test.failed = true;
         g_non_lifo_test.state = non_lifo_sleep_test::ABORTED;
         g_non_lifo_test.cv.notify_all();
         break;
       }
       g_non_lifo_test.state = (i + 1 < 20) ? non_lifo_sleep_test::START_A : non_lifo_sleep_test::ROUND_DONE;
       g_non_lifo_test.cv.notify_all();
     }
   }
 });

 th_a.join();
 th_b.join();

 shizuku::freertos_compat::shizuku_port::set_host_syscall_hook(&host_shizuku_syscall);
 assert(!g_non_lifo_test.failed);
 assert(g_non_lifo_test.a_calls == 20);
 assert(g_non_lifo_test.b_calls == 20);
 // Identity follows the running thread, not the lender-rewritten global: a mutex
 // taken by thread 3 while thread 2 sleeps must be owned by 3 (not by id 0) and
 // must be releasable by 3.
 {
   shizuku::freertos_compat::shizuku_port id_port; domain id_dom(id_port);
   const int id_mutex=id_dom.create_mutex(true); assert(id_mutex>=0);
   g_identity={&id_port,&id_dom,id_mutex};
#if defined(SHIZUKU_PORT_STACK_ID)
   shizuku::freertos_compat::shizuku_port::note_task_stack(2,0x20ff0);
   shizuku::freertos_compat::shizuku_port::note_task_stack(3,0x21ff0);
   shizuku::freertos_compat::shizuku_port::set_stack_probe(&identity_sp);
#endif
   shizuku::freertos_compat::shizuku_port::set_host_syscall_hook(&identity_syscall);
   assert(id_port.run_for(2,1000));
   assert(g_identity.ran&&g_identity.took);
   assert(id_dom.mutex_owner(id_mutex)==3); // old code: owner is 0 (the lender id)
   g_identity.sp=0x21800;
#if defined(SHIZUKU_PORT_STACK_ID)
   assert(id_port.current_task()==3);
   assert(id_dom.mutex_give(id_mutex)&&id_dom.mutex_owner(id_mutex)==0);
   shizuku::freertos_compat::shizuku_port::set_stack_probe(nullptr);
#endif
   shizuku::freertos_compat::shizuku_port::set_host_syscall_hook(&host_shizuku_syscall);
 }
 task_entry_probe started{&syscall_port,syscall_task};
 shizuku::freertos_compat::shizuku_port::task_start start_req{
   shizuku::freertos_compat::shizuku_port::kStartTask,syscall_task,9,128,&mark_task_started,&started};
 assert(shizuku_freertos_compat_dispatch(reinterpret_cast<uintptr_t>(&start_req))==0);
 assert(started.started&&syscall_port.current_task()==0);
 shizuku::freertos_compat::shizuku_port::enter_isr(); assert(syscall_port.in_isr());
 shizuku::freertos_compat::shizuku_port::leave_isr(); assert(!syscall_port.in_isr());
 // The radio path creates ordinary PLAIN task objects, then spawns method 0.
 // It does not issue CALL_METHOD to a secondary HANDLER.
 shizuku::freertos_compat::shizuku_port plain_port;
 domain plain_domain(plain_port);
 syscall_observation plain_observed; g_syscall_observation=&plain_observed;
 g_hook_port=&plain_port; g_plain_task_route=true;
#if defined(SHIZUKU_ESP32_C6)
 g_plain_kernel_thread_id=7; // Deliberately differs from the PLAIN object ID (48).
 g_plain_spawn_result=7;
#else
 g_plain_kernel_thread_id=48; // Preserve the pre-C6 task/object ID expectation.
 g_plain_spawn_result=78; // Confirm legacy PLAIN ignores the returned thread ID.
#endif
 shizuku::freertos_compat::shizuku_port::set_host_syscall_hook(&host_shizuku_syscall);
 assert(plain_domain.configure_backend(0,48,100));
 uint32_t plain_task=0;
 assert(plain_domain.task_create(&task_yield_probe,"plain_task",128,nullptr,5,&plain_task));
 assert(plain_task==g_plain_kernel_thread_id&&g_plain_object_id==48);
#if defined(SHIZUKU_ESP32_C6)
 assert(g_plain_spawn_result==plain_task&&plain_task!=g_plain_object_id);
#else
 assert(g_plain_spawn_result!=plain_task&&plain_task==g_plain_object_id);
#endif
 assert(g_spawn_packet&&g_spawn_packet->id==plain_task);
 bool saw_create=false,saw_plain_spawn=false,saw_nested_call=false;
 for(size_t i=0;i<plain_observed.count;i++) {
   saw_create|=plain_observed.calls[i]==shizuku::object_api::CREATE_OBJECT;
   saw_plain_spawn|=plain_observed.calls[i]==shizuku::object_api::SPAWN;
   saw_nested_call|=plain_observed.calls[i]==shizuku::object_api::CALL_METHOD;
 }
 assert(saw_create&&saw_plain_spawn&&!saw_nested_call);
 assert(plain_port.run_for(plain_task,16000));
 assert(g_task_started_id==plain_task&&plain_port.current_task()==0);
#if defined(SHIZUKU_ESP32_C6)
 assert(g_run_for_raw.task_id==plain_task&&g_run_for_raw.a0_error==0&&
        g_run_for_raw.a1_value==0); // Normal grant expiration is not an API error.
#endif
 g_plain_task_route=false;
 shizuku::freertos_compat::shizuku_port::set_host_syscall_hook(nullptr);

#if defined(SHIZUKU_ESP32_C6)
 shizuku::freertos_compat::shizuku_port raw_port;
 shizuku::freertos_compat::shizuku_port::set_host_syscall_hook(&raw_run_for_syscall);
 g_mock_run_for_error=0; g_mock_run_for_value=0;
 assert(raw_port.run_for(12,1600));
 assert(g_run_for_raw.task_id==12&&g_run_for_raw.a0_error==0&&g_run_for_raw.a1_value==0);
 g_mock_run_for_error=9; g_mock_run_for_value=0;
 assert(!raw_port.run_for(12,1600));
 assert(g_run_for_raw.a0_error==9&&g_run_for_raw.a1_value==0);
 g_mock_run_for_error=14; g_mock_run_for_value=0;
 assert(!raw_port.run_for(12,1600));
 assert(g_run_for_raw.a0_error==14&&g_run_for_raw.a1_value==0);
 g_mock_run_for_error=0;
 for(uint32_t i=0;i<1024;++i)assert(raw_port.run_for(12,1600));
 assert(g_c6_stack_probe_count==1&&g_c6_stack_probe_id==12);
 shizuku::freertos_compat::shizuku_port::set_host_syscall_hook(nullptr);
#endif

 // The compat scheduler explicitly grants equal-priority READY tasks in
 // round-robin order; the hardware SYSTIMER alone does not launch these grants.
 test_port schedule_port; domain schedule_domain(schedule_port);
 uint32_t rr_a=0,rr_b=0;
 assert(schedule_domain.task_create([](void*){},"rr_a",256,nullptr,5,&rr_a));
 assert(schedule_domain.task_create([](void*){},"rr_b",256,nullptr,5,&rr_b));
 uint32_t grants[3]{}; size_t grant_count=0;
 schedule_port.on_run_for=[&](uint32_t id,uint32_t cycles) {
   assert(cycles==16000);
   grants[grant_count++]=id;
   if(grant_count==3)throw stop_scheduler_test{};
 };
 try { schedule_domain.run_scheduler(16000); } catch(const stop_scheduler_test &) {}
 assert(grant_count==3&&grants[0]==rr_a&&grants[1]==rr_b&&grants[2]==rr_a);

}
