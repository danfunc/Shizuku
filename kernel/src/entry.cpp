#include "shizuku/config.hpp"
#include "shizuku/object.hpp"
#include "shizuku/processors/rp2040.hpp"
#include "shizuku_entry.h"
#include "stdio.h"
void sub_func();
shizuku::context sub_context, main_context;
shizuku::cpu_driver driver = shizuku::cpu_driver();
void shizuku_entry(void) {
  shizuku::cpu_driver::entry_func(sub_func, sub_context);
  while (true) {
    printf("main");
    driver.context_switch(main_context, sub_context);
    
  }
  return;
}

void sub_func() {
  while (true) {
    printf("sub");
    driver.context_switch(sub_context, main_context);
  }
}