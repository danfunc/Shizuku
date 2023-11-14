#include "cstdlib"
#include "pico/stdlib.h"
#include "shizuku/kernel.hpp"
#include "shizuku/processors/rp2040.hpp"
#include "stdio.h"
#include "stdlib.h"

/*this is secure version but not test and disable. if you enable this
version, please comment out there.*/
void shizuku::types::processors::rp2040::cpu_driver::entry_func(
    void (*entry)(), context &context) {
  void *sp = (void *)((int)malloc(1 * 1024) + 1 * 1024);
  sp = (void *)((int)sp & ~0xfL);
  if (save_context(context) == 0) {
    context.sp = sp;
    return;
  } else {
    asm("mov r4,#0" ::: "r4");
    asm("mov r5,%0" ::"r"(entry) : "r5");
    asm("mov r6,#0" ::: "r6");
    asm("mov r7,#0" ::: "r7");
    asm("mov r8,r4" ::: "r8");
    asm("mov r9,r4" ::: "r9");
    asm("mov r10,r4" ::: "r10");
    asm("mov r11,r4" ::: "r11");
    asm("mov r12,r4" ::: "r12");
    asm("blx r5");
    while (true)
      ;
  }
}
