
#include "memory"
#include "shizuku/kernel.hpp"
#include "shizuku/processors/rp2040.hpp"
void shizuku::types::processors::rp2040::cpu_driver::add_task(
    shizuku::platform::std::weak_ptr<
        shizuku::types::processors::rp2040::context>
        context,
    size_t time, int priority) {
  task_queue.emplace(
      context, shizuku::platform::std::make_shared<shizuku::types::object>(),
      time, priority);
  return;
}
void shizuku::types::processors::rp2040::cpu_driver::add_task(
    shizuku::platform::std::weak_ptr<
        shizuku::types::processors::rp2040::context>
        context,

    shizuku::platform::std::weak_ptr<object> parent, size_t time,
    int priority) {
  task_queue.emplace(context, parent, time, priority);
  return;
}