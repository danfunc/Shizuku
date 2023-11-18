#include "shizuku/processors/rp2040.hpp"
void shizuku::types::processors::rp2040::cpu_driver::change_current_context(
    shizuku::types::processors::rp2040::context &context) {
  current_task.context = &context;
  return;
}

void shizuku::types::processors::rp2040::cpu_driver::change_current_object(
    shizuku::types::object &object) {
  current_task.object = &object;
  return;
}