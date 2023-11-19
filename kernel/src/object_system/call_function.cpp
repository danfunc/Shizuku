#include "map"
#include "shizuku/kernel.hpp"
namespace shizuku {
namespace types {
void kernel::call_func(shizuku::platform::std::string const &object_name,
                       shizuku::platform::std::string const &func_name,
                       int argc, char *argv[]) {
  shizuku::platform::std::shared_ptr<shizuku::context> context =
      shizuku::platform::std::make_shared<shizuku::context>();
  cpu_driver::entry_func(object_tree[object_name]->functions[func_name],
                         context, argc, argv);
  object_tree[object_name]->thread_map.insert(
      thread(shizuku::platform::std::shared_ptr<shizuku::context>(context), 0));
  cpu_driver[cpu_driver::get_core_num()].get_current_object();
}
} // namespace types
} // namespace shizuku