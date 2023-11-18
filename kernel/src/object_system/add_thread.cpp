#include "shizuku/kernel.hpp"
void shizuku::types::kernel::add_thread(
    shizuku::platform::std::string const &name, int (*entry)(int, char *[]),
    int argc, char *argv[]) {
  shizuku::context *context = new shizuku::context();
  cpu_driver::entry_func(entry, *context, argc, argv);
  object_tree[name].thread_map.insert(shizuku::types::thread(
      shizuku::platform::std::shared_ptr<shizuku::context>(context), 0));
}