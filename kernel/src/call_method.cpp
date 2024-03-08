#include "shizuku/kernel.hpp"

using namespace shizuku::types;

int kernel::call_method(size_t callee_object_num,
                        shizuku::string const &method_name, size_t arg1,
                        size_t arg2) {
  if (auto caller_thread = this->get_current_thread().lock()) {
    if (auto caller_object = caller_thread->parent_object.lock()) {
      return this->call_method(callee_object_num, caller_object->object_id,
                               method_name, arg1, arg2);
    }
  }
  return 0;
}

int kernel::call_method(size_t callee_object_num, size_t caller_object_num,
                        shizuku::string const &method_name, size_t arg1,
                        size_t arg2) {
  return (*(object_tree[callee_object_num]->method_map[method_name].get()))(
      callee_object_num, caller_object_num, arg1, arg2);
}
