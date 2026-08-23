#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "shizuku/app_entry.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/flash_fs.hpp"
#include "shizuku/objects/gdb_stub.hpp"
#include "shizuku/objects/usb_cdc.hpp"
#include "shizuku/objects/peripherals.hpp"
#include "shizuku/apps/thermal.hpp"
#include "shizuku/selftest.hpp"
#include "stdio.h"

// スレッド 0 が最初に走らせるコード = 系の組み立て。ここはまだどのオブジェクトの
// メソッドでもない (フレーム 0 段) ので、撃った svc はオブジェクトと同じ経路で
// カーネルオブジェクトのハンドラへ届く。
void shizuku::app_entry() {
  // ★スレッドが落ちたときに実行権を渡す先を教えておく。誰に渡すかは方針なので
  //   カーネルは選ばない。ここではブートスレッド (アイドル役) を指定する。
  shizuku::kernel_instance.set_recovery_thread(0);

  // ボードが提供するペリフェラルオブジェクト (特権を宣言する数少ないオブジェクト)。
  const uint32_t peripheral_failures = shizuku::objects::register_peripherals();
  // 媒体を持つオブジェクト。読むのは XIP のアドレスを配るだけなので安いが、
  // 書くと XIP ごと止まるので、扱いはペリフェラルと同じく特権側。
  shizuku::objects::register_flash_fs();
  shizuku::selftest::call_ladder();
  shizuku::selftest::thread_ladder();
  shizuku::selftest::memory_ladder();
  shizuku::selftest::unprivileged_probe();

  // ★2 本目のコアを起こす。ここまでの自己テストが 1 コアで通っていることを
  //   確かめてから起こす — 先に起こすと、失敗したときに「並行のせいか元からか」を
  //   切り分けられない (梯子式の作法。DESIGN §16)。
  if (shizuku::kernel_object_instance.start_secondary_core())
    shizuku::KERNEL::BOARD::diag_printf("[BOOT] secondary core launched\n");
  shizuku::selftest::multicore_probe();
  shizuku::selftest::stream_ladder();
  // ★flash の書き込みは 2 コア目を起こした**後**に試す。消去中は XIP が止まるので、
  //   相手が止められていなければそのコアは flash 上のコードを踏んで即死する。
  //   つまりここで書けること自体が「止められている」ことの証拠になる。
  shizuku::objects::flash_fs_probe();
  shizuku::selftest::flash_stream_ladder();
  shizuku::selftest::debug_ladder();

  // 温度の履歴アプリ。★負荷試験より前に起こして、負荷の下で周期がどれだけ
  //   揺らぐかを見る (静かな系で測っても揺らぎの話にならない)。
  shizuku::apps::start_thermal();

  // ★GDB stub。繋がれるまでは何もしない (繋がれた瞬間に診断出力を GDB へ譲る)。
  shizuku::objects::start_gdb_stub();

  // 負荷試験を起動する。以後、点滅と報告は専用スレッドが行う。
  shizuku::selftest::stress_launch();

  // ★スレッド 0 は以後アイドル役に徹する。誰かが走れるなら渡し、誰も居なければ
  //   空回りするだけ。**ここで自分が仕事をしてはいけない** — アイドルが仕事を
  //   持つと、その仕事が他の全部の遅れになる。
  while (true)
    shizuku::KERNEL::ARCH::syscall((uintptr_t)shizuku::object_api::YIELD);
}

int main() {
  // ★USB は自前で持つ (CDC 2 本: 診断と GDB)。pico_stdio_usb は 1 本前提で、
  //   記述子も差し替えられないため (D42)。
  shizuku::objects::usb_cdc_init();
  sleep_ms(1000); // ホストが CDC を開く前の出力を落とさないための待ち
  shizuku::kernel_instance.init();
  // 系の組み立て: カーネルオブジェクトの表を用意し、そのハンドラをカーネルへ据える。
  // これ以降、オブジェクトが撃った svc はすべてそのハンドラへ届く。
  shizuku::kernel_object_instance.init();
  shizuku::kernel_instance.set_object_handler(
      shizuku::KERNEL_OBJECT::handler_entry());
  // 今の実行をスレッド 0 として採用し、スレッドスタックへ移って app_entry へ。
  // ★最初の 1 本のスタックもオブジェクトランドから借りる (他のスレッドと同じ扱い)。
  const auto boot = shizuku::kernel_object_instance.lend_boot_stack();
  shizuku::kernel_instance.bootstrap(shizuku::app_entry, boot.base, boot.bytes);
}
