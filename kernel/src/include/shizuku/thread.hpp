#ifndef SHIZUKU_THREAD_HPP
#define SHIZUKU_THREAD_HPP
#include "memory"
#include "shizuku/platform.hpp"
#include "shizuku/processor.hpp"
namespace shizuku {
namespace types {
struct object;
struct thread {
  shizuku::platform::std::shared_ptr<shizuku::context> context;
  shizuku::platform::std::weak_ptr<shizuku::types::object> parent_object;
  int priority;
  int thread_id;
  const bool operator<(shizuku::types::thread const &right) const {
    return this->thread_id < right.thread_id;
  };
  thread(const thread &right) {
    this->context = right.context;
    this->parent_object = right.parent_object;
    this->priority = right.priority;
    this->thread_id = right.thread_id;
  }
  thread() {
    this->context = shizuku::platform::std::make_shared<shizuku::context>();
    this->parent_object =
        shizuku::platform::std::shared_ptr<shizuku::types::object>();
    this->priority = 0;
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