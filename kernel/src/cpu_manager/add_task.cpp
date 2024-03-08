#include "shizuku/cpu_manager.hpp"

void shizuku::types::cpu_manager::add_task(
    shizuku::platform::std::shared_ptr<shizuku::types::thread> thread,
    size_t processing_time, int priority) {
  thread->status = wait_executing;
  task_queue.push(
      {.thread = thread, .remain_time = processing_time, .priority = priority});
}