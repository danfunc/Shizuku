#include "map"
#include "shizuku/kernel.hpp"
namespace shizuku {
namespace types {
void kernel::create_object(shizuku::platform::std::string name,
                           void (*entry)(int, char *[]), int argc,
                           char *argv[]) {
  shizuku::platform::std::unique_ptr<shizuku::types::object> object{};
  shizuku::types::thread thread = shizuku::types::thread();
  thread.context = shizuku::platform::std::make_shared<shizuku::context>();
  object->thread_map.insert(thread);
  object->name = name;
  object_tree[name] = shizuku::platform::std::move(object);
};
} // namespace types
} // namespace shizuku