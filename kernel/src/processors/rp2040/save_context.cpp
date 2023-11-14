

#include "shizuku/kernel.hpp"
#include "shizuku/processors/rp2040.hpp"

int shizuku::types::processors::rp2040::cpu_driver::save_context(
    context &context) {
  void *sp, *lr;
  uint32_t r4, r5, r6, r7, r8, r9, r10, r11, r12;
  asm volatile("mov %0, r4" : "=r"(r4));
  context.r4 = r4;
  asm volatile("mov %0, r5" : "=r"(r5));
  context.r5 = r5;
  asm volatile("mov %0, r6" : "=r"(r6));
  context.r6 = r6;
  asm volatile("mov %0, r7" : "=r"(r7));
  context.r7 = r7;
  asm volatile("mov %0, r8" : "=r"(r8));
  context.r8 = r8;
  asm volatile("mov %0, r9" : "=r"(r9));
  context.r9 = r9;
  asm volatile("mov %0, r10" : "=r"(r10));
  context.r10 = r10;
  asm volatile("mov %0, r11" : "=r"(r11));
  context.r11 = r11;
  asm volatile("mov %0, r12" : "=r"(r12));
  context.r12 = r12;
  asm volatile("mov %0, sp" : "=r"(sp) :);
  context.sp = sp;
  asm volatile("mov %0, lr" : "=r"(lr));
  context.lr = lr;
  return 0;
}
