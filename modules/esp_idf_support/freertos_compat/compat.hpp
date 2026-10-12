#ifndef SHIZUKU_ESP_IDF_FREERTOS_COMPAT_HPP
#define SHIZUKU_ESP_IDF_FREERTOS_COMPAT_HPP

// Experimental FreeRTOS ABI boundary. All objects live in the compatibility
// handler's fixed tables; this layer never creates child objects, avoiding the
// currently broken grandchild syscall route. The Port is the sole kernel seam.
#include <cstddef>
#include <cstdint>

namespace shizuku::freertos_compat {
using tick_t = uint32_t;
constexpr tick_t kForever = UINT32_MAX;
constexpr size_t kMaxTasks = 16, kMaxObjects = 32, kMaxWaiters = 16;

enum class wait_result : uint8_t { ready, timeout, invalid, from_isr_block };
enum class state : uint8_t { unused, ready, running, blocked, suspended, done };
enum class object_kind : uint8_t { queue, semaphore, mutex, event_group, timer };

// Port methods are implemented by the Shizuku HANDLER (syscall dispatch) or
// by the host test adapter. The target backend uses a bounded 100 us SLEEP_US
// poll because the kernel has no event-driven UNPARK primitive.
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
  uint32_t id = 0, priority = 0;
  uint64_t wake_deadline = UINT64_MAX;
  uint64_t eligible_after_us = 0;
  void (*entry)(void *) = nullptr;
  void *arg = nullptr;
};
struct sync_object {
  object_kind kind{};
  bool used = false;
  uint32_t count = 0, limit = 0, owner = 0, recursion = 0;
  size_t item_size = 0, capacity = 0, head = 0, length = 0;
  uint8_t *storage = nullptr;
  waiter waiters[2][kMaxWaiters]{};
  size_t waiter_count[2]{};
};

class domain {
 public:
  explicit domain(port &p) : p_(p) {}
  bool task_create(void (*entry)(void *), const char *name, uint32_t stack_depth_bytes,
                   void *arg, uint32_t priority, uint32_t *handle);
  void task_delay(tick_t ticks);
  bool task_suspend(uint32_t task);
  bool task_resume(uint32_t task, bool from_isr = false, bool *higher_priority_woken = nullptr);
  uint32_t task_priority(uint32_t task) const;
  uint64_t tick_count();
  uint64_t time_us();
  bool isr_context();
  bool configure_backend(uint32_t handler_object, uint32_t method_id, uint32_t tick_hz=100);
  void block_current(uint64_t deadline);
  void make_ready(uint32_t task);
  void run_scheduler(uint32_t grant_cycles);
  void finish_current_task();

  int create_queue(size_t capacity, size_t item_size);
  bool queue_send(int handle, const void *item, tick_t timeout, bool front = false,
                  bool from_isr = false, bool *higher_priority_woken = nullptr);
  bool queue_receive(int handle, void *item, tick_t timeout, bool from_isr = false,
                     bool *higher_priority_woken = nullptr);
  size_t queue_messages(int handle) const;
  int create_semaphore(uint32_t limit, uint32_t initial);
  bool semaphore_take(int handle, tick_t timeout, bool from_isr = false);
  bool semaphore_give(int handle, bool from_isr = false,
                      bool *higher_priority_woken = nullptr);
  int create_mutex(bool recursive);
  bool mutex_take(int handle, tick_t timeout, bool from_isr = false);
  bool mutex_give(int handle, bool from_isr = false);
  uint32_t critical_enter();
  void critical_exit(uint32_t key);

 private:
  port &p_;
  task_control tasks_[kMaxTasks]{};
  sync_object objects_[kMaxObjects]{};
  uint64_t sequence_ = 0;
  size_t scheduler_cursor_ = 0;
  uint32_t tick_hz_ = 100;
  sync_object *get(int handle, object_kind kind);
  bool wait(sync_object &o, bool want_data, tick_t timeout, bool from_isr);
  bool wake_one(sync_object &o, bool waiting_for_data);
  bool add_waiter(sync_object &o, bool waiting_for_data);
  bool remove_waiter(sync_object &o, uint32_t task, bool waiting_for_data);
};
}  // namespace shizuku::freertos_compat
#endif
