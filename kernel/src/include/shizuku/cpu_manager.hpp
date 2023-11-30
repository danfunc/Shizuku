
#ifndef SHIZUKU_CPU_MANAGER_HPP
#define SHIZUKU_CPU_MANAGER_HPP
#include "cstdint"
#include "memory"
#include "queue"
#include "set"
#include "shizuku/platform.hpp"
#include "shizuku/processor.hpp"
#include "shizuku/thread.hpp"
namespace shizuku {
namespace types {
struct task {
  shizuku::platform::std::shared_ptr<shizuku::types::thread> thread =
      shizuku::platform::std::make_shared<shizuku::types::thread>();
  size_t remain_time;
  int priority;
  bool operator<(const task &y) const { return this->priority < y.priority; };
  bool operator>(const task &y) const { return this->priority > y.priority; };
};

class cpu_manager : shizuku::cpu_driver {
private:
  shizuku::platform::std::priority_queue<shizuku::types::task> task_queue;
  shizuku::types::task current_task;
  shizuku::platform::std::shared_ptr<shizuku::context> before_context;

public:
  void context_switch();
  static void context_switch(shizuku::context *current, shizuku::context *next);
  void
  add_task(shizuku::platform::std::shared_ptr<shizuku::types::thread> thread,
           size_t processing_time, int priority);
  inline shizuku::platform::std::shared_ptr<shizuku::context>
  get_current_context() {
    return current_task.thread->context;
  };
  inline shizuku::platform::std::shared_ptr<shizuku::types::thread>
  get_current_thread() {
    return current_task.thread;
  };
  inline void set_current_remain(size_t remain_time) {
    current_task.remain_time = remain_time;
  };
  inline void abort_current_task() {
    set_current_remain(1);
    context_switch();
  }
  using cpu_driver::get_core_num;
};
} // namespace types
} // namespace shizuku

static inline bool const operator<(const shizuku::types::task &x,
                                   const shizuku::types::task &y) {
  return x.priority < y.priority;
}

#endif