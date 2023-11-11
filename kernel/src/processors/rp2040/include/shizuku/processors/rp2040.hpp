#ifndef RP2040_HPP
#define RP2040_HPP
#include "functional"
#include "list"
#include "queue"
#include "shizuku.hpp"
#include "shizuku/processors.hpp"
#include "shizuku_entry.h"
#include "stdint.h"

struct shizuku::types::processors::rp2040::context {
  uint32_t r4, r5, r6, r7, r8, r9, r10, r11, r12;
  void *sp, *lr;
};
class shizuku::types::processors::rp2040::cpu_driver {
public:
  static int load_context(const int return_value, const context &context);
  static int save_context(context &context);
  static void context_switch(context &current, context &next);
  void context_switch();
  static void entry_func(void (*entry)(void), context &context);
  void add_task(shizuku::types::processors::rp2040::context &context,
                size_t time);
  void
  change_current_context(shizuku::types::processors::rp2040::context &context);

private:
  using task = struct task {
    shizuku::types::processors::rp2040::context *context;
    size_t remain_time;
  };
  std::deque<task> task_queue;
  shizuku::types::processors::rp2040::context default_context = context();
  task current_task = {.context = &default_context, .remain_time = 1};
};
#endif