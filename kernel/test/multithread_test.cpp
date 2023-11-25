#include "pico/stdlib.h"
#include "shizuku/cpu_manager.hpp"
#include "shizuku/kernel.hpp"
const uint LED_PIN = PICO_DEFAULT_LED_PIN;
shizuku::context sub_context, main_context;
shizuku::cpu_driver driver = shizuku::cpu_driver();
shizuku::types::object boot_object = shizuku::types::object();
shizuku::types::cpu_manager manager = shizuku::types::cpu_manager();
int sub_func(int argc, char **argv);
void multi_thread_entry() {
  shizuku::platform::std::shared_ptr<shizuku::context>
      main_context_pointer =

          shizuku::platform::std::shared_ptr<shizuku::context>(&main_context),
      sub_context_pointer =
          shizuku::platform::std::shared_ptr<shizuku::context>(&sub_context);
  shizuku::cpu_driver::entry_func(sub_func, &sub_context, 3, 0);
  manager.set_current_remain(1);
  manager.add_task(sub_context_pointer, 1, 0);
  while (true) {
    printf("main_thread\n");
    sleep_ms(500);
    manager.add_task(main_context_pointer, 1, 1);
    // manager.context_switch();
    shizuku::types::cpu_manager::context_switch(&main_context, &sub_context);
  }
  return;
}
int sub_func(int argc, char **argv) {
  auto sub_context_pointer =
      shizuku::platform::std::shared_ptr<shizuku::context>(&sub_context);
  while (true) {
    printf("sub_thread\n");
    printf("thread_argc:%d\n", argc);
    sleep_ms(500);
    manager.add_task(sub_context_pointer, 1, 1);
    // manager.context_switch();
    shizuku::types::cpu_manager::context_switch(&sub_context, &main_context);
  }
}