#include "shizuku/kernel.hpp"

using namespace shizuku::types;

size_t kernel::create_object(shizuku::string const &name, method init_method,
                             size_t arg1, size_t arg2) {

  size_t target_id = ++total_object_count;
  size_t creator_id = 0;
  if (auto current_thread =
          cpu_manager[get_core_num()].get_current_thread().lock()) {
    if (auto current_object = current_thread->parent_object.lock()) {
      creator_id = current_object->object_id;
    }
  }
  object_tree.insert_or_assign(
      (size_t)target_id,
      shizuku::object_shared_ptr{new object{target_id, creator_id, init_method,
                                            arg1, arg2, super_object}});
  shizuku::object_shared_ptr created_object = object_tree[(size_t)target_id];
  created_object->thread_map[1]->parent_object = created_object;
  created_object->name = name;
  cpu_manager[get_core_num()].add_task(
      object_tree[(size_t)target_id]->thread_map[1], 1, 100);
  return target_id;
}