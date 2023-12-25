
#ifndef SHIZUKU_KERNEL_HPP
#define SHIZUKU_KERNEL_HPP
#include "map"
#include "set"
#include "shizuku/config.hpp"
#include "shizuku/cpu_manager.hpp"
#include "shizuku/namespaces.hpp"
#include "shizuku/object.hpp"
#include "string"
namespace shizuku {
namespace types {
class kernel {
private:
  shizuku::types::cpu_manager cpu_manager[shizuku::processor_count];
  shizuku::memory_manager memory_manager;
  shizuku::types::object_tree object_tree;
  size_t thread_count = 0;

  int add_thread(shizuku::platform::std::string const &name,
                 int (*entry)(int, char *[]), int argc, char *argv[]);

public:
  void recharge_slice();
  kernel();
  ~kernel();
  void context_switch(void);
  void create_object(shizuku::platform::std::string name,
                     void (*entry)(int, char *[]), int argc, char *argv[]);
  void create_object(shizuku::platform::std::string name,
                     void (*entry)(int, char *[]));
  int add_thread(int (*entry)(int, char *[]), int argc, char *argv[]);
  int call_func(shizuku::platform::std::string const &object_name,
                shizuku::platform::std::string const &func_name, int argc,
                char *argv[]);
  void add_func(shizuku::platform::std::string const &name,
                int (*entry)(int, char *[]));
  inline shizuku::platform::std::weak_ptr<shizuku::types::thread>
  get_current_thread() {
    return this->cpu_manager[shizuku::types::cpu_manager::get_core_num()]
        .get_current_thread();
  };
  inline void
  add_task(shizuku::platform::std::shared_ptr<shizuku::types::thread> &thread,
           size_t processing_time, int priority) {
    this->cpu_manager[shizuku::types::cpu_manager::get_core_num()].add_task(
        thread, processing_time, priority);
  };
  inline void abort_current_task() {
    this->cpu_manager[shizuku::types::cpu_manager::get_core_num()]
        .abort_current_task();
  }
  void init();
  void exit();
  void exit(shizuku::platform::std::weak_ptr<shizuku::types::object> &parent);
  void exit(int exit_cord);
};

} // namespace types

extern types::kernel kernel;

} // namespace shizuku

#endif