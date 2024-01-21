#include "cstdint"
#include "cstdlib"
#include "pico/stdlib.h"
#include "shizuku/processors/rp2040.hpp"

void shizuku::types::processors::rp2040::cpu_driver::entry_func(
    int (*entry)(int argc, char *argv[]), context *context, int argc,
    char *argv[]) {
  asm("mov r4,%0" ::"r"(entry) : "r4");
  asm("mov r5,%0" ::"r"(argc) : "r5");
  asm("mov r6,%0" ::"r"(argv) : "r6");
  constexpr size_t size = 2 * 1024;
  if (save_context(context) == 0) {
    void *sp = (void *)((int)malloc(size) + size);
    sp = (void *)((int)sp & ~0xfL);
    context->sp = sp;
    return;
  } else {
    asm("mov r7,#0" ::: "r7");
    asm("mov r8,r7" ::: "r8");
    asm("mov r9,r7" ::: "r9");
    asm("mov r10,r7" ::: "r10");
    asm("mov r11,r7" ::: "r11");
    asm("mov r12,r7" ::: "r12");
    asm("mov r0,r5");
    asm("mov r1,r6");
    asm("blx r4");
    while (true)
      ;
  }
}
