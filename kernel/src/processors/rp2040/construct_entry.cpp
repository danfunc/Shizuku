#include "shizuku/kernel.hpp"
#include "shizuku/processors/rp2040.hpp"
void shizuku::types::processors::rp2040::cpu_driver::construct_entry_context(
    void *caller_arg1, void *caller_arg2,
    int (*entry)(void *arg1, void *arg2,
                 ::shizuku::thread_weak_ptr &current_thread_ptr,
                 ::shizuku::object_weak_ptr &caller_object_ptr),
    shizuku::types::processors::rp2040::context *context) {
  asm("mov r4,%0" ::"r"(entry) : "r4");
  asm("mov r5,%0" ::"r"(caller_arg1) : "r5");
  asm("mov r6,%0" ::"r"(caller_arg2) : "r6");
  constexpr size_t size = 2 * 1024;
  if (save_context(context) == 0) {
    context->r4 = (uint32_t)entry;
    context->r5 = (uint32_t)caller_arg1;
    context->r6 = (uint32_t)caller_arg2;
    if (auto thread_ptr = shizuku::kernel.get_current_thread().lock()) {
      context->r7 = (uint32_t)(&(thread_ptr->parent_object));
    }
    void *sp = (void *)((int)malloc(size) + size);
    sp = (void *)((int)sp & ~0xfL);
    context->sp = sp;
    return;
  } else {
    asm("mov r0,r5");
    asm("mov r1,r6");
    asm("mov r3,r7");
    asm("mov r7,#0" ::: "r7");
    asm("mov r8,r7" ::: "r8");
    asm("mov r9,r7" ::: "r9");
    asm("mov r10,r7" ::: "r10");
    asm("mov r11,r7" ::: "r11");
    asm("mov r12,r7" ::: "r12");
    asm("blx r4");
    while (true)
      ;
  }
}