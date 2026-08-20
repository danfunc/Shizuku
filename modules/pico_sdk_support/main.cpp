#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "shizuku/app_entry.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/peripherals.hpp"
#include "shizuku/selftest.hpp"
#include "stdio.h"

// スレッド 0 が最初に走らせるコード = 系の組み立て。ここはまだどのオブジェクトの
// メソッドでもない (フレーム 0 段) ので、撃った svc はオブジェクトと同じ経路で
// カーネルオブジェクトのハンドラへ届く。
void shizuku::app_entry() {
  // ボードが提供するペリフェラルオブジェクト (特権を宣言する数少ないオブジェクト)。
  shizuku::objects::register_peripherals();
  shizuku::selftest::call_ladder();

  // 生存表示。**オブジェクトのメソッド呼び出し経由で** LED を叩くので、シリアルが
  // 見えなくても「オブジェクトシステムが回っている」ことが目視でわかる。
#ifdef PICO_DEFAULT_LED_PIN
  using namespace shizuku::objects;
  gpio_request request{PICO_DEFAULT_LED_PIN, 0};
  shizuku::KERNEL::ARCH::syscall((uintptr_t)shizuku::object_api::CALL_METHOD,
                                 GPIO_OBJECT, (uintptr_t)gpio_method::CONFIGURE,
                                 (uintptr_t)&request);
  while (true) {
    request.value ^= 1u;
    shizuku::KERNEL::ARCH::syscall((uintptr_t)shizuku::object_api::CALL_METHOD,
                                   GPIO_OBJECT, (uintptr_t)gpio_method::WRITE,
                                   (uintptr_t)&request);
    printf("alive (led=%lu)\n", (unsigned long)request.value);
    sleep_ms(500);
  }
#else
  while (true) {
    printf("alive\n");
    sleep_ms(1000);
  }
#endif
}

int main() {
  stdio_init_all();
  sleep_ms(1000); // ホストが CDC を開く前の出力を落とさないための待ち
  shizuku::kernel_instance.init();
  // 系の組み立て: カーネルオブジェクトの表を用意し、そのハンドラをカーネルへ据える。
  // これ以降、オブジェクトが撃った svc はすべてそのハンドラへ届く。
  shizuku::kernel_object_instance.init();
  shizuku::kernel_instance.set_object_handler(
      shizuku::KERNEL_OBJECT::handler_entry());
  // 今の実行をスレッド 0 として採用し、スレッドスタックへ移って app_entry へ。
  shizuku::kernel_instance.bootstrap(shizuku::app_entry);
}
