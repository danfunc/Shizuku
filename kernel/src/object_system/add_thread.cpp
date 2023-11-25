#include "shizuku/kernel.hpp"
void shizuku::types::kernel::add_thread(
    shizuku::platform::std::string const &name, int (*entry)(int, char *[]),
    int argc, char *argv[]) {
  shizuku::context *context = new shizuku::context();
  cpu_driver::entry_func(entry, context, argc, argv);
}