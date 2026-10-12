#include "compat.hpp"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
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
using namespace shizuku::freertos_compat;

struct test_port final : port {
  uint64_t now=0; uint32_t awakened=0;
  std::function<void()> on_park;
  std::mutex m; std::condition_variable cv; bool parked=false, token=false;
  std::set<uint32_t> parked_ids, tokens;
  uint32_t priorities[32]{};
  uint32_t route_handler=0,route_method=0,route_hz=0;
  domain *route_domain=nullptr;
  static thread_local uint32_t task;
  static thread_local bool isr;
  static std::atomic<uint32_t> next_task;
  void set_task(uint32_t id){task=id;}
  bool configure_route(uint32_t h,uint32_t method,uint32_t hz,void *d) override{route_handler=h;route_method=method;route_hz=hz;route_domain=static_cast<domain*>(d);return h&&d&&hz;}
  uint64_t now_ticks() override{return now;}
  uint64_t now_us() override{return now*10000u;}
  uint32_t current_task() override{return task;}
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
  uint32_t irq_save() override{return 9;}
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
void task_yield_probe(void*) {
  const auto id=g_hook_port->current_task();
  g_hook_port->yield();
  const bool after_yield=g_hook_port->current_task()==id;
  g_hook_port->sleep_us(11);
  g_yield_context_restored=after_yield&&g_hook_port->current_task()==id;
}
uintptr_t host_shizuku_syscall(shizuku::object_api api,uintptr_t a1,uintptr_t a2,uintptr_t a3,uintptr_t *value) {
  assert(g_syscall_observation&&g_syscall_observation->count<32);
  g_syscall_observation->calls[g_syscall_observation->count++]=api;
  if(api==shizuku::object_api::CALL_METHOD) {
    assert(a1==41&&a2==0);
    if(value)*value=shizuku_freertos_compat_dispatch(a3);
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
int main(){
 test_port p; domain d(p); uint32_t tid=0;
 assert(d.task_create([](void*){},"worker",256,nullptr,3,&tid)&&tid==2);
 assert(!d.task_create(nullptr,"bad",256,nullptr,1,nullptr));
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
 const int m=d.create_mutex(true); assert(m>=0);
 assert(d.mutex_take(m,0)); assert(d.mutex_take(m,0)); assert(d.mutex_give(m)); assert(d.mutex_give(m));
 assert(!d.mutex_give(m));
 assert(d.task_suspend(tid)); assert(d.task_resume(tid)); assert(p.awakened==tid);
 assert(d.critical_enter()==9); d.critical_exit(9);
 assert(shizuku_freertos_compat_bind(&d,1,0,100)==pdPASS);
 assert(p.route_handler==1&&p.route_method==0&&p.route_hz==100&&p.route_domain==&d);
 shizuku_freertos_compat_set_clock(&test_clock_us);
 shizuku::freertos_compat::shizuku_port::task_start clock_req{
   shizuku::freertos_compat::shizuku_port::kReadClock,0,0,0,nullptr,nullptr};
 assert(shizuku_freertos_compat_dispatch(reinterpret_cast<uintptr_t>(&clock_req))==1234567);
 TaskHandle_t api_task=nullptr;
 assert(xTaskCreate([](void*){},"abi_task",128,nullptr,4,&api_task)==pdPASS);
 assert(api_task&&uxTaskPriorityGet(api_task)==4);
 QueueHandle_t cq=xQueueCreate(2,sizeof(in)); assert(cq);
 assert(xQueueSendToBack(cq,&in,0)==pdTRUE); out=0;
 assert(xQueueReceive(cq,&out,0)==pdTRUE&&out==in);
 SemaphoreHandle_t cs=xSemaphoreCreateBinary(); assert(cs);
 assert(xSemaphoreGive(cs)==pdTRUE&&xSemaphoreTake(cs,0)==pdTRUE);
 test_port::isr=true; assert(xPortInIsrContext()==pdTRUE);
 BaseType_t woken=pdFALSE; assert(xSemaphoreGiveFromISR(cs,&woken)==pdTRUE);
 assert(woken==pdFALSE&&xSemaphoreTakeFromISR(cs,&woken)==pdTRUE);
 assert(xQueueSendFromISR(cq,&in,&woken)==pdTRUE);
 assert(xQueueReceiveFromISR(cq,&out,&woken)==pdTRUE&&out==in);
 assert(woken==pdFALSE);
 test_port::isr=false;
 portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
 portENTER_CRITICAL(&mux); portEXIT_CRITICAL(&mux);
 assert(xEventGroupCreate()==nullptr); // declared but deliberately unsupported
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
 task_entry_probe started{&syscall_port,syscall_task};
 shizuku::freertos_compat::shizuku_port::task_start start_req{
   shizuku::freertos_compat::shizuku_port::kStartTask,syscall_task,9,128,&mark_task_started,&started};
 assert(shizuku_freertos_compat_dispatch(reinterpret_cast<uintptr_t>(&start_req))==0);
 assert(started.started&&syscall_port.current_task()==0);
 shizuku::freertos_compat::shizuku_port::enter_isr(); assert(syscall_port.in_isr());
 shizuku::freertos_compat::shizuku_port::leave_isr(); assert(!syscall_port.in_isr());
 shizuku::freertos_compat::shizuku_port::set_host_syscall_hook(nullptr);
}
