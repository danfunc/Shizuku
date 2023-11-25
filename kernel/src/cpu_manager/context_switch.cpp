
#include "shizuku/cpu_manager.hpp"

using namespace shizuku::types;
void cpu_manager::context_switch(context *current, context *next) {
  if (!save_context(current)) {
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
      if (task_queue.empty())
        return;
      else {
        before_context = current_task.context;
        current_task = task_queue.top();
        task_queue.pop();
        context_switch(before_context.get(), current_task.context.get());
        return;
      };
    } else {
      return;
    }
  }
}