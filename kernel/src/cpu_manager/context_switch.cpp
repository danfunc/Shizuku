
#include "shizuku/cpu_manager.hpp"

using namespace shizuku::types;
void cpu_manager::context_switch(context *current, context *next) {
  if (save_context(current) == 0) {
    load_context(next);
  } else {
    return;
  }
}

void cpu_manager::context_switch() {
  if (current_task.remain_time == 0) {
    return;
  } else {
    if (--current_task.remain_time == 0) {
      before_context = current_context;
      while (true) {
        if (task_queue.empty()) {
          current_task.thread = default_thread;
          current_task.remain_time = 0;
          current_task.priority = 0;
          current_context = default_thread->context;
          break;
        } else {
          current_task = task_queue.top();
          task_queue.pop();
          if (auto next_thread = current_task.thread.lock()) {
            current_context = next_thread->context;
            break;
          } else {
            continue;
          }
        }
      }
      context_switch(before_context.get(), current_context.get());
      before_context.reset();
      return;
    } else {
      return;
    }
  }
}