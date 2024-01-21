#include "shizuku/kernel.hpp"

using namespace shizuku::types;

void kernel::add_task(
    shizuku::platform::std::shared_ptr<shizuku::types::thread> &thread,
    size_t processing_time, int priority) {
  if (auto current_thread =
          this->cpu_manager[shizuku::types::cpu_manager::get_core_num()]
              .get_current_thread()
              .lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      if (current_object->attribute == super_object) {
        this->cpu_manager[shizuku::types::cpu_manager::get_core_num()].add_task(
            thread, processing_time, priority);
      }
    }
  }
};