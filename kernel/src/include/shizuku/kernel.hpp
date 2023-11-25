
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
  shizuku::platform::std::map<shizuku::platform::std::string, object>
      object_tree;
  shizuku::platform::std::set<shizuku ::types::thread> thread_tree;
  void enqueue();

public:
  kernel();
  ~kernel();
  void context_switch(void);
  void create_object(shizuku::platform::std::string name,
                     void (*entry)(int, char *[]), int argc, char *argv[]);
  void add_thread(shizuku::platform::std::string const &name,
                  int (*entry)(int, char *[]), int argc, char *argv[]);
  int call_func(shizuku::platform::std::string const &object_name,
                shizuku::platform::std::string const &func_name, int argc,
                char *argv[]);
  void add_func(shizuku::platform::std::string const &name,
                int (*entry)(int, char *[]));
};

} // namespace types
extern types::kernel kernel;
} // namespace shizuku

#endif