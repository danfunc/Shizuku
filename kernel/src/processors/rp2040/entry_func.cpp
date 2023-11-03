#include "cstdlib"
#include "shizuku.hpp"
#include "shizuku/processors/rp2040.hpp"
#include "stdlib.h"

void shizuku::types::processors::rp2040::cpu_driver::entry_func(
    void (*entry)(), context &context) {
  if (save_context(context) != 0) {
    entry();
    while (true)
      ;
    // no return
  } else {
    void *sp = (void *)((((int)((char *)(malloc(8 * 64)) + 8 * 64) / 8)) * 8);
    context.sp = sp;
    return;
  }
}