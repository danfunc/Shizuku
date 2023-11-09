#include "pico/stdlib.h"
#include "shizuku/config.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/object.hpp"
#include "shizuku/processors/rp2040.hpp"
#include "shizuku_entry.h"
#include "stdio.h"
void sub_func();
shizuku::context sub_context, main_context;
shizuku::cpu_driver driver = shizuku::cpu_driver();

struct context_list {
  context_list *next;
  shizuku::context *context;
};

context_list main_node, sub_node, *current_node;

void shizuku_entry(void) {
  driver.current_task.context = &main_context;
  driver.current_task.remain_time = 1;
  shizuku::cpu_driver::entry_func(sub_func, sub_context);
  driver.task_queue.push(
      shizuku::cpu_driver::task{.context = &sub_context, .remain_time = 1});
  while (true) {
    driver.task_queue.push(
        shizuku::cpu_driver::task{.context = &main_context, .remain_time = 1});
    printf("main");
    sleep_ms(100);
    driver.context_switch();
  }
  return;
}

void sub_func() {
  while (true) {
    driver.task_queue.push(
        shizuku::cpu_driver::task{.context = &sub_context, .remain_time = 1});
    printf("sub");
    driver.context_switch();
  }
}