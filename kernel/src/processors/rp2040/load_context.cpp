
#include "shizuku.hpp"
#include "shizuku/processors/rp2040.hpp"

int shizuku::types::processors::rp2040::cpu_driver::load_context(
    context &context) {
  void *sp, *lr;
  uint32_t r4, r5, r6, r7, r8, r9, r10, r11, r12;
  r4 = context.r4;
  asm("mov r4, %0" : : "r"(r4));
  r5 = context.r5;
  asm("mov r5, %0" : : "r"(r5));
  r6 = context.r6;
  asm("mov r6, %0" : : "r"(r6));
  r7 = context.r7;
  asm("mov r7, %0" : : "r"(r7));
  r8 = context.r8;
  asm("mov r8, %0" : : "r"(r8));
  r9 = context.r9;
  asm("mov r9, %0" : : "r"(r9));
  r10 = context.r10;
  asm("mov r10, %0" : : "r"(r10));
  r11 = context.r11;
  asm("mov r11, %0" : : "r"(r11));
  r12 = context.r12;
  asm("mov r12, %0" : : "r"(r12));
  sp = context.sp;
  asm("mov sp, %0" : : "r"(sp));
  lr = context.lr;
  asm("mov lr, %0" : : "r"(lr));
  return context.return_value;
}