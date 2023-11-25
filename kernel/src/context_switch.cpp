#include "shizuku/kernel.hpp"

void shizuku::types::kernel::context_switch() {
  this->cpu_manager[shizuku::types::cpu_manager::get_core_num()]
      .context_switch();
}