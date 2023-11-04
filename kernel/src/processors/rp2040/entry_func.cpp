#include "cstdlib"
#include "pico/stdlib.h"
#include "shizuku.hpp"
#include "shizuku/processors/rp2040.hpp"
#include "stdio.h"
#include "stdlib.h"

void shizuku::types::processors::rp2040::cpu_driver::entry_func(
    void (*entry)(), context &context) {
  void *sp = (void *)((int)malloc(4 * 1024) + 4 * 1024);
  sp = (void *)((int)sp & ~0xfL);
  if (save_context(context) == 0) {
    context.sp = sp;
    return;
  } else {
    entry();
    while (true)
      ;
  }
}