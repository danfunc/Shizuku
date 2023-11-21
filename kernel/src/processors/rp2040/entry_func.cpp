#include "cstdlib"
#include "pico/stdlib.h"
#include "shizuku/processors/rp2040.hpp"

/*this is secure version but not test and disable. if you enable this
version, please comment out there.*/
void shizuku::types::processors::rp2040::cpu_driver::entry_func(
    int (*entry)(int argc, char *argv[]), context *context, int argc,
    char *argv[]) { /*
   asm("mov r4,%0" ::"r"(entry) : "r4");
   asm("mov r5,%0" ::"r"(argc) : "r5");
   asm("mov r6,%0" ::"r"(argv) : "r6");*/
  if (save_context(context) == 0) {
    void *sp = (void *)((int)malloc(1 * 1024) + 1 * 1024);
    sp = (void *)((int)sp & ~0xfL);
    context->sp = sp;
    return;
  } else { /*
     asm("mov r7,#0" ::: "r7");
     asm("mov r8,r7" ::: "r8");
     asm("mov r9,r7" ::: "r9");
     asm("mov r10,r7" ::: "r10");
     asm("mov r11,r7" ::: "r11");
     asm("mov r12,r7" ::: "r12");
     asm("mov r0,r5");
     asm("mov r1,r6");
     asm("blx r4");*/
    entry(argc, argv);
    while (true)
      ;
  }
}
