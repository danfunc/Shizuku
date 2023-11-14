#ifndef RP2040_HPP
#define RP2040_HPP
#include "queue"
#include "shizuku/platform.hpp"
namespace shizuku {
namespace types {
namespace processors {
namespace rp2040 {
struct context;
class cpu_driver;
struct task {
  shizuku::types::processors::rp2040::context *context;
  size_t remain_time;
  int priority;
};

inline bool operator<(const task &x, const task &y) {
  return x.priority < y.priority;
}
} // namespace rp2040
} // namespace processors
} // namespace types
} // namespace shizuku

struct shizuku::types::processors::rp2040::context {
  uint32_t r4, r5, r6, r7, r8, r9, r10, r11, r12;
  void *sp, *lr;
};
class shizuku::types::processors::rp2040::cpu_driver {
public:
  static int load_context(const int return_value, const context &context);
  static int save_context(context &context);
  static void context_switch(context &current, context &next);
  static void entry_func(void (*entry)(void), context &context);
  void context_switch();
  void add_task(shizuku::types::processors::rp2040::context &context,
                size_t time);
  void
  change_current_context(shizuku::types::processors::rp2040::context &context);

private:
  shizuku::platform::std::priority_queue<task> task_queue;
  shizuku::types::processors::rp2040::context default_context = context();
  task current_task = {.context = &default_context, .remain_time = 1};
};

#endif