
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
                                  &sub_context, 0, 0);
  while (true) {
    printf("main\n");
    gpio_put(LED_PIN, 1); // LED点灯
    printf("core_num=%d\n", driver.get_core_num());
    sleep_ms(500);
    if (shizuku::cpu_driver::save_context(&main_context) == 0)
      shizuku::cpu_driver::load_context(&sub_context);
  }
  return;
}

void sub_func() {
  while (true) {
    printf("sub\n");
    gpio_put(LED_PIN, 0);
    sleep_ms(500);
    if (shizuku::cpu_driver::save_context(&sub_context) == 0)
      shizuku::cpu_driver::load_context(&main_context); // LED消灯
  }
}

namespace shizuku {
types::object boot_object;
}
