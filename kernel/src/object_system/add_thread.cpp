#include "shizuku/kernel.hpp"

int shizuku::types::kernel::add_thread(
    const shizuku::platform::std::string &name, int (*entry)(int, char **),
    int argc, char **argv) {
  this->object_tree[name];
  return 0;
};
