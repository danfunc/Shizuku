#include "shizuku/kernel.hpp"

using namespace shizuku::types;

size_t kernel::export_method(method entry, shizuku::string const &name) {
  if (auto current_thread =
          cpu_manager[get_core_num()].get_current_thread().lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      current_object->export_method(entry, name);
      return current_object->object_id;
    }
  }
  return 0;
}
size_t kernel::export_method(size_t object_id, method entry,
                             shizuku::string const &name) {
  object_tree[object_id]->export_method(entry, name);
  return object_id;
}