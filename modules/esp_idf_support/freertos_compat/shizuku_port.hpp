#ifndef SHIZUKU_ESP_IDF_FREERTOS_SHIZUKU_PORT_HPP
#define SHIZUKU_ESP_IDF_FREERTOS_SHIZUKU_PORT_HPP
#include "compat.hpp"
#include "shizuku/object_api.hpp"

namespace shizuku::freertos_compat {
class shizuku_port final : public port {
 public:
  using host_syscall_hook = uintptr_t (*)(shizuku::object_api,uintptr_t,uintptr_t,uintptr_t,uintptr_t *);
  enum : uint32_t { kStartTask=1, kReadClock=2, kHandshake=3 };
  static constexpr uint32_t kFirstPlainTaskObject = 48;
  static constexpr uintptr_t kHandshakeReply=0x46525453u; // "FRTS"
  static constexpr uint32_t kKernelThreadStackBytes=4096;
  struct task_start { uint32_t operation, id, priority, stack_depth_bytes; void (*entry)(void *); void *arg; };
  bool configure_route(uint32_t handler, uint32_t method, uint32_t tick_hz, void *domain) override;
  uint64_t now_ticks() override;
  uint64_t now_us() override;
  uint32_t current_task() override;
  bool in_isr() override;
  void yield() override;
  void park_until(uint64_t deadline) override;
  bool wake(uint32_t task_id) override;
  bool spawn(void (*entry)(void *), void *arg, uint32_t stack_depth_bytes,
             uint32_t priority, uint32_t *task_id) override;
  bool run_for(uint32_t task_id, uint32_t cycles) override;
  void sleep_us(uint64_t us) override;
  void exit_thread() override;
  void task_finished(uint32_t task_id) override;
  uint32_t irq_save() override;
  void irq_restore(uint32_t state) override;

  // Thread identity from the stack pointer. g_current_task_id is a global that the
  // kernel does not switch with its threads, so while one thread is inside SLEEP_US
  // (global rewritten to its lender) any other compat thread the kernel runs would
  // be mis-identified (as id 0), taking mutexes it can never release.
  // Each compat thread registers the stack it started on; current_task() maps sp back.
#define SHIZUKU_PORT_STACK_ID 1
  static void note_task_stack(uint32_t task_id, uintptr_t sp);
  static bool task_for_stack(uintptr_t sp, uint32_t *task_id);
  static void set_stack_probe(uintptr_t (*probe)()); // test seam; null = real sp
  static void set_clock(uint64_t (*clock_us)());
  static void set_host_syscall_hook(host_syscall_hook hook);
  static void enter_isr();
  static void leave_isr();
  static void request_yield_from_isr();
  static bool pending_yield_from_isr();
  bool consume_reschedule_pending() override;

  static uintptr_t call(shizuku::object_api api, uintptr_t a1=0, uintptr_t a2=0, uintptr_t a3=0,
                        uintptr_t *value=nullptr);
 private:
  uint32_t handler_object_ = 0; // zero selects PLAIN task objects
  uint32_t dispatch_method_ = 0;
  uint32_t tick_hz_ = 100;
  domain *domain_ = nullptr;
  static constexpr size_t kStartSlots = kMaxTasks;
  task_start starts_[kStartSlots]{};
  uintptr_t call_handler(task_start *packet);
  static void plain_task_entry(void *raw);
};
} // namespace shizuku::freertos_compat
#endif
