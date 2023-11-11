
#include "shizuku/processors/rp2040.hpp"

void shizuku::types::processors::rp2040::cpu_driver::add_task(
    shizuku::types::processors::rp2040::context &context, size_t time) {
  task_queue.emplace_back(&context, time);
  return;
}