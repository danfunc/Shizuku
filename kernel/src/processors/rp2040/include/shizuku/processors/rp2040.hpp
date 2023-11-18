#ifndef RP2040_HPP
#define RP2040_HPP
#include "memory"
#include "queue"
#include "shizuku/platform.hpp"
#ifdef SIO_CPUID_OFFSET
#undef SIO_CPUID_OFFSET
#endif
#ifdef SIO_BASE
#undef SIO_BASE
#endif
#define SIO_BASE (0xd0000000)         // imported from pico_sdk
#define SIO_CPUID_OFFSET (0x00000000) // imported from pico_sdk

namespace shizuku {
namespace types {
struct object;
namespace processors {
namespace rp2040 {
struct context;
class cpu_driver;
struct task {
  shizuku::types::processors::rp2040::context *context;
  types::object *object;
  size_t remain_time;
  int priority;
};

static inline bool operator<(const task &x, const task &y) {
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
private:
  shizuku::platform::std::priority_queue<task> task_queue;
  shizuku::types::processors::rp2040::context default_context = context();
  task current_task = {.context = &default_context,
                       .object = nullptr,
                       .remain_time = 1,
                       .priority = 0};

public:
  static int load_context(const int return_value, const context &context);
  static int save_context(context &context);
  static void context_switch(context &current, context &next);
  static void entry_func(int (*entry)(int argc, char *argv[]), context &context,
                         int argc, char *argv[]);
  static unsigned int get_core_num() {
    return (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET));
  };
  void context_switch();
  void add_task(shizuku::types::processors::rp2040::context &context,
                size_t time = 1, int priority = 0);
  void add_task(shizuku::types::processors::rp2040::context &context,
                object &object, size_t time = 1, int priority = 0);
  void
  change_current_context(shizuku::types::processors::rp2040::context &context);
  void change_current_object(shizuku::types::object &object);
  context *get_current_context(void);
  object *get_current_object(void);
  void abort_current_task();
};

#endif