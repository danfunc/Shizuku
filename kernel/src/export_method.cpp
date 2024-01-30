#include "shizuku/kernel.hpp"

using namespace shizuku::types;

size_t kernel::export_method(method entry, shizuku::string const &name) {
  if (auto current_thread = this->get_current_thread().lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      size_t object_id = current_object->object_id;
      return export_method(object_id, entry, name);
    }
  }
  return 0;
}
size_t kernel::export_method(size_t object_id, method entry,
                             shizuku::string const &name) {
  object_tree[object_id]->export_method(entry, name);
  return object_id;
}