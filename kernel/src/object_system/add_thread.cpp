#include "shizuku/kernel.hpp"
/*
void shizuku::types::kernel::add_thread(
    shizuku::platform::std::string const &name, int (*entry)(int, char *[]),
    int argc, char *argv[]) {
  shizuku::platform::std::shared_ptr<shizuku::context> context =
      shizuku::platform::std::make_shared<shizuku::context>();
  cpu_driver::entry_func(entry, context.get(), argc, argv);
  thread_tree.insert(shizuku::types::thread{
      .context = context, .priority = 0, .thread_id = 0});
}*/