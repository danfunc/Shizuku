#include "shizuku/processors/rp2040.hpp"

void shizuku::types::processors::rp2040::cpu_driver::abort_current_task() {
  current_task.remain_time = 1;
  context_switch();
}