
#ifndef SHIZUKU_KERNEL_HPP
#define SHIZUKU_KERNEL_HPP
#include "map"
#include "set"
#include "shizuku/config.hpp"
#include "shizuku/namespaces.hpp"
#include "shizuku/object.hpp"
#include "string"
namespace shizuku {

namespace types {
class kernel {
private:
  shizuku::cpu_driver cpu_driver[shizuku::processor_count];
  shizuku::memory_manager memory_manager;
  shizuku::platform::std::set<shizuku::types::thread> thread_tree;
  void schedule();

public:
  kernel();
  ~kernel();
  void context_switch(void);
  void create_object(shizuku::platform::std::string name,
                     void (*entry)(int, char *[]), int argc, char *argv[]);
  void add_thread(shizuku::platform::std::string const &name,
                  int (*entry)(int, char *[]), int argc, char *argv[]);
  void add_thread(int (*entry)(int, char *[]), int argc, char *argv[]);
  void call_func(shizuku::platform::std::string const &object_name,
                 shizuku::platform::std::string const &func_name, int argc,
                 char *argv[]);
  void add_func(shizuku::platform::std::string const &name,
                int (*entry)(int, char *[]));
  shizuku::platform::std::map<shizuku::platform::std::string,
                              shizuku::platform::std::shared_ptr<object>>
      object_tree;
};

} // namespace types
extern types::kernel kernel;
} // namespace shizuku

#endif