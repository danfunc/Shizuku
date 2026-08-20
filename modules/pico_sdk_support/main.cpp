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
  shizuku::selftest::unprivileged_probe();

  // 負荷試験を起動する。以後、点滅と報告は専用スレッドが行う。
  shizuku::selftest::stress_launch();

  // ★スレッド 0 は以後アイドル役に徹する。誰かが走れるなら渡し、誰も居なければ
  //   空回りするだけ。**ここで自分が仕事をしてはいけない** — アイドルが仕事を
  //   持つと、その仕事が他の全部の遅れになる。
  while (true)
    shizuku::KERNEL::ARCH::syscall((uintptr_t)shizuku::object_api::YIELD);
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
