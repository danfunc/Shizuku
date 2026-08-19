#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "shizuku/app_entry.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/selftest.hpp"
#include "stdio.h"

// ブート活性化 (スレッド 0) が最初に走らせるコード。将来ここがカーネルオブジェクトの
// main になる (Phase 3)。今はカーネル機構の自己テストを回してから生存表示に入る。
void shizuku::app_entry() {
  shizuku::selftest::call_ladder();
  while (true) {
    printf("alive\n");
    sleep_ms(1000);
  }
}

int main() {
  stdio_init_all();
  sleep_ms(1000); // ホストが CDC を開く前の出力を落とさないための待ち
  shizuku::kernel_instance.init();
  // 今の実行をスレッド 0 として採用し、スレッドスタックへ移って app_entry へ。
  shizuku::kernel_instance.bootstrap(shizuku::selftest::BOOT_COOKIE,
                                     shizuku::app_entry);
}
