#ifndef SHIZUKU_THREAD_HPP
#define SHIZUKU_THREAD_HPP
#include "shizuku/method.hpp"
#include "shizuku/platform.hpp"
#include "shizuku/processor.hpp"
#include <atomic>
namespace shizuku {
namespace types {
struct object;
class kernel;
enum status {
  wait_queueing,
  wait_executing,
  executing,
  sleeping,
};
struct thread {
  shizuku::context_shared_ptr context;
  shizuku::object_weak_ptr parent_object;
  int priority;
  const size_t thread_id;
  shizuku::types::status status = wait_queueing;
  thread(method entry, size_t callee_object_num, size_t caller_object_num,
         size_t arg1, size_t arg2, size_t id, int priority = 0)
      : context(new shizuku::context(entry, callee_object_num,
                                     caller_object_num, arg1, arg2)),
        thread_id{id} {
    this->priority = priority;
  };
  thread(size_t thread_id)
      : context{new shizuku::context}, thread_id{thread_id} {};
};

} // namespace types
} // namespace shizuku

#endif