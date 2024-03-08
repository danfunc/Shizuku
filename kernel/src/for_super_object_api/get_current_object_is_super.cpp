#include "shizuku/kernel.hpp"

namespace shizuku {
namespace types {
bool kernel_api_for_super_object::get_current_object_is_super() {
  if (auto current_thread = wrapping_kernel.get_current_thread().lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      if (current_object->attribute == super_object) {
        return true;
      }
    }
  }
  return false;
}
} // namespace types

} // namespace shizuku
