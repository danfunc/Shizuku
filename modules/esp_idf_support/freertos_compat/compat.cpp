#include "compat.hpp"
#include <cstring>
#include <cstdio>

namespace shizuku::freertos_compat {
bool domain::task_create(void (*entry)(void *), const char *, uint32_t stack,
                         void *arg, uint32_t priority, uint32_t *handle) {
  if (!entry || !stack || p_.in_isr()) return false;
  for (auto &t : tasks_) if (t.status == state::unused || t.status == state::done) {
    uint32_t id = 0;
    if (!p_.spawn(entry, arg, stack, priority, &id) || !id) return false;
    t = {state::ready, id, priority, UINT64_MAX, 0, entry, arg};
    if (handle) *handle = id;
    return true;
  }
  return false;
}
void domain::task_delay(tick_t ticks) {
  if (p_.in_isr()) return;
  if (ticks == 0) { p_.yield(); return; }
  const auto deadline=ticks==kForever?UINT64_MAX:p_.now_ticks()+ticks;
  while(p_.now_ticks()<deadline) { block_current(deadline); p_.park_until(deadline); }
}
bool domain::task_suspend(uint32_t id) {
  if (p_.in_isr()) return false;
  for (auto &t : tasks_) if (t.id == id && t.status != state::done) { t.status = state::suspended; return true; }
  return false;
}
bool domain::task_resume(uint32_t id, bool from_isr, bool *higher) {
  if (from_isr != p_.in_isr()) return false;
  for (auto &t : tasks_) if (t.id == id && t.status == state::suspended) { t.status = state::ready; const bool hp=p_.wake(id);if(higher&&hp)*higher=true;return true; }
  return false;
}
uint32_t domain::task_priority(uint32_t id) const {
  for (const auto &t : tasks_) if (t.id == id && t.status != state::unused) return t.priority;
  return 0;
}
uint64_t domain::tick_count() { return p_.now_ticks(); }
uint64_t domain::time_us() { return p_.now_us(); }
bool domain::isr_context() { return p_.in_isr(); }
bool domain::configure_backend(uint32_t handler, uint32_t method, uint32_t tick_hz) {
  if(!handler||!tick_hz)return false;
  if(!p_.configure_route(handler, method, tick_hz, this))return false;
  tick_hz_=tick_hz; return true;
}
void domain::block_current(uint64_t deadline) {
  const uint32_t id=p_.current_task();
  for(auto &t:tasks_) if(t.id==id&&t.status!=state::done) {
    t.status=state::blocked; t.wake_deadline=deadline;
    t.eligible_after_us=p_.now_us()+100; p_.block_task(id,deadline); return;
  }
}
void domain::make_ready(uint32_t id) {
  for(auto &t:tasks_) if(t.id==id&&t.status==state::blocked) {
    t.status=state::ready; t.wake_deadline=UINT64_MAX; p_.make_ready(id); return;
  }
}
void domain::finish_current_task() {
  const uint32_t id=p_.current_task();
  for(auto &t:tasks_) if(t.id==id) { t.status=state::done; t.wake_deadline=UINT64_MAX; break; }
  p_.task_finished(id); p_.exit_thread();
}
void domain::run_scheduler(uint32_t grant_cycles) {
  if(!grant_cycles)return;
  for(;;) {
    (void)p_.consume_reschedule_pending(); // ISR requests are applied at this grant boundary.
    const uint64_t now=p_.now_ticks();
    const uint64_t now_us=p_.now_us();
    task_control *best=nullptr; size_t best_index=0; uint64_t deadline=UINT64_MAX;
    for(size_t step=0;step<kMaxTasks;step++) {
      const size_t i=(scheduler_cursor_+step)%kMaxTasks; auto &t=tasks_[i];
      if(t.status==state::blocked) {
        if(t.wake_deadline<=now) { t.status=state::ready; t.wake_deadline=UINT64_MAX; p_.make_ready(t.id); }
        else if(t.wake_deadline<deadline) deadline=t.wake_deadline;
      }
      if(t.status==state::ready&&t.eligible_after_us<=now_us&&(!best||t.priority>best->priority)) { best=&t;best_index=i; }
    }
    if(best) {
      scheduler_cursor_=(best_index+1)%kMaxTasks;
      best->status=state::running;
      if(!p_.run_for(best->id,grant_cycles)) { best->status=state::suspended; return; }
      if(best->status==state::running) best->status=state::ready;
      continue;
    }
    uint64_t idle_us=100; // IRQ wake is observed on the next bounded idle poll.
    if(deadline!=UINT64_MAX&&deadline>now) {
      const auto until_deadline=((deadline-now)*1000000u+tick_hz_-1)/tick_hz_;
      if(until_deadline<idle_us)idle_us=until_deadline;
    }
    for(const auto &t:tasks_)if(t.status==state::ready&&t.eligible_after_us>now_us) {
      const auto until_eligible=t.eligible_after_us-now_us;
      if(until_eligible<idle_us)idle_us=until_eligible;
    }
    // Bound idle latency for ISR wakeups; the target Port tracks the lender
    // context while SLEEP_US internally schedules other Shizuku threads.
    p_.sleep_us(idle_us?idle_us:1);
  }
}
sync_object *domain::get(int h, object_kind k) {
  if (h < 0 || static_cast<size_t>(h) >= kMaxObjects) return nullptr;
  auto &o = objects_[h]; return o.used && o.kind == k ? &o : nullptr;
}
int domain::create_queue(size_t cap, size_t sz) {
  if (p_.in_isr() || !cap || !sz || cap > SIZE_MAX / sz) return -1;
  for (size_t i=0;i<kMaxObjects;i++) if (!objects_[i].used) {
    auto &o=objects_[i]; o={}; o.used=true; o.kind=object_kind::queue;
    o.capacity=cap; o.item_size=sz; o.storage=new uint8_t[cap*sz];
    if (!o.storage) { o.used=false; return -1; } return static_cast<int>(i);
  }
  return -1;
}
int domain::create_semaphore(uint32_t limit, uint32_t initial) {
  if (p_.in_isr() || !limit || initial>limit) return -1;
  for(size_t i=0;i<kMaxObjects;i++) if(!objects_[i].used) { auto &o=objects_[i];o={};o.used=true;o.kind=object_kind::semaphore;o.limit=limit;o.count=initial;return (int)i; } return -1;
}
int domain::create_mutex(bool recursive) {
  if (p_.in_isr()) return -1;
  for(size_t i=0;i<kMaxObjects;i++) if(!objects_[i].used) { auto &o=objects_[i];o={};o.used=true;o.kind=object_kind::mutex;o.limit=recursive?1:0;return (int)i; } return -1;
}
bool domain::add_waiter(sync_object &o, bool for_data) {
  const size_t side=for_data?1:0;
  if (o.waiter_count[side] == kMaxWaiters) return false;
  uint32_t id=p_.current_task(), pri=0;
  for(auto &t:tasks_) if(t.id==id) pri=t.priority;
  o.waiters[side][o.waiter_count[side]++]={id,pri,sequence_++}; return true;
}
bool domain::remove_waiter(sync_object &o, uint32_t id, bool for_data) {
  const size_t side=for_data?1:0;
  for(size_t i=0;i<o.waiter_count[side];i++) if(o.waiters[side][i].task==id) {
    o.waiters[side][i]=o.waiters[side][--o.waiter_count[side]]; return true;
  }
  return false;
}
bool domain::wake_one(sync_object &o, bool for_data) {
  const size_t side=for_data?1:0;
  if(!o.waiter_count[side]) return false;
  size_t best=0;
  for(size_t i=1;i<o.waiter_count[side];i++) if(o.waiters[side][i].priority>o.waiters[side][best].priority || (o.waiters[side][i].priority==o.waiters[side][best].priority && o.waiters[side][i].sequence<o.waiters[side][best].sequence)) best=i;
  auto id=o.waiters[side][best].task; o.waiters[side][best]=o.waiters[side][--o.waiter_count[side]];
  const bool higher=p_.wake(id); make_ready(id); return higher;
}
bool domain::wait(sync_object &o, bool data, tick_t timeout, bool from_isr) {
  if(from_isr != p_.in_isr()) return false;
  if(from_isr && timeout) return false;
  const uint64_t end=timeout==kForever?UINT64_MAX:p_.now_ticks()+timeout;
  bool registered=false;
  while(data ? o.length==0 : o.length==o.capacity) {
    if(timeout==0 || p_.now_ticks()>=end) { if(registered)remove_waiter(o,p_.current_task(),data); return false; }
    if(!registered) { if(!add_waiter(o,data)) return false; registered=true; }
    block_current(end); p_.park_until(end);
    if(registered) remove_waiter(o,p_.current_task(),data);
    registered=false;
  }
  return true;
}
bool domain::queue_send(int h,const void *item,tick_t timeout,bool front,bool isr,bool *higher) {
  auto *o=get(h,object_kind::queue); if(!o||!item) return false;
  if(!wait(*o,false,timeout,isr)) return false;
  uint32_t irq=p_.irq_save(); size_t slot=front?(o->head+o->capacity-1)%o->capacity:(o->head+o->length)%o->capacity;
  std::memcpy(o->storage+slot*o->item_size,item,o->item_size); if(front)o->head=slot; ++o->length; p_.irq_restore(irq); const bool hp=wake_one(*o,true); if(higher&&hp)*higher=true; return true;
}
bool domain::queue_receive(int h,void *item,tick_t timeout,bool isr,bool *higher) {
  auto *o=get(h,object_kind::queue); if(!o||!item||!wait(*o,true,timeout,isr)) return false;
  uint32_t irq=p_.irq_save(); std::memcpy(item,o->storage+o->head*o->item_size,o->item_size); o->head=(o->head+1)%o->capacity; --o->length; p_.irq_restore(irq); const bool hp=wake_one(*o,false); if(higher&&hp)*higher=true; return true;
}
size_t domain::queue_messages(int h) const { return h>=0&&size_t(h)<kMaxObjects&&objects_[h].used?objects_[h].length:0; }
bool domain::semaphore_take(int h,tick_t timeout,bool isr) {
  auto *o=get(h,object_kind::semaphore); if(!o||isr!=p_.in_isr()||(isr&&timeout))return false;
  if(o->count){--o->count;return true;} if(timeout==0)return false;
  uint64_t end=timeout==kForever?UINT64_MAX:p_.now_ticks()+timeout;
  bool registered=false;
  do {
    if(o->count){--o->count;return true;}
    if(p_.now_ticks()>=end)break;
    if(!registered){if(!add_waiter(*o,true))return false;registered=true;}
    block_current(end); p_.park_until(end);
    if(registered)remove_waiter(*o,p_.current_task(),true);
    registered=false;
  } while(true);
  remove_waiter(*o,p_.current_task(),true); return false;
}
bool domain::semaphore_give(int h,bool isr,bool *higher) { auto *o=get(h,object_kind::semaphore);if(!o||isr!=p_.in_isr()||o->count==o->limit)return false;++o->count;const bool hp=wake_one(*o,true);if(higher&&hp)*higher=true;return true; }
bool domain::mutex_take(int h,tick_t timeout,bool isr) {
  auto *o=get(h,object_kind::mutex); if(!o||isr||p_.in_isr())return false;
  auto me=p_.current_task(); if(o->owner==me){if(!o->limit)return false;++o->recursion;return true;}
  uint64_t end=timeout==kForever?UINT64_MAX:p_.now_ticks()+timeout; bool registered=false;
  do {
    if(!o->owner){o->owner=me;o->recursion=1;return true;}
    if(timeout==0||p_.now_ticks()>=end)break;
    if(!registered){if(!add_waiter(*o,true))return false;registered=true;}
    block_current(end); p_.park_until(end); if(registered)remove_waiter(*o,me,true); registered=false;
  } while(true);
  remove_waiter(*o,me,true); return false;
}
bool domain::mutex_give(int h,bool isr) { auto *o=get(h,object_kind::mutex);if(!o||isr||p_.in_isr()||o->owner!=p_.current_task())return false;if(--o->recursion==0){o->owner=0;wake_one(*o,true);}return true; }
uint32_t domain::critical_enter(){return p_.irq_save();}
void domain::critical_exit(uint32_t k){p_.irq_restore(k);}
} // namespace shizuku::freertos_compat
