
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
void shizuku_entry(void) {
  multi_thread_entry();
  return;
}

namespace shizuku {
types::object boot_object;
}
