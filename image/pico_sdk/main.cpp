#include "pico/stdlib.h"
#include "shizuku_entry.h"
#include "stdio.h"

int main(void) {
  stdio_init_all();
  sleep_ms(100);
  shizuku_entry();
  while (1) {
    sleep_ms(100);
    printf("hello");
    sleep_ms(100);
  }

  return 0;
}
