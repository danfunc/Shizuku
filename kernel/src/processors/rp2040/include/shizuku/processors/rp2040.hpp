#ifndef RP2040_HPP
#define RP2040_HPP
#include "functional"
#include "list"
#include "queue"
#include "shizuku/processors.hpp"
#include "shizuku_entry.h"
#include "stdint.h"
#include "vector"

struct shizuku::types::processors::rp2040::context {
  uint32_t r4, r5, r6, r7, r8, r9, r10, r11, r12;
  void *sp, *lr;
};
struct shizuku::types::processors::rp2040::cpu_driver {
  static int load_context(const int return_value, const context &context);
  static int save_context(context &context);
  static void context_switch(context &current, context &next);
  void context_switch();
  static void entry_func(void (*entry)(void), context &context);
  static void create_thread(void (*entry)(void));
  void add_context(context &context);
  std::list<context> context_list;
  std::list<context>::iterator current_context;
  void add_task(shizuku::types::processors::rp2040::context context,
                size_t time);
  using task = struct task {
    shizuku::types::processors::rp2040::context *context;
    size_t remain_time;
  };
  std::queue<task> task_queue;
  task current_task = {.context = nullptr, .remain_time = 0};
};
#endif