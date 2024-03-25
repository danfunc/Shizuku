#include "shizuku/kernel.hpp"

using namespace shizuku::types;

api_result<size_t> kernel::create_thread(method entry, size_t arg1,
                                         size_t arg2) {
  if (auto current_thread = this->get_current_thread().lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      shizuku::thread_shared_ptr created_thread =
          current_object->create_thread(entry, current_object->object_id,
                                        current_object->object_id, arg1, arg2);
      created_thread->parent_object = current_object;
      return created_thread->thread_id;
    } else {
      return error_code::your_object_are_killed;
    }
  } else {
    return error_code::your_thread_are_killed;
  }
}