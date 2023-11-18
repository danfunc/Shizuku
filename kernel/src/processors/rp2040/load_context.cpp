

#include "shizuku/processors/rp2040.hpp"

int shizuku::types::processors::rp2040::cpu_driver::load_context(
    const int return_value, const context &context) {
  void *sp, *lr;
  uint32_t r4, r5, r6, r7, r8, r9, r10, r11, r12;
  r4 = context.r4;
  asm volatile("mov r4, %0" : : "r"(r4));
  r5 = context.r5;
  asm volatile("mov r5, %0" : : "r"(r5));
  r6 = context.r6;
  asm volatile("mov r6, %0" : : "r"(r6));
  r7 = context.r7;
  asm volatile("mov r7, %0" : : "r"(r7));
  r8 = context.r8;
  asm volatile("mov r8, %0" : : "r"(r8));
  r9 = context.r9;
  asm volatile("mov r9, %0" : : "r"(r9));
  r10 = context.r10;
  asm volatile("mov r10, %0" : : "r"(r10));
  r11 = context.r11;
  asm volatile("mov r11, %0" : : "r"(r11));
  r12 = context.r12;
  asm volatile("mov r12, %0" : : "r"(r12));
  sp = context.sp;
  asm volatile("mov sp, %0" : : "r"(sp));
  lr = context.lr;
  asm volatile("mov lr, %0" : : "r"(lr));
  if (return_value != 0) {
    return return_value;
  } else {
    return 1;
  }
}