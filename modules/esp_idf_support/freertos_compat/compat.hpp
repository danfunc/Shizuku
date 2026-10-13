#ifndef SHIZUKU_ESP_IDF_FREERTOS_COMPAT_HPP
#define SHIZUKU_ESP_IDF_FREERTOS_COMPAT_HPP

// Experimental FreeRTOS ABI boundary. Synchronization objects live in fixed
// tables. The C5 backend can spawn tasks as PLAIN objects through the root
// KERNEL_OBJECT route, without HANDLER-to-HANDLER nesting.
#include <cstddef>
#include <cstdint>

namespace shizuku::freertos_compat {
using tick_t = uint32_t;
constexpr tick_t kForever = UINT32_MAX;
#if defined(SHIZUKU_FREERTOS_COMPAT_TINY)
// C5 controller-init profile bounds static tables to fit alongside the BT blob.
// It keeps headroom for 8 PLAIN task slots and 24 synchronization objects.
constexpr size_t kMaxTasks = 8, kMaxObjects = 24, kMaxWaiters = 4;
// NimBLE's NPL event-queue length is controller supplied, not a fixed small count.
constexpr size_t kQueueStorageBytes = 1024;
#else
constexpr size_t kMaxTasks = 16, kMaxObjects = 32, kMaxWaiters = 16;
constexpr size_t kQueueStorageBytes = 1024;
#endif

enum class wait_result : uint8_t { ready, timeout, invalid, from_isr_block };
enum class state : uint8_t { unused, ready, running, blocked, suspended, done };
enum class object_kind : uint8_t { queue, semaphore, mutex, event_group, timer };

// Port methods use the C5 root KERNEL_OBJECT API through PLAIN task objects,
// or the host test adapter. Blocking remains a bounded 100 us SLEEP_US poll
// because the kernel has no event-driven UNPARK primitive.
struct port {
  virtual ~port() = default;
  virtual uint64_t now_ticks() = 0;
  virtual uint64_t now_us() = 0;
  virtual uint32_t current_task() = 0;
  virtual bool in_isr() = 0;
  virtual void yield() = 0;
  // Atomically yield until wake(task) or the deadline; racing wakeups are latched.
  virtual void park_until(uint64_t deadline) = 0;
  virtual bool wake(uint32_t task_id) = 0;
  virtual bool spawn(void (*entry)(void *), void *arg, uint32_t stack_depth_bytes,
                     uint32_t priority, uint32_t *task_id) = 0;
  virtual bool configure_route(uint32_t, uint32_t, uint32_t, void *) { return false; }
  virtual void block_task(uint32_t, uint64_t) {}
  virtual void make_ready(uint32_t) {}
  virtual bool run_for(uint32_t, uint32_t) { return false; }
  virtual void sleep_us(uint64_t) {}
  virtual void exit_thread() {}
  virtual void task_finished(uint32_t) {}
  virtual bool consume_reschedule_pending() { return false; }
  virtual uint32_t irq_save() = 0;
  virtual void irq_restore(uint32_t state) = 0;
};

struct waiter { uint32_t task; uint32_t priority; uint64_t sequence; };
struct task_control {
  state status = state::unused;
  uint32_t id = 0, priority = 0, base_priority = 0;
  uint64_t wake_deadline = UINT64_MAX;
  uint64_t eligible_after_us = 0;
  uint64_t ready_since_us = 0;
  bool ready_since_valid = false;
  void (*entry)(void *) = nullptr;
  void *arg = nullptr;
  uint32_t notification_value = 0;
  bool notification_pending = false;
  bool notification_waiting = false;
};
struct sync_object {
  object_kind kind{};
  bool used = false;
  uint32_t count = 0, limit = 0, owner = 0, recursion = 0;
  uint32_t creator_pc = 0, raw_handle = 0;
  uint32_t last_taker_task = UINT32_MAX, last_giver_task = UINT32_MAX;
  uint32_t take_count = 0, give_count = 0;
  size_t item_size = 0, capacity = 0, head = 0, length = 0;
  uint8_t storage[kQueueStorageBytes]{};
  waiter waiters[2][kMaxWaiters]{};
  size_t waiter_count[2]{};
  uint64_t expiry_tick = UINT64_MAX;
  uint32_t period_ticks = 0;
  bool auto_reload = false;
  bool active = false;
  uint32_t event_bits = 0;
  void *timer_id = nullptr;
  void (*timer_callback)(void *) = nullptr;
};
struct sync_snapshot {
  uint32_t kind = 0, creator_pc = 0, raw_handle = 0, recursive = 0;
  uint32_t owner = UINT32_MAX, recursion = 0;
  uint32_t count = 0, waiters = 0, last_taker_task = UINT32_MAX;
  uint32_t last_giver_task = UINT32_MAX, take_count = 0, give_count = 0;
  uint32_t highest_waiter_task = UINT32_MAX;
  uint32_t owner_state = UINT32_MAX;
};

class domain {
 public:
  explicit domain(port &p) : p_(p) {}
  bool task_create(void (*entry)(void *), const char *name, uint32_t stack_depth_bytes,
                   void *arg, uint32_t priority, uint32_t *handle);
  void task_delay(tick_t ticks);
  bool task_suspend(uint32_t task);
  bool task_resume(uint32_t task, bool from_isr = false, bool *higher_priority_woken = nullptr);
  bool task_delete(uint32_t task);
  bool task_set_priority(uint32_t task, uint32_t priority);
  uint32_t task_priority(uint32_t task) const;
  bool task_notify(uint32_t task, uint32_t index, uint32_t value, uint32_t action,
                   uint32_t *previous, bool from_isr = false,
                   bool *higher_priority_woken = nullptr);
  uint32_t task_notify_take(uint32_t index, bool clear_count_on_exit, tick_t timeout);
  bool task_notify_wait(uint32_t index, uint32_t clear_on_entry, uint32_t clear_on_exit,
                        uint32_t *value, tick_t timeout);
  bool task_notify_state_clear(uint32_t task, uint32_t index);
  uint32_t task_notify_value_clear(uint32_t task, uint32_t index, uint32_t bits);
  uint64_t tick_count();
  uint64_t time_us();
  uint32_t current_task() const;
  bool isr_context();
  bool configure_backend(uint32_t handler_object, uint32_t method_id, uint32_t tick_hz=100);
  void block_current(uint64_t deadline);
  void make_ready(uint32_t task);
  void run_scheduler(uint32_t grant_cycles);
  void finish_current_task();

  int create_queue(size_t capacity, size_t item_size);
  size_t objects_used() const;
  uint32_t object_kind_code(int handle) const;
  bool queue_send(int handle, const void *item, tick_t timeout, bool front = false,
                  bool from_isr = false, bool *higher_priority_woken = nullptr);
  bool queue_receive(int handle, void *item, tick_t timeout, bool from_isr = false,
                     bool *higher_priority_woken = nullptr);
  size_t queue_messages(int handle) const;
  bool queue_empty(int handle) const;
  size_t queue_spaces(int handle) const;
  bool queue_reset(int handle);
  bool object_delete(int handle);
  int create_semaphore(uint32_t limit, uint32_t initial);
  bool semaphore_take(int handle, tick_t timeout, bool from_isr = false);
  bool semaphore_give(int handle, bool from_isr = false,
                      bool *higher_priority_woken = nullptr);
  uint32_t semaphore_count(int handle) const;
  int create_mutex(bool recursive);
  bool mutex_take(int handle, tick_t timeout, bool from_isr = false,
                  bool recursive_api = true);
  bool mutex_give(int handle, bool from_isr = false);
  uint32_t mutex_owner(int handle) const;
  bool object_set_creator(int handle, uint32_t pc);
  bool object_set_handle(int handle, uint32_t raw_handle);
  bool object_snapshot(int handle, sync_snapshot *out) const;
  uint32_t critical_enter();
  void critical_exit(uint32_t key);

  int create_event_group();
  uint32_t event_group_set(int handle, uint32_t bits);
  uint32_t event_group_clear(int handle, uint32_t bits);
  uint32_t event_group_wait(int handle, uint32_t bits, bool clear_on_exit,
                            bool wait_all, tick_t timeout);
  int create_timer(uint32_t period_ticks, bool auto_reload, void *timer_id,
                   void (*callback)(void *));
  bool timer_start(int handle, bool from_isr = false);
  bool timer_stop(int handle, bool from_isr = false);
  bool timer_delete(int handle);
  bool timer_change_period(int handle, uint32_t period_ticks, bool from_isr = false);
  uint32_t timer_expiry(int handle) const;
  bool timer_active(int handle) const;
  void *timer_id(int handle) const;
  void poll_timers();

 private:
  port &p_;
  task_control tasks_[kMaxTasks]{};
  task_control root_task_{state::running};
  sync_object objects_[kMaxObjects]{};
  uint64_t sequence_ = 0;
  size_t scheduler_cursor_ = 0;
  uint32_t tick_hz_ = 100;
  bool polling_timers_ = false;
  sync_object *get(int handle, object_kind kind);
  bool wait(sync_object &o, bool want_data, tick_t timeout, bool from_isr);
  bool wake_one(sync_object &o, bool waiting_for_data);
  bool add_waiter(sync_object &o, bool waiting_for_data);
  bool remove_waiter(sync_object &o, uint32_t task, bool waiting_for_data);
  void refresh_mutex_priority(uint32_t task);
  void publish_sync_object(int handle);
  void publish_owned_mutexes(uint32_t task);
  task_control *find_task(uint32_t task);
  const task_control *find_task(uint32_t task) const;
};
}  // namespace shizuku::freertos_compat
extern "C" void shizuku_freertos_compat_set_scheduler_state(int32_t state);
#endif
