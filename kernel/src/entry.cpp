
#include "pico/stdlib.h"
#include "shizuku/config.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/object.hpp"
#include "shizuku/processors/rp2040.hpp"
#include "shizuku_entry.h"
#include "stdio.h"
void sub_func();
void mmu_test_entry();
void multi_thread_entry();
void object_system_test_main();
void shizuku_entry(size_t argc, char **argv, method init_obj_entry) {

  object_system_test_main();
  //  shizuku::kernel.init();
  //  object_system_test_main();
  return;
}
