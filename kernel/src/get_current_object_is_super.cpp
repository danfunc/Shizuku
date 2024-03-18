#include <shizuku/kernel.hpp>

bool shizuku::types::kernel::get_current_object_is_super() {
  if (auto current_thread = get_current_thread().lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      if (current_object->attribute == super_object) {
        return true;
      }
    }
  }
  return false;
}