#include "shizuku/cpu_manager.hpp"

void shizuku::types::cpu_manager::add_task(
    shizuku::platform::std::shared_ptr<shizuku::context> context,
    size_t processing_time, int priority) {
  task_queue.push(shizuku::types::task{.context = context,
                                       .remain_time = processing_time,
                                       .priority = priority});
}