#include <pico/stdlib.h>
#include <shizuku/kernel.hpp>
#include <stdio.h>
void test_object_main(int argc, char **argv);
void object_system_test_main() {
  auto main_thread = shizuku::kernel.get_current_thread().lock();
  shizuku::kernel.create_object("test_object", test_object_main, 1, nullptr);
  shizuku::kernel.add_task(main_thread, 1, 0);
  shizuku::kernel.context_switch();
  shizuku::kernel.call_func("test_object", "test_arm", 0, nullptr);
  shizuku::kernel.context_switch();
  while (1) {
    printf("returned\n");
    sleep_ms(1000);
  }
}

int test_arm_func(int argc, char **argv);

void test_object_main(int argc, char **argv) {
  shizuku::kernel.add_func("test_arm", test_arm_func);
  shizuku::kernel.abort_current_task();
  while (true) {
    shizuku::kernel.context_switch();
  }
}
int test_arm_func(int argc, char **argv) {
  while (true) {
    printf("success!!!\n");
    sleep_ms(1000);
  }
}
