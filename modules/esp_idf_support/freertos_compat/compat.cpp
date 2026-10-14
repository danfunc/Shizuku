#include "compat.hpp"
#include <cstring>
#include <cstdio>

extern "C" void shizuku_esp_ble_note_sync_object(
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t)
    __attribute__((weak));
extern "C" void shizuku_esp_ble_note_queue_send(uint32_t, uint32_t, uint32_t)
    __attribute__((weak));
extern "C" void shizuku_esp_ble_note_queue_receive(uint32_t, uint32_t, uint32_t)
    __attribute__((weak));
extern "C" void shizuku_esp_ble_note_task_scheduled(uint32_t, uint32_t, uint32_t, uint32_t)
    __attribute__((weak));
#if defined(SHIZUKU_ESP32_C6)
extern "C" void shizuku_esp_ble_note_compat_progress(uint32_t, uint32_t, uint32_t)
    __attribute__((weak));
#endif

namespace shizuku::freertos_compat {
bool domain::task_create(void (*entry)(void *), const char *, uint32_t stack,
                         void *arg, uint32_t priority, uint32_t *handle) {
  if (!entry || !stack || p_.in_isr()) return false;
  for (auto &t : tasks_) if (t.status == state::unused || t.status == state::done) {
    uint32_t id = 0;
    if (!p_.spawn(entry, arg, stack, priority, &id) || !id) return false;
    t = {};
    t.status = state::ready;
    t.id = id;
    t.priority = priority;
    t.base_priority = priority;
    t.wake_deadline = UINT64_MAX;
    t.ready_since_us = p_.now_us();
    t.ready_since_valid = true;
    t.entry = entry;
    t.arg = arg;
    if (handle) *handle = id;
    return true;
  }
  return false;
}
void domain::task_delay(tick_t ticks) {
  if (p_.in_isr()) return;
  if (ticks == 0) { p_.yield(); return; }
  const auto deadline=ticks==kForever?UINT64_MAX:p_.now_ticks()+ticks;
  while(p_.now_ticks()<deadline) { poll_timers(); block_current(deadline); p_.park_until(deadline); }
}
bool domain::task_suspend(uint32_t id) {
  if (p_.in_isr()) return false;
  for (auto &t : tasks_) if (t.id == id && t.status != state::done) { t.status = state::suspended; return true; }
  return false;
}
bool domain::task_resume(uint32_t id, bool from_isr, bool *higher) {
  if (from_isr != p_.in_isr()) return false;
  for (auto &t : tasks_) if (t.id == id && t.status == state::suspended) {
    t.status = state::ready; t.ready_since_us = p_.now_us(); t.ready_since_valid = true;
    const bool hp=p_.wake(id);if(higher&&hp)*higher=true;return true;
  }
  return false;
}
bool domain::task_delete(uint32_t id) {
  if (p_.in_isr()) return false;
  if (!id) id = p_.current_task();
  for (auto &t : tasks_) if (t.id == id && t.status != state::unused && t.status != state::done) {
    t.status = state::done; t.wake_deadline = UINT64_MAX;
    publish_owned_mutexes(id);
    // task_finished() may drop the identity current_task() derives from the stack,
    // so decide "self" first.
    const bool is_self = id == p_.current_task();
    p_.task_finished(id);
    if (is_self) p_.exit_thread();
    return true;
  }
  return false;
}
bool domain::task_set_priority(uint32_t id, uint32_t priority) {
  if (p_.in_isr()) return false;
  auto *t = find_task(id);
  if (!t) return false;
  t->base_priority = priority;
  refresh_mutex_priority(id);
  return true;
}
bool domain::task_set_scheduling_boost(uint32_t id, uint32_t boost) {
  if (p_.in_isr()) return false;
  auto *t = find_task(id);
  if (!t) return false;
  t->scheduling_boost = boost;
  return true;
}
uint32_t domain::task_priority(uint32_t id) const {
  if (id == 0) return root_task_.priority;
  for (const auto &t : tasks_) if (t.id == id && t.status != state::unused) return t.priority;
  return 0;
}
void domain::set_scheduler_idle_poll_us(uint32_t poll_us) {
  scheduler_idle_poll_us_ = poll_us ? poll_us : 1u;
}
task_control *domain::find_task(uint32_t id) {
  if (id == 0) return &root_task_;
  for (auto &t : tasks_) if (t.id == id && t.status != state::unused && t.status != state::done) return &t;
  return nullptr;
}
const task_control *domain::find_task(uint32_t id) const {
  if (id == 0) return &root_task_;
  for (const auto &t : tasks_) if (t.id == id && t.status != state::unused && t.status != state::done) return &t;
  return nullptr;
}
bool domain::task_notify(uint32_t id, uint32_t index, uint32_t value, uint32_t action,
                         uint32_t *previous, bool from_isr, bool *higher) {
  if (index != 0 || from_isr != p_.in_isr()) return false;
  auto *t = find_task(id);
  if (!t) return false;
  if (previous) *previous = t->notification_value;
  if (action == 4u && t->notification_pending) return false; // eSetValueWithoutOverwrite
  switch (action) {
    case 0: break; // eNoAction
    case 1: t->notification_value |= value; break; // eSetBits
    case 2: t->notification_value = t->notification_value + 1u; break; // eIncrement
    case 3: t->notification_value = value; break; // eSetValueWithOverwrite
    case 4: t->notification_value = value; break; // eSetValueWithoutOverwrite
    default: return false;
  }
  t->notification_pending = true;
  bool woke = false;
  if (t->notification_waiting && t->status == state::blocked) {
    make_ready(id);
    woke = p_.wake(id);
  } else if (t->notification_waiting) {
    t->wake_pending = true; // waiter is between its check and block_current()
  }
  if (higher) *higher = woke;
  if (woke && !from_isr) p_.yield();
  return true;
}
uint32_t domain::task_notify_take(uint32_t index, bool clear_count, tick_t timeout) {
  if (index != 0 || p_.in_isr()) return 0;
  const uint32_t id = p_.current_task();
  auto *t = find_task(id);
  if (!t) return 0;
  const uint64_t end = timeout == kForever ? UINT64_MAX : p_.now_ticks() + timeout;
  while (t->notification_value == 0) {
    if (timeout == 0 || p_.now_ticks() >= end) {
      t->notification_waiting = false;
      if (t->status != state::done) t->status = state::running;
      return 0;
    }
    t->notification_waiting = true;
    if (block_current(end)) p_.park_until(end);
    make_ready(id);
    t->notification_waiting = false;
  }
  const uint32_t value = t->notification_value;
  if (clear_count) t->notification_value = 0;
  else t->notification_value = t->notification_value - 1u;
  t->notification_pending = false;
  if (t->status != state::done) t->status = state::running;
  return value;
}
bool domain::task_notify_wait(uint32_t index, uint32_t clear_entry, uint32_t clear_exit,
                              uint32_t *value, tick_t timeout) {
  if (index != 0 || p_.in_isr()) return false;
  const uint32_t id = p_.current_task();
  auto *t = find_task(id);
  if (!t) return false;
  if (!t->notification_pending) t->notification_value &= ~clear_entry;
  const uint64_t end = timeout == kForever ? UINT64_MAX : p_.now_ticks() + timeout;
  while (!t->notification_pending) {
    if (timeout == 0 || p_.now_ticks() >= end) {
      if (value) *value = t->notification_value;
      t->notification_waiting = false;
      if (t->status != state::done) t->status = state::running;
      return false;
    }
    t->notification_waiting = true;
    if (block_current(end)) p_.park_until(end);
    make_ready(id);
    t->notification_waiting = false;
  }
  if (value) *value = t->notification_value;
  t->notification_value &= ~clear_exit;
  t->notification_pending = false;
  if (t->status != state::done) t->status = state::running;
  return true;
}
bool domain::task_notify_state_clear(uint32_t id, uint32_t index) {
  if (index != 0 || p_.in_isr()) return false;
  auto *t = find_task(id);
  if (!t) return false;
  const bool was_pending = t->notification_pending;
  t->notification_pending = false;
  return was_pending;
}
uint32_t domain::task_notify_value_clear(uint32_t id, uint32_t index, uint32_t bits) {
  if (index != 0 || p_.in_isr()) return 0;
  auto *t = find_task(id);
  if (!t) return 0;
  const uint32_t previous = t->notification_value;
  t->notification_value &= ~bits;
  return previous;
}
uint64_t domain::tick_count() { poll_timers(); return p_.now_ticks(); }
uint64_t domain::time_us() { return p_.now_us(); }
uint32_t domain::current_task() const { return p_.current_task(); }
bool domain::isr_context() { return p_.in_isr(); }
bool domain::configure_backend(uint32_t handler, uint32_t method, uint32_t tick_hz) {
  if(!tick_hz)return false;
  // handler == 0 selects the single-stage PLAIN task-object backend. The method
  // argument is then the first object ID reserved for compatibility tasks.
  if(!handler && (method < 48 || method > 63))return false;
  if(!p_.configure_route(handler, method, tick_hz, this))return false;
  tick_hz_=tick_hz; return true;
}
bool domain::block_current(uint64_t deadline) {
  const uint32_t id=p_.current_task();
  if (id == 0) {
    if (root_task_.wake_pending) { root_task_.wake_pending = false; return false; }
    root_task_.status = state::blocked;
    root_task_.wake_deadline = deadline;
    p_.block_task(id, deadline);
    return true;
  }
  for(auto &t:tasks_) if(t.id==id&&t.status!=state::done) {
    // A wake delivered between waiter registration and this call found the task
    // not BLOCKED; do not sleep through it. The caller re-evaluates its condition.
    if(t.wake_pending){t.wake_pending=false;return false;}
    t.status=state::blocked; t.wake_deadline=deadline;
    // A blocked task is not eligible until make_ready() transitions it back;
    // add no extra delay here, especially for controller LL tasks woken by IRQ.
    t.eligible_after_us=p_.now_us(); p_.block_task(id,deadline); return true;
  }
  return true; // unknown task (e.g. host/root-less caller): nothing to skip, caller parks
}
void domain::make_ready(uint32_t id) {
  if (id == 0 && root_task_.status == state::blocked) {
    root_task_.status = state::ready;
    root_task_.wake_deadline = UINT64_MAX;
    p_.make_ready(id);
    return;
  }
  for(auto &t:tasks_) if(t.id==id&&t.status==state::blocked) {
    t.status=state::ready; t.wake_deadline=UINT64_MAX;
    t.ready_since_us=p_.now_us(); t.ready_since_valid=true;
    p_.make_ready(id); return;
  }
}
bool domain::has_waiter(const sync_object &o, uint32_t id, bool for_data) const {
  const size_t side=for_data?1:0;
  for(size_t i=0;i<o.waiter_count[side];i++) if(o.waiters[side][i].task==id) return true;
  return false;
}
void domain::clear_wake_pending(uint32_t id) {
  if (id == 0) { root_task_.wake_pending = false; return; }
  for(auto &t:tasks_) if(t.id==id) { t.wake_pending=false; return; }
}
uint64_t domain::bounded_block_deadline(uint64_t end) {
  if (end != UINT64_MAX) return end;
  return p_.now_ticks() + kForeverRecheckTicks;
}
void domain::wake_waiter(uint32_t id) {
  if (id == 0) {
    if (root_task_.status == state::blocked) make_ready(id); else root_task_.wake_pending = true;
    return;
  }
  for(auto &t:tasks_) if(t.id==id&&t.status!=state::done&&t.status!=state::unused) {
    if(t.status==state::blocked) make_ready(id); else t.wake_pending=true;
    return;
  }
}
void domain::finish_current_task() {
  const uint32_t id=p_.current_task();
  for(auto &t:tasks_) if(t.id==id) { t.status=state::done; t.wake_deadline=UINT64_MAX; break; }
  publish_owned_mutexes(id);
  p_.task_finished(id); p_.exit_thread();
}
void domain::run_scheduler(uint32_t grant_cycles) {
  if(!grant_cycles)return;
#if defined(SHIZUKU_ESP32_C6)
  if (shizuku_esp_ble_note_compat_progress)
    shizuku_esp_ble_note_compat_progress(0u, 0u, 0u);
#endif
  for(;;) {
    (void)p_.consume_reschedule_pending(); // ISR requests are applied at this grant boundary.
    const uint64_t now=p_.now_ticks();
    const uint64_t now_us=p_.now_us();
    task_control *best=nullptr; size_t best_index=0; uint64_t deadline=UINT64_MAX;
    for(size_t step=0;step<kMaxTasks;step++) {
      const size_t i=(scheduler_cursor_+step)%kMaxTasks; auto &t=tasks_[i];
      if(t.status==state::blocked) {
        if(t.wake_deadline<=now) {
          t.status=state::ready; t.wake_deadline=UINT64_MAX;
          t.ready_since_us=now_us; t.ready_since_valid=true;
          p_.make_ready(t.id);
        }
        else if(t.wake_deadline<deadline) deadline=t.wake_deadline;
      }
      const uint32_t effective_priority = UINT32_MAX - t.priority < t.scheduling_boost
          ? UINT32_MAX : t.priority + t.scheduling_boost;
      const uint32_t best_effective_priority = best && UINT32_MAX - best->priority < best->scheduling_boost
          ? UINT32_MAX : (best ? best->priority + best->scheduling_boost : 0u);
      if(t.status==state::ready&&t.eligible_after_us<=now_us&&(!best||effective_priority>best_effective_priority)) { best=&t;best_index=i; }
    }
    if(best) {
      scheduler_cursor_=(best_index+1)%kMaxTasks;
      if (best->ready_since_valid) {
#if defined(SHIZUKU_ESP_BLE_ENABLE_SCHED_DIAG_IRQ_MASK) && SHIZUKU_ESP_BLE_ENABLE_SCHED_DIAG_IRQ_MASK
        const uint32_t irq_key = p_.irq_save();
#endif
        const uint64_t ready_since_us = best->ready_since_us;
        const uint64_t dispatch_us = p_.now_us();
        best->ready_since_valid = false;
#if defined(SHIZUKU_ESP_BLE_ENABLE_SCHED_DIAG_IRQ_MASK) && SHIZUKU_ESP_BLE_ENABLE_SCHED_DIAG_IRQ_MASK
        p_.irq_restore(irq_key);
#endif
        const bool clock_skew = dispatch_us < ready_since_us;
        const uint32_t latency = clock_skew ? 0u : static_cast<uint32_t>(
            dispatch_us - ready_since_us);
        if (shizuku_esp_ble_note_task_scheduled)
          shizuku_esp_ble_note_task_scheduled(best->id,
                                             best->priority + best->scheduling_boost, latency,
                                             clock_skew ? 1u : 0u);
      }
      best->status=state::running;
#if defined(SHIZUKU_ESP32_C6)
      if (shizuku_esp_ble_note_compat_progress)
        shizuku_esp_ble_note_compat_progress(1u, best->id, 0u);
#endif
      const bool grant_ok = p_.run_for(best->id,grant_cycles);
#if defined(SHIZUKU_ESP32_C6)
      if (shizuku_esp_ble_note_compat_progress)
        shizuku_esp_ble_note_compat_progress(2u, best->id, grant_ok ? 1u : 0u);
#endif
      if(!grant_ok) { best->status=state::suspended; return; }
      if(best->status==state::running) best->status=state::ready;
      continue;
    }
    uint64_t idle_us=scheduler_idle_poll_us_; // IRQ wake is observed on the next bounded idle poll.
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
size_t domain::objects_used() const {
  size_t used = 0;
  for (const auto &o : objects_) if (o.used) ++used;
  return used;
}
uint32_t domain::object_kind_code(int handle) const {
  if (handle < 0 || static_cast<size_t>(handle) >= kMaxObjects || !objects_[handle].used) return 0;
  return static_cast<uint32_t>(objects_[handle].kind) + 1u;
}
int domain::create_queue(size_t cap, size_t sz) {
  if (p_.in_isr() || !cap || !sz || cap > SIZE_MAX / sz || cap * sz > kQueueStorageBytes) return -1;
  for (size_t i=0;i<kMaxObjects;i++) if (!objects_[i].used) {
    auto &o=objects_[i]; o={}; o.used=true; o.kind=object_kind::queue;
    o.capacity=cap; o.item_size=sz; return static_cast<int>(i);
  }
  return -1;
}
int domain::create_semaphore(uint32_t limit, uint32_t initial) {
  if (p_.in_isr() || !limit || initial>limit) return -1;
  for (size_t i = 0; i < kMaxObjects; ++i) {
    if (!objects_[i].used) {
      auto &o = objects_[i]; o = {}; o.used = true; o.kind = object_kind::semaphore;
      o.limit = limit; o.count = initial; publish_sync_object(static_cast<int>(i)); return static_cast<int>(i);
    }
  }
  return -1;
}
int domain::create_mutex(bool recursive) {
  if (p_.in_isr()) return -1;
  for (size_t i = 0; i < kMaxObjects; ++i) {
    if (!objects_[i].used) {
      auto &o = objects_[i]; o = {}; o.used = true; o.kind = object_kind::mutex;
      o.limit = recursive ? 1u : 0u; o.owner = UINT32_MAX; publish_sync_object(static_cast<int>(i)); return static_cast<int>(i);
    }
  }
  return -1;
}
bool domain::object_set_creator(int handle, uint32_t pc) {
  if (handle < 0 || static_cast<size_t>(handle) >= kMaxObjects || !objects_[handle].used)
    return false;
  objects_[handle].creator_pc = pc;
  publish_sync_object(handle);
  return true;
}
bool domain::object_set_handle(int handle, uint32_t raw_handle) {
  if (handle < 0 || static_cast<size_t>(handle) >= kMaxObjects || !objects_[handle].used)
    return false;
  objects_[handle].raw_handle = raw_handle;
  publish_sync_object(handle);
  return true;
}
bool domain::object_snapshot(int handle, sync_snapshot *out) const {
  if (!out || handle < 0 || static_cast<size_t>(handle) >= kMaxObjects) return false;
  const auto &o = objects_[handle];
  if (!o.used) return false;
  out->kind = static_cast<uint32_t>(o.kind) + 1u;
  out->creator_pc = o.creator_pc;
  out->raw_handle = o.raw_handle;
  out->recursive = o.kind == object_kind::mutex && o.limit != 0 ? 1u : 0u;
  out->owner = o.kind == object_kind::mutex ? o.owner : UINT32_MAX;
  out->recursion = o.kind == object_kind::mutex ? o.recursion : 0u;
  if (o.kind == object_kind::mutex && o.owner != UINT32_MAX) {
    if (o.owner == 0) out->owner_state = static_cast<uint32_t>(root_task_.status);
    else for (const auto &task : tasks_)
      if (task.id == o.owner && task.status != state::unused) {
        out->owner_state = static_cast<uint32_t>(task.status);
        break;
      }
  }
  out->count = o.kind == object_kind::semaphore ? o.count : static_cast<uint32_t>(o.length);
  out->waiters = static_cast<uint32_t>(o.waiter_count[0] + o.waiter_count[1]);
  out->last_taker_task = o.last_taker_task;
  out->last_giver_task = o.last_giver_task;
  out->take_count = o.take_count;
  out->give_count = o.give_count;
  uint32_t highest_priority = 0;
  for (size_t i = 0; i < o.waiter_count[1]; ++i) {
    const auto &w = o.waiters[1][i];
    if (out->highest_waiter_task == UINT32_MAX || w.priority > highest_priority) {
      out->highest_waiter_task = w.task;
      highest_priority = w.priority;
    }
  }
  return true;
}
void domain::publish_sync_object(int handle) {
  if (!shizuku_esp_ble_note_sync_object) return;
  sync_snapshot s{};
  if (!object_snapshot(handle, &s)) {
    shizuku_esp_ble_note_sync_object(static_cast<uint32_t>(handle), 0, 0, 0, 0,
        UINT32_MAX, 0, 0, 0, UINT32_MAX, UINT32_MAX, 0, 0, UINT32_MAX,
        UINT32_MAX);
    return;
  }
  shizuku_esp_ble_note_sync_object(static_cast<uint32_t>(handle), s.kind, s.creator_pc,
      s.raw_handle,
      s.recursive,
      s.owner, s.recursion, s.count, s.waiters, s.last_taker_task,
      s.last_giver_task, s.take_count, s.give_count, s.highest_waiter_task,
      s.owner_state);
}
void domain::publish_owned_mutexes(uint32_t task) {
  for (size_t i = 0; i < kMaxObjects; ++i)
    if (objects_[i].used && objects_[i].kind == object_kind::mutex &&
        objects_[i].owner == task) publish_sync_object(static_cast<int>(i));
}
void domain::refresh_mutex_priority(uint32_t id) {
  auto *task = find_task(id);
  if (!task) return;
  uint32_t inherited = task->base_priority;
  for (const auto &o : objects_) {
    if (!o.used || o.kind != object_kind::mutex || o.owner != id) continue;
    for (size_t i = 0; i < o.waiter_count[1]; ++i)
      if (o.waiters[1][i].priority > inherited) inherited = o.waiters[1][i].priority;
  }
  task->priority = inherited;
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
  const bool higher=p_.wake(id); wake_waiter(id); return higher;
}
bool domain::wait(sync_object &o, bool data, tick_t timeout, bool from_isr) {
  if(from_isr != p_.in_isr()) return false;
  if(from_isr && timeout) return false;
  const uint64_t end=timeout==kForever?UINT64_MAX:p_.now_ticks()+timeout;
  bool registered=false;
  for (;;) {
    const uint64_t now = p_.now_ticks();
    const uint32_t key = p_.irq_save();
    const bool condition_ready = data ? o.length != 0 : o.length < o.capacity;
    if (condition_ready) {
      if (registered) remove_waiter(o, p_.current_task(), data);
      p_.irq_restore(key);
      return true;
    }
    if (now >= end) {
      if (registered) remove_waiter(o, p_.current_task(), data);
      p_.irq_restore(key);
      return false;
    }
    if (!registered) {
      if (!add_waiter(o, data)) {
        p_.irq_restore(key);
        return false;
      }
      registered = true;
    }
    // Mark BLOCKED before restoring interrupts so a concurrent FromISR give
    // cannot wake the task between waiter registration and block_current().
    block_current(end);
    p_.irq_restore(key);
    poll_timers();
    p_.park_until(end);
    const uint32_t wake_key = p_.irq_save();
    if (registered) remove_waiter(o, p_.current_task(), data);
    registered = false;
    p_.irq_restore(wake_key);
  }
}
bool domain::queue_send(int h,const void *item,tick_t timeout,bool front,bool isr,bool *higher) {
  auto *o=get(h,object_kind::queue); if(!o||!item) return false;
  if (isr != p_.in_isr() || (isr && timeout)) return false;
  const uint64_t end = timeout == kForever ? UINT64_MAX : p_.now_ticks() + timeout;
  for (;;) {
    const uint64_t now = p_.now_ticks();
    const tick_t remaining = end == UINT64_MAX ? kForever :
        (now >= end ? 0u : static_cast<tick_t>(end - now));
    if (!wait(*o, false, remaining, isr)) return false;
    const uint32_t irq = p_.irq_save();
    // Availability can change after wait() observes it but before the item
    // copy begins (e.g. an ISR or equal-priority task filled the last slot).
    // Recheck while masked to avoid writing past the logical ring capacity.
    if (o->length >= o->capacity) {
      p_.irq_restore(irq);
      continue;
    }
    const size_t slot = front ? (o->head + o->capacity - 1) % o->capacity
                              : (o->head + o->length) % o->capacity;
    std::memcpy(o->storage + slot * o->item_size, item, o->item_size);
    if (front) o->head = slot;
    ++o->length;
    uint32_t first_word = 0;
    if (o->item_size >= sizeof(first_word))
      std::memcpy(&first_word, item, sizeof(first_word));
    const uint32_t raw_handle = o->raw_handle;
    const uint32_t item_size = static_cast<uint32_t>(o->item_size);
    const bool hp = wake_one(*o, true);
    p_.irq_restore(irq);
    if (shizuku_esp_ble_note_queue_send)
      shizuku_esp_ble_note_queue_send(raw_handle, item_size, first_word);
    if (higher && hp) *higher = true;
    return true;
  }
}
bool domain::queue_receive(int h,void *item,tick_t timeout,bool isr,bool *higher) {
  auto *o=get(h,object_kind::queue); if(!o||!item) return false;
  if (isr != p_.in_isr() || (isr && timeout)) return false;
  const uint64_t end = timeout == kForever ? UINT64_MAX : p_.now_ticks() + timeout;
  for (;;) {
    const uint64_t now = p_.now_ticks();
    const tick_t remaining = end == UINT64_MAX ? kForever :
        (now >= end ? 0u : static_cast<tick_t>(end - now));
    if (!wait(*o, true, remaining, isr)) return false;
    const uint32_t irq = p_.irq_save();
    // An ISR/task may have consumed the item after wait() observed it.
    if (o->length == 0) {
      p_.irq_restore(irq);
      continue;
    }
    std::memcpy(item, o->storage + o->head * o->item_size, o->item_size);
    o->head = (o->head + 1) % o->capacity;
    --o->length;
    uint32_t first_word = 0;
    if (o->item_size >= sizeof(first_word))
      std::memcpy(&first_word, item, sizeof(first_word));
    const uint32_t raw_handle = o->raw_handle;
    const uint32_t item_size = static_cast<uint32_t>(o->item_size);
    const bool hp = wake_one(*o, false);
    p_.irq_restore(irq);
    if (shizuku_esp_ble_note_queue_receive)
      shizuku_esp_ble_note_queue_receive(raw_handle, item_size, first_word);
    if (higher && hp) *higher = true;
    return true;
  }
}
size_t domain::queue_messages(int h) const {
  if (h < 0 || size_t(h) >= kMaxObjects) return 0;
  const uint32_t irq = p_.irq_save();
  const auto &o = objects_[h];
  const size_t result = o.used && o.kind == object_kind::queue ? o.length : 0;
  p_.irq_restore(irq);
  return result;
}
bool domain::queue_empty(int h) const {
  if (h < 0 || size_t(h) >= kMaxObjects) return false;
  const uint32_t irq = p_.irq_save();
  const auto &o = objects_[h];
  const bool result = o.used && o.kind == object_kind::queue && o.length == 0;
  p_.irq_restore(irq);
  return result;
}
size_t domain::queue_spaces(int h) const { return h>=0&&size_t(h)<kMaxObjects&&objects_[h].used&&objects_[h].kind==object_kind::queue?objects_[h].capacity-objects_[h].length:0; }
bool domain::queue_reset(int h) {
  auto *o=get(h,object_kind::queue);
  if (!o || o->waiter_count[0] || o->waiter_count[1]) return false;
  o->head=0; o->length=0; return true;
}
bool domain::object_delete(int h) {
  if (p_.in_isr() || h<0 || size_t(h)>=kMaxObjects) return false;
  auto &o=objects_[h];
  if (!o.used || o.waiter_count[0] || o.waiter_count[1]) return false;
  o={}; publish_sync_object(h); return true;
}
bool domain::semaphore_take(int h,tick_t timeout,bool isr) {
  auto *o=get(h,object_kind::semaphore); if(!o||isr!=p_.in_isr()||(isr&&timeout))return false;
  o->last_taker_task = p_.current_task();
  o->take_count = o->take_count + 1u;
  if(o->count){--o->count;publish_sync_object(h);return true;} if(timeout==0){publish_sync_object(h);return false;}
  uint64_t end=timeout==kForever?UINT64_MAX:p_.now_ticks()+timeout;
  bool registered=false; const uint32_t me=p_.current_task();
  do {
    poll_timers();
    if(o->count){if(registered)remove_waiter(*o,me,true);clear_wake_pending(me);--o->count;publish_sync_object(h);return true;}
    if(p_.now_ticks()>=end)break;
    if(!registered){if(!add_waiter(*o,true))return false;registered=true;publish_sync_object(h);continue;}
    if(block_current(bounded_block_deadline(end))) p_.park_until(end);
    registered=has_waiter(*o,me,true); // a give pops the waiter; a bounded recheck leaves it queued
  } while(true);
  remove_waiter(*o,me,true); clear_wake_pending(me); publish_sync_object(h); return false;
}
bool domain::semaphore_give(int h,bool isr,bool *higher) { auto *o=get(h,object_kind::semaphore);if(!o||isr!=p_.in_isr()||o->count==o->limit)return false;++o->count;o->last_giver_task=p_.current_task();o->give_count=o->give_count+1u;const bool hp=wake_one(*o,true);if(higher&&hp)*higher=true;publish_sync_object(h);return true; }
uint32_t domain::semaphore_count(int h) const { return h>=0&&size_t(h)<kMaxObjects&&objects_[h].used&&objects_[h].kind==object_kind::semaphore?objects_[h].count:0; }
bool domain::mutex_take(int h,tick_t timeout,bool isr,bool recursive_api) {
  auto *o=get(h,object_kind::mutex); if(!o||isr||p_.in_isr())return false;
  auto me=p_.current_task();
  o->last_taker_task = me;
  o->take_count = o->take_count + 1u;
#if defined(SHIZUKU_C5_MUTEX_SELF_RETAKE_FAILFAST)
  // Diagnostic-only A/B mode: restore the pre-regression fast failure for a
  // same-task nonrecursive retake. Production/default builds keep FreeRTOS
  // block-until-release-or-timeout semantics.
  if (o->owner == me && !recursive_api) {
    publish_sync_object(h);
    return false;
  }
#endif
  if(o->owner==me && recursive_api) {
    if(!o->limit){publish_sync_object(h);return false;}
    ++o->recursion;
    publish_sync_object(h);
    return true;
  }
  uint64_t end=timeout==kForever?UINT64_MAX:p_.now_ticks()+timeout; bool registered=false;
  do {
    poll_timers();
    if(o->owner==UINT32_MAX){if(registered)remove_waiter(*o,me,true);clear_wake_pending(me);o->owner=me;o->recursion=1;publish_sync_object(h);return true;}
    if(timeout==0||p_.now_ticks()>=end){publish_sync_object(h);break;}
    if(!registered){
      if(!add_waiter(*o,true)){publish_sync_object(h);return false;}
      registered=true;refresh_mutex_priority(o->owner);publish_sync_object(h);
      continue; // a give may have run between the owner check and registration: re-check before blocking
    }
    const uint32_t owner = o->owner;
    // Never sleep without a bound: a wake that is missed anyway (see wake_pending) is
    // recovered by the scheduler at the next tick instead of waiting forever.
    if(block_current(bounded_block_deadline(end))) p_.park_until(end);
    registered=has_waiter(*o,me,true); // a give pops the waiter; a bounded recheck leaves it queued
    if(!registered){if(owner!=UINT32_MAX)refresh_mutex_priority(owner);publish_sync_object(h);}
  } while(true);
  remove_waiter(*o,me,true); clear_wake_pending(me);
  if(o->owner!=UINT32_MAX)refresh_mutex_priority(o->owner);
  publish_sync_object(h); return false;
}
bool domain::mutex_give(int h,bool isr) { auto *o=get(h,object_kind::mutex);if(!o||isr||p_.in_isr()||o->owner!=p_.current_task())return false;o->last_giver_task=p_.current_task();o->give_count=o->give_count+1u;if(--o->recursion==0){const uint32_t owner=o->owner;o->owner=UINT32_MAX;wake_one(*o,true);refresh_mutex_priority(owner);}publish_sync_object(h);return true; }
uint32_t domain::mutex_owner(int h) const { return h>=0&&size_t(h)<kMaxObjects&&objects_[h].used&&objects_[h].kind==object_kind::mutex&&objects_[h].owner!=UINT32_MAX?objects_[h].owner:0; }
uint32_t domain::critical_enter(){return p_.irq_save();}
void domain::critical_exit(uint32_t k){p_.irq_restore(k);}

int domain::create_event_group() {
  if (p_.in_isr()) return -1;
  for (size_t i = 0; i < kMaxObjects; ++i) if (!objects_[i].used) {
    auto &o = objects_[i]; o = {}; o.used = true; o.kind = object_kind::event_group;
    return static_cast<int>(i);
  }
  return -1;
}
uint32_t domain::event_group_set(int handle, uint32_t bits) {
  auto *o = get(handle, object_kind::event_group);
  if (!o || p_.in_isr()) return 0;
  o->event_bits |= bits & 0x00ffffffu;
  return o->event_bits;
}
uint32_t domain::event_group_clear(int handle, uint32_t bits) {
  auto *o = get(handle, object_kind::event_group);
  if (!o || p_.in_isr()) return 0;
  const uint32_t previous = o->event_bits;
  o->event_bits &= ~(bits & 0x00ffffffu);
  return previous;
}
uint32_t domain::event_group_wait(int handle, uint32_t bits, bool clear_on_exit,
                                  bool wait_all, tick_t timeout) {
  auto *o = get(handle, object_kind::event_group);
  if (!o || p_.in_isr()) return 0;
  const uint32_t mask = bits & 0x00ffffffu;
  const uint64_t end = timeout == kForever ? UINT64_MAX : p_.now_ticks() + timeout;
  for (;;) {
    const uint32_t observed = o->event_bits;
    const bool ready = wait_all ? (observed & mask) == mask : (observed & mask) != 0;
    if (ready) {
      if (clear_on_exit) o->event_bits &= ~mask;
      return observed;
    }
    if (timeout == 0 || p_.now_ticks() >= end) return o->event_bits;
    poll_timers();
    block_current(end);
    p_.park_until(end);
    make_ready(p_.current_task());
  }
}
int domain::create_timer(uint32_t period, bool auto_reload, void *id,
                         void (*callback)(void *)) {
  if (p_.in_isr() || !period || !callback) return -1;
  for (size_t i = 0; i < kMaxObjects; ++i) if (!objects_[i].used) {
    auto &o = objects_[i]; o = {}; o.used = true; o.kind = object_kind::timer;
    o.period_ticks = period; o.auto_reload = auto_reload; o.timer_id = id;
    o.timer_callback = callback; o.expiry_tick = UINT64_MAX;
    return static_cast<int>(i);
  }
  return -1;
}
bool domain::timer_start(int handle, bool from_isr) {
  auto *o = get(handle, object_kind::timer); if (!o || from_isr != p_.in_isr()) return false;
  o->expiry_tick = p_.now_ticks() + o->period_ticks; o->active = true; return true;
}
bool domain::timer_stop(int handle, bool from_isr) {
  auto *o = get(handle, object_kind::timer); if (!o || from_isr != p_.in_isr()) return false;
  o->active = false; o->expiry_tick = UINT64_MAX; return true;
}
bool domain::timer_delete(int handle) {
  auto *o = get(handle, object_kind::timer); if (!o || p_.in_isr()) return false;
  *o = {}; return true;
}
bool domain::timer_change_period(int handle, uint32_t period, bool from_isr) {
  auto *o = get(handle, object_kind::timer); if (!o || from_isr != p_.in_isr() || !period) return false;
  o->period_ticks = period; o->expiry_tick = p_.now_ticks() + period; o->active = true; return true;
}
uint32_t domain::timer_expiry(int handle) const {
  if (handle < 0 || static_cast<size_t>(handle) >= kMaxObjects) return 0;
  const auto &o = objects_[handle];
  return o.used && o.kind == object_kind::timer ? static_cast<uint32_t>(o.expiry_tick) : 0;
}
bool domain::timer_active(int handle) const {
  return handle >= 0 && static_cast<size_t>(handle) < kMaxObjects &&
         objects_[handle].used && objects_[handle].kind == object_kind::timer && objects_[handle].active;
}
void *domain::timer_id(int handle) const {
  return handle >= 0 && static_cast<size_t>(handle) < kMaxObjects &&
         objects_[handle].used && objects_[handle].kind == object_kind::timer ? objects_[handle].timer_id : nullptr;
}
void domain::poll_timers() {
  if (polling_timers_ || p_.in_isr()) return;
  polling_timers_ = true;
  const uint64_t now = p_.now_ticks();
  for (size_t i = 0; i < kMaxObjects; ++i) {
    auto &o = objects_[i];
    if (!o.used || o.kind != object_kind::timer || !o.active || o.expiry_tick > now) continue;
    if (o.auto_reload) o.expiry_tick = now + o.period_ticks;
    else { o.active = false; o.expiry_tick = UINT64_MAX; }
    const auto callback = o.timer_callback;
    if (callback) callback(reinterpret_cast<void *>(static_cast<uintptr_t>(i) + 1));
  }
  polling_timers_ = false;
}
} // namespace shizuku::freertos_compat
