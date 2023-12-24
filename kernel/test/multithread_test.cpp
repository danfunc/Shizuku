#include "pico/stdlib.h"
#include "shizuku/cpu_manager.hpp"
#include "shizuku/kernel.hpp"
const uint LED_PIN = PICO_DEFAULT_LED_PIN;
shizuku::context sub_context, main_context;
shizuku::cpu_driver driver = shizuku::cpu_driver();
shizuku::types::cpu_manager manager = shizuku::types::cpu_manager();
int sub_func(int argc, char **argv);
void multi_thread_entry() {
  shizuku::platform::std::shared_ptr<shizuku::types::thread>
      sub_thread = std::make_shared<shizuku::types::thread>(),
      sub2_thread = std::make_shared<shizuku::types::thread>(),
      sub3_thread = std::make_shared<shizuku::types::thread>(),
      sub4_thread = std::make_shared<shizuku::types::thread>();
  shizuku::cpu_driver::entry_func(sub_func, sub_thread->context.get(), 1, 0);
  shizuku::cpu_driver::entry_func(sub_func, sub2_thread->context.get(), 2, 0);
  shizuku::cpu_driver::entry_func(sub_func, sub3_thread->context.get(), 3, 0);
  shizuku::cpu_driver::entry_func(sub_func, sub4_thread->context.get(), 4, 0);
  shizuku::kernel.add_task(sub_thread, 1, 6);
  shizuku::kernel.add_task(sub2_thread, 1, 5);
  shizuku::kernel.add_task(sub3_thread, 1, 4);
  shizuku::kernel.add_task(sub4_thread, 1, 3);
  shizuku::kernel.abort_current_task();
  /*while (true) {
    printf("main_thread\n");
    sleep_ms(500);
    manager.add_task(main_thread, 1, 1);
    manager.context_switch();
  }*/
  return;
}
int sub_func(int argc, char **argv) {
  auto sub_thread = shizuku::kernel.get_current_thread().lock();
  int count = 0;
  while (true) {
    printf("sub_thread\n");
    printf("thread_argc:%d\n", argc);
    sleep_ms(500);
    if (++count % 3)
      shizuku::kernel.add_task(sub_thread, 3, 1);
    shizuku::kernel.context_switch();
  }
}