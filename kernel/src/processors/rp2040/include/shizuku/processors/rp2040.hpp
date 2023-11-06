#ifndef RP2040_HPP
#define RP2040_HPP
#include "functional"
#include "list"
#include "shizuku/processors.hpp"
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
  std::list<shizuku::types::processors::rp2040::context> context_list;
  std::list<shizuku::types::processors::rp2040::context>::iterator current;
};
#endif