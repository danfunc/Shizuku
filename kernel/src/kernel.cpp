#include "shizuku/kernel.hpp"
#include "shizuku.hpp"
#include "shizuku/object.hpp"
namespace shizuku {
types::kernel::kernel() {}
types::kernel::~kernel() {}
types::kernel kernel;

void context_switch() { kernel.context_switch(); }

int types::kernel::add_thread(int (*entry)(int, char *[]), int argc,
                              char *argv[]) {
  if (auto object_pointer = cpu_manager[types::cpu_manager::get_core_num()]
                                .get_current_thread()
                                .lock()
                                ->parent_object) {
    auto thread_pointer = shizuku::platform::std::make_shared<types::thread>(
        entry, argc, argv, object_pointer);
    thread_pointer->thread_id = ++thread_count;
    object_pointer->threads.insert(thread_pointer);
    return thread_count;
  } else {
    return 0;
  }
}

void types::kernel::init() {};
void shizuku::types::kernel::exit(
    shizuku::platform::std::weak_ptr<shizuku::types::object> &parent) {}

int add_thread(int (*entry)(int, char **), int argc, char *argv[]) {
  return kernel.add_thread(entry, argc, argv);
};

void types::kernel::add_func(const std::string &name,
                             int (*entry)(int, char **)) {
  if (auto current_thread = get_current_thread().lock()) {
    current_thread->parent_object;
  }
}

void types::kernel::context_switch() {
  cpu_manager[types::cpu_manager::get_core_num()].context_switch();
}

} // namespace shizuku
