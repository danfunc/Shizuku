#include "shizuku/kernel.hpp"
using namespace shizuku::types;

shizuku::thread_weak_ptr kernel::get_current_thread() {
  if (auto current_thread = this->cpu_manager[cpu_manager::get_core_num()]
                                .get_current_thread()
                                .lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      if (current_object->attribute == super_object)
        return current_thread;
    }
  }
  return shizuku::thread_weak_ptr{};
}