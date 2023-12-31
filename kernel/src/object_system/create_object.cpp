#include "map"
#include "shizuku/kernel.hpp"
namespace shizuku {
namespace types {
void kernel::create_object(shizuku::platform::std::string name,
                           void (*entry)(int, char *[]), int argc,
                           char *argv[]) {
  shizuku::platform::std::set<shizuku::types::object>::iterator result =
      this->object_tree.emplace(name).first;
  auto thread = shizuku::platform::std::make_shared<shizuku::types::thread>(
      (int (*)(int, char *[]))entry, argc, argv,
      (shizuku::types::object *)&(*result));
  ((shizuku::types::object *)&(*result))->add_thread(thread);
  this->cpu_manager[shizuku::cpu_driver::get_core_num()].add_task(thread, 0,
                                                                  102);
}
} // namespace types
} // namespace shizuku