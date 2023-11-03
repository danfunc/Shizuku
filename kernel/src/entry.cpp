#include "shizuku/config.hpp"
#include "shizuku/processors/rp2040.hpp"
#include "shizuku_entry.h"
#include "stdio.h"
void sub_func();
shizuku::context sub_context, main_context;
void shizuku_entry(void) {
  if (shizuku::cpu_driver::save_context(main_context) == 0) {
    shizuku::cpu_driver::entry_func(sub_func, sub_context);
  }
  while (1) {
    if (shizuku::cpu_driver::save_context(main_context) == 0) {
      printf("main");
      shizuku::cpu_driver::load_context(sub_context);
    }
  }
  return;
}

void sub_func() {
  while (1) {
    if (shizuku::cpu_driver::save_context(sub_context) == 0) {
      printf("sub");
      shizuku::cpu_driver::load_context(main_context);
    }
  }
}