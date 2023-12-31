#ifndef SHIZUKU_THREAD_HPP
#define SHIZUKU_THREAD_HPP
#include "memory"
#include "shizuku/platform.hpp"
#include "shizuku/processor.hpp"
namespace shizuku {
namespace types {
struct object;
class kernel;
struct thread {
  friend types::kernel;
  shizuku::platform::std::shared_ptr<shizuku::context> context;
  shizuku::types::object *parent_object;
  int priority;
  int thread_id;
  const bool operator<(shizuku::types::thread const &right) const {
    return this->thread_id < right.thread_id;
  };
  thread(const thread &&right) {
    this->context = right.context;
    this->parent_object = right.parent_object;
    this->priority = right.priority;
    this->thread_id = right.thread_id;
  }
  thread() {
    this->context = shizuku::platform::std::make_shared<shizuku::context>();
    this->parent_object =
        shizuku::platform::std::set<shizuku::types::object>::pointer();
    this->priority = 0;
    this->thread_id = 0;
  }
  thread(int (*entry)(int, char **), int argc, char **argv,
         shizuku::types::object *parent_object) {
    this->context = shizuku::platform::std::make_shared<shizuku::context>();
    shizuku::cpu_driver::entry_func(entry, this->context.get(), argc, argv);
    this->parent_object = parent_object;
    this->thread_id = 0;
  }
};

} // namespace types
} // namespace shizuku

static inline const int operator<=>(shizuku::types::thread const &x,
                                    shizuku::types::thread const &y) {
  return x.thread_id - y.thread_id;
}
#endif