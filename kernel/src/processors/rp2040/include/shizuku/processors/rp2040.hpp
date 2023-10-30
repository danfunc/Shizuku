#ifndef RP2040_HPP
#define RP2040_HPP
#include "shizuku/processors.hpp"
#include "stdint.h"
struct shizuku::types::processors::rp2040::context {
  uint32_t r4, r5, r6, r7, r8, r9, r10, r11, r12, return_value;
  void *sp, *lr;
};
struct shizuku::types::processors::rp2040::cpu_driver {
  int load_context(context &context);
  int save_context(context &context, int return_value);
};
#endif