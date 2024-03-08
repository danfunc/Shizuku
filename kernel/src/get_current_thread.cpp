#include "shizuku/kernel.hpp"
using namespace shizuku::types;

shizuku::thread_weak_ptr kernel::get_current_thread() {
  return this->cpu_manager[cpu_manager::get_core_num()].get_current_thread();
}