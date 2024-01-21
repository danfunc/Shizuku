
#ifndef SHIZUKU_KERNEL_HPP
#define SHIZUKU_KERNEL_HPP
#include "cstdint"
#include "shizuku/check_config.hpp"
#include "shizuku/config.hpp"
#include "shizuku/cpu_manager.hpp"
#include "shizuku/namespaces.hpp"
#include "shizuku/object.hpp"
namespace shizuku {
int kernel_init_method(
    size_t, size_t, size_t,
    size_t); // arg1 = mode(0=kernel_initialize 1=cpu_manager_initialize)
             // arg2 = core_num(mode=1 only)
namespace types {
class kernel {
private:
  friend int shizuku::kernel_init_method(size_t, size_t, size_t, size_t);
  shizuku::types::cpu_manager cpu_manager[shizuku::processor_count];
  shizuku::memory_manager memory_manager;
  shizuku::map<size_t, shizuku::object_shared_ptr> object_tree;
  size_t total_object_count = 0;
  shizuku::object_shared_ptr kernel_object;
  int create_object(shizuku::string name, uint32_t creator_object_id,
                    method init_method, uint32_t arg1, uint32_t arg2);

public:
  kernel()
      : kernel_object{new shizuku::types::object{
            0, 0, shizuku::kernel_init_method, 0, 0, super_object}} {
    for (size_t i = 0; i < shizuku::processor_count; i++) {
      kernel_object->total_thread_count++;
      kernel_object->thread_map[(size_t)kernel_object->total_thread_count] =
          shizuku::thread_shared_ptr{
              new shizuku::types::thread(kernel_init_method, 0, 0, 1, i,
                                         kernel_object->total_thread_count)};
      cpu_manager[i].set_default_thread(
          kernel_object->thread_map[kernel_object->total_thread_count]);
    }
  };
  int call_method(size_t callee_object_num, shizuku::string const &method_name,
                  size_t arg1, size_t arg2);
  int call_method(size_t callee_object_num, size_t caller_object_num,
                  shizuku::string const &method_name, size_t arg1, size_t arg2);
  inline int call_method(size_t arm_num, size_t arg1, size_t arg2);
  int call_armed_method(size_t arm_num, size_t arg1, size_t arg2);
  void context_switch(void);
  void exit(int exit_cord);
  size_t create_thread(method entry, size_t arg1, size_t arg2);
  size_t create_object(shizuku::string const &name, method init_method,
                       size_t arg1, size_t arg2);
  inline void abort_current_task() {
    this->cpu_manager[shizuku::types::cpu_manager::get_core_num()]
        .abort_current_task();
  };
  size_t get_core_num() { return types::cpu_manager::get_core_num(); };
  size_t export_method(method entry, shizuku::string const &name);
  size_t export_method(size_t object_id, method entry,
                       shizuku::string const &name);

public:
  // super_object_only
  shizuku::thread_weak_ptr get_current_thread();
  void
  add_task(shizuku::platform::std::shared_ptr<shizuku::types::thread> &thread,
           size_t processing_time, int priority);
};

} // namespace types

extern types::kernel kernel;

} // namespace shizuku

// inline function definitions

namespace shizuku {
namespace types {
int shizuku::types::kernel::call_method(size_t arm_num, size_t arg1,
                                        size_t arg2) {
  return call_armed_method(arm_num, arg1, arg2);
};
} // namespace types
} // namespace shizuku

#endif