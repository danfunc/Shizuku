
#include "shizuku/processors/rp2040.hpp"

void shizuku::types::processors::rp2040::cpu_driver::context_switch(
    context &current, context &next) {
  if (save_context(current) == 0) {
    load_context(1, next);
  }
  return;
}

void shizuku::types::processors::rp2040::cpu_driver::context_switch() {
  if (current_task.remain_time == 0) {
    return;
  } else if ((--current_task.remain_time) == 0) {
    if (current_task.context == nullptr)
      current_task.context = new shizuku::types::processors::rp2040::context();
    context &current_context = *current_task.context;
    if (task_queue.empty()) {
      ++current_task.remain_time;
      return;
    } else {
      context *current = current_task.context, *next = task_queue.top().context;
      current_task = task_queue.top();

      task_queue.pop();
      if (next == nullptr)
        return;
      else
        context_switch(*current, *next);
    }
  } else {
    return;
  }
}
