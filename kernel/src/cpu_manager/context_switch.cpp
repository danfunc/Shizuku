
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
      before_context = current_context;
      current_task.thread.reset();
      if (task_queue.empty()) {
        current_task.thread = default_thread;
        current_task.remain_time = 0;
        current_task.priority = 0;
      } else {
        while (true) {
          if (auto next_thread = task_queue.top().thread.lock()) {
            break;
          }
        }

        current_task = task_queue.top();
        task_queue.pop();
        current_context =
            current_task.thread.lock()
                ->context; // スレッドが死なないことが前提になってる。要リファクタリング
      };
      context_switch(before_context.get(), current_context.get());
      before_context.reset();
      return;
    } else {
      return;
    }
  }
}