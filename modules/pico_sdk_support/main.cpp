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
  const uint32_t peripheral_failures = shizuku::objects::register_peripherals();
  shizuku::selftest::call_ladder();
  shizuku::selftest::thread_ladder();

  // 生存表示。**オブジェクトのメソッド呼び出し経由で** LED を叩く。
  // ★ここは LED がどのピンにあるか (そもそも GPIO かどうか) を知らない。
  //   pico2 は GPIO 25、pico2_w は無線チップ側と実体が違うが、その差は LED
  //   オブジェクトの中に閉じている。
  // ★呼び出しの結果は必ず見る。捨てると「呼んでいるのに何も起きない」になり、
  //   オブジェクト側かハードかを切り分けられなくなる (D12)。
  using namespace shizuku::objects;
  led_request request{0};

  auto call = [&](led_method method) {
    return shizuku::KERNEL::ARCH::syscall(
        (uintptr_t)shizuku::object_api::CALL_METHOD, LED_OBJECT,
        (uintptr_t)method, (uintptr_t)&request);
  };

  request.value = 1;
  const auto first = call(led_method::WRITE);
  const auto read_back = call(led_method::READ);
  // 書いた値を対象自身に読み戻させて突き合わせる (DESIGN §16)。
  printf("[LED] write(err=%lu) read=%lu(err=%lu) peripheral_failures=%lu\n",
         (unsigned long)first.error, (unsigned long)read_back.value,
         (unsigned long)read_back.error, (unsigned long)peripheral_failures);

  while (true) {
    request.value ^= 1u;
    const auto written = call(led_method::WRITE);
    // ★自己テストの結果を生存表示に載せる。起動時の出力はホストが繋ぐ前に流れて
    //   消えることがあるので、いつ繋いでも「あのとき全部通ったのか」が分かる。
    printf("alive led=%lu err=%lu selftest=%lu passed/%lu failed\n",
           (unsigned long)request.value, (unsigned long)written.error,
           (unsigned long)shizuku::selftest::passed,
           (unsigned long)shizuku::selftest::failed);
    sleep_ms(500);
  }
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
