#include "cstdlib"
#include "pico/stdlib.h"
#include "shizuku.hpp"
#include "shizuku/processors/rp2040.hpp"
#include "stdio.h"
#include "stdlib.h"

void shizuku::types::processors::rp2040::cpu_driver::entry_func(
    void (*entry)(), context &context) {
  register void (*entry_point)() = entry;
  void *sp = malloc(8 * 1024) + 8 * 1024;

  if (save_context(context) == 0) {
    context.sp = sp;
    return;
  } else {
    entry_point();
    while (true)
      ;
  }
}