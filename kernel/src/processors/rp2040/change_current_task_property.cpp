#include "shizuku/processors/rp2040.hpp"
void shizuku::types::processors::rp2040::cpu_driver::change_current_context(
    shizuku::platform::std::shared_ptr<
        shizuku::types::processors::rp2040::context>
        context) {
  current_task.context.reset();
  current_task.context = context;

  return;
}

void shizuku::types::processors::rp2040::cpu_driver::change_current_object(
    shizuku::types::object &object) {
  return;
}