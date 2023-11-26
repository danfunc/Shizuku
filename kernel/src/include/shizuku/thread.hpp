#ifndef SHIZUKU_THREAD_HPP
#define SHIZUKU_THREAD_HPP
#include "memory"
#include "shizuku/platform.hpp"
#include "shizuku/processor.hpp"
namespace shizuku {
namespace types {
struct thread {
  shizuku::platform::std::shared_ptr<shizuku::context> context;
  int priority;
  int thread_id;
  const bool operator<(shizuku::types::thread const &right) const {
    return this->thread_id < right.thread_id;
  };
};

} // namespace types
} // namespace shizuku

static inline const bool operator<(shizuku::types::thread const &x,
                                   shizuku::types::thread const &y) {
  return x.thread_id < y.thread_id;
}
#endif