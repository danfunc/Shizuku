#include "pico/stdlib.h"
#include "shizuku/config.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/object.hpp"
#include "shizuku/processors/rp2040.hpp"
#include "shizuku_entry.h"
#include "stdio.h"
void sub_func();
void mmu_test_entry();
const uint LED_PIN = PICO_DEFAULT_LED_PIN;
shizuku::context sub_context, main_context;
shizuku::cpu_driver driver = shizuku::cpu_driver();
shizuku::types::object boot_object = shizuku::types::object();
void shizuku_entry(void) {
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
  shizuku::cpu_driver::entry_func((int (*)(int, char *[]))sub_func,
                                  sub_context, 0, 0);
  driver.change_current_object(boot_object);
  shizuku::kernel.context_switch();
  driver.add_task(sub_context, 1);
  driver.change_current_context(main_context);
  while (true) {
    driver.add_task(main_context, boot_object, 1);
    printf("main\n");
    driver.context_switch();
    gpio_put(LED_PIN, 1); // LED点灯
    printf("core_num=%d\n", driver.get_core_num());
    sleep_ms(500);
  }
  return;
}

void sub_func() {
  while (true) {
    driver.add_task(sub_context, boot_object, 1);
    printf("sub\n");
    gpio_put(LED_PIN, 0); // LED消灯
    sleep_ms(500);
    driver.context_switch();
  }
}

namespace shizuku {
types::object boot_object;
}