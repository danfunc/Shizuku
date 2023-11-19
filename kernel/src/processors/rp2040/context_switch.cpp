
#include "shizuku/processors/rp2040.hpp"

void shizuku::types::processors::rp2040::cpu_driver::context_switch(
    shizuku::platform::std::shared_ptr<context> current,
    shizuku::platform::std::shared_ptr<context> next) {
  if (save_context(current) == 0) {
    load_context(1, next);
  }
  return;
}

void shizuku::types::processors::rp2040::cpu_driver::context_switch() {
  if (current_task.remain_time == 0) {
    return;
  } else if ((--current_task.remain_time) == 0) {

    if (task_queue.empty()) {
      ++current_task.remain_time;
      return;
    } else {
      shizuku::platform::std::shared_ptr<
          shizuku::types::processors::rp2040::context>
          current_context, next_context;
      if (auto locked_context = current_task.context.lock())
        current_context = shizuku::platform::std::move(locked_context);
      else
        current_context = shizuku::platform::std::make_shared<
            shizuku::types::processors::rp2040::context>();
      if (auto locked_context = task_queue.top().context.lock()) {
        next_context = shizuku::platform::std::move(locked_context);
        current_task = task_queue.top();
        task_queue.pop();
        context_switch(current_context, next_context);
        return;
      } else {
        task_queue.pop();
        current_task.remain_time = 1;
        return;
      }
    }
  } else {
    return;
  }
}
