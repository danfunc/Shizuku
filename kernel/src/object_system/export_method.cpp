
#include "shizuku/kernel.hpp"
using namespace shizuku::types;

void object::export_method(method entry, shizuku::string const &name) {
  method_map.insert_or_assign(
      name, shizuku::method_shared_ptr{new shizuku::types::method{entry}});
}

using namespace shizuku::types;

api_result<void> kernel::export_method(method entry,
                                       shizuku::string const &name) {
  if (auto current_thread = this->get_current_thread().lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      current_object->export_method(entry, name);
      return error_code::success;
    } else {
      return error_code::your_object_are_killed;
    }
  } else {
    return error_code::your_thread_are_killed;
  }
}
api_result<void> kernel::export_method(size_t object_id, method entry,
                                       shizuku::string const &name) {
  auto search_result = object_tree.find(object_id);
  if (search_result != object_tree.end()) {
    if (auto pointer = search_result->second) {
      pointer->export_method(entry, name);
      pointer.reset();
      return error_code::success;
    }
  }
  return error_code::no_such_object;
}