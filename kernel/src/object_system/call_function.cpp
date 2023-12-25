#include "map"
#include "shizuku/kernel.hpp"
namespace shizuku {
namespace types {
int kernel::call_func(shizuku::platform::std::string const &object_name,
                      shizuku::platform::std::string const &func_name, int argc,
                      char *argv[]) {
  auto entry_point =
      this->object_tree[object_name]->functions.find(func_name)->entry_point;
  auto thread = shizuku::platform::std::make_shared<shizuku::types::thread>(
      (int (*)(int, char **))entry_point, argc, argv,
      this->object_tree[object_name]);
  this->object_tree[object_name]->add_thread(thread); // 安全じゃない！！！
  this->cpu_manager[shizuku::cpu_driver::get_core_num()].add_task(thread, 0,
                                                                  100);
  return ++this->thread_count;
}
} // namespace types
} // namespace shizuku