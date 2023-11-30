#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "shizuku_entry.h"
#include "stdio.h"

int main(void) {
  stdio_init_all();
  sleep_ms(100);
  multicore_launch_core1(shizuku_entry);
  while (1) {
  }
  return 0;
}
