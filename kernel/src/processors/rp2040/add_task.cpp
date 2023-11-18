
#include "shizuku/processors/rp2040.hpp"

void shizuku::types::processors::rp2040::cpu_driver::add_task(
    shizuku::types::processors::rp2040::context &context, size_t time,
    int priority) {
  task_queue.emplace(&context, nullptr, time, priority);
  return;
}
void shizuku::types::processors::rp2040::cpu_driver::add_task(context &context,

                                                              object &parent,
                                                              size_t time,
                                                              int priority) {
  task_queue.emplace(&context, &parent, time, priority);
  return;
}