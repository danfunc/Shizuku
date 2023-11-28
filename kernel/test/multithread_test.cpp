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
  shizuku::platform::std::shared_ptr<shizuku::types::thread> main_thread(
      new shizuku::types::thread(shizuku::types::thread{
          .context = shizuku::platform::std::make_shared<shizuku::context>(),
          .parent_object =
              shizuku::platform::std::make_shared<shizuku::types::object>(),
          .thread_id = 0})),
      sub_thread(new shizuku::types::thread(shizuku::types::thread{
          .context = shizuku::platform::std::make_shared<shizuku::context>(),
          .parent_object =
              shizuku::platform::std::make_shared<shizuku::types::object>(),
          .thread_id = 1}));
  shizuku::cpu_driver::entry_func(sub_func, sub_thread->context.get(), 3, 0);
  manager.set_current_remain(3);
  manager.add_task(sub_thread, 1, 0);
  while (true) {
    printf("main_thread\n");
    sleep_ms(500);
    manager.add_task(main_thread, 1, 1);
    // manager.context_switch();
    // manager.abort_current_task();
    shizuku::types::cpu_manager::context_switch(&main_context, &sub_context);
  }
  return;
}
int sub_func(int argc, char **argv) {
  auto sub_thread = manager.get_current_thread();
  while (true) {
    printf("sub_thread\n");
    printf("thread_argc:%d\n", argc);
    sleep_ms(500);
    manager.add_task(sub_thread, 1, 1);
    manager.context_switch();
  }
}