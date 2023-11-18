#include "map"
#include "shizuku/kernel.hpp"
namespace shizuku {
namespace types {
void kernel::create_object(shizuku::platform::std::string name,
                           void (*entry)(int, char *[]), int argc,
                           char *argv[]) {
  shizuku::types::object object = shizuku::types::object();
  shizuku::types::thread thread = shizuku::types::thread();
  thread.context = new shizuku::context;
  object.thread_map.insert(thread);
  object_tree.insert(object);
};
} // namespace types
} // namespace shizuku