#include "shizuku/kernel.hpp"

using namespace shizuku::types;

void kernel::delete_exported_method(::shizuku::string const &name) {
  if (auto current_thread = this->get_current_thread().lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      current_object->method_map.erase(name);
    }
  }
}