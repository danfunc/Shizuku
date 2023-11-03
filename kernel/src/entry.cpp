#include "shizuku/config.hpp"
#include "shizuku/processors/rp2040.hpp"
#include "shizuku_entry.h"
#include "stdio.h"
void sub_func();
shizuku::context sub_context, main_context;
void shizuku_entry(void) {
  shizuku::cpu_driver::entry_func(sub_func, sub_context);
  shizuku::cpu_driver::load_context(1, sub_context);
  return;
}

void sub_func() {
  while (1) {
    printf("sub");
  }
}