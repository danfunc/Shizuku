#include "shizuku/kernel.hpp"

using namespace shizuku::types;

size_t kernel::create_thread(method entry, size_t arg1, size_t arg2) {
  if (auto current_thread =
          this->cpu_manager[get_core_num()].get_current_thread().lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      return current_object->create_thread(entry, current_object->object_id,
                                           current_object->object_id, arg1,
                                           arg2);
    }
  }
  return 0;
}