// ===========================================================================
//  負荷試験 — 揺らぎ (jitter) が出ないことを数字で見る
// ===========================================================================
//  ランダムな長さの仕事をするスレッドを何本か走らせた状態で、周期スレッドが
//  締切をどれだけ外すかを測る。**点滅が目で揺れて見えるかどうかでは判定しない** —
//  数 ms のズレは目には見えないが、実時間の仕事では致命的になり得るため。
//
//  ★負荷スレッドは 3 種類にする。壊れ方が違うものを混ぜないと機構の別々の面を
//    突けない:
//    (a) 自分からは絶対に返さない  → 期限による取り上げが効かないと系が凍る
//    (b) たまに自分から返す        → 協調側の経路
//    (c) 眠っては短く暴れる        → 起床と round-robin の絡み
//  ★負荷スレッドは印字しない。印字は内部でロックを取るので、期限で任意の点から
//    取り上げられる相手にやらせると、ロックを持ったまま止まって系が固まる
//    (参照実装が「借り手は printf を避けよ」と書いている理由そのもの)。
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/peripherals.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_BLINK = 6;
constexpr uintptr_t OBJECT_LOAD = 7;
constexpr uintptr_t METHOD_MAIN = 0;

// 点滅の周期。人が見て分かる速さで、かつ測定の分解能も確保できるところ。
constexpr uint64_t BLINK_PERIOD_US = 250000;
// 何周期ごとに報告するか (報告そのものが負荷にならない程度に間引く)。
constexpr uint32_t REPORT_PERIODS = 8;

uintptr_t syscall_value(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
                        uintptr_t a3 = 0) {
  return ARCH::syscall((uintptr_t)number, a1, a2, a3).value;
}

// 軽い擬似乱数 (xorshift)。負荷の長さをばらけさせるためだけのもの。
uint32_t next_random(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

void burn_microseconds(uint32_t microseconds) {
  const uint64_t until = BOARD::time_us() + microseconds;
  while ((int64_t)(until - BOARD::time_us()) > 0) {
  }
}

volatile uint32_t g_load_rounds[3];
uintptr_t g_load_threads[3];

// ★期限を振って揺らぎが追随するかを見る。追随するなら原因は方針 (期限) であり、
//   ある値で頭打ちになるなら、そこから下は機構の費用 (取り上げ・切替・走査) か
//   周辺デバイスの費用。1 回の書き込みで曲線が取れるよう、窓ごとに切り替える。
// ★掃引は**クロック数**で書く (貸し借りの単位はクロック)。数字を読みやすくする
//   ため µs で書いてクロックへ直す — 換算するのは方針側であって、カーネルではない。
constexpr uint32_t BUDGET_SWEEP_US[] = {2000, 1000, 500, 200, 100};
constexpr uint32_t BUDGET_STEPS =
    sizeof(BUDGET_SWEEP_US) / sizeof(BUDGET_SWEEP_US[0]);

// 負荷スレッド。a0 = 種別 (0/1/2)。**印字しない**。
uintptr_t load(uintptr_t kind, uintptr_t, uintptr_t, uintptr_t) {
  uint32_t random_state = 0x12345678u + (uint32_t)kind * 2654435761u;
  while (true) {
    const uint32_t roll = next_random(random_state);
    g_load_rounds[kind % 3] = g_load_rounds[kind % 3] + 1;
    switch (kind) {
    case 0:
      // 自分からは返さない。期限で取り上げられる以外に止まる理由が無い。
      burn_microseconds(200 + (roll % 3000));
      break;
    case 1:
      burn_microseconds(50 + (roll % 800));
      syscall_value(object_api::YIELD); // たまに自分から返す
      break;
    default:
      burn_microseconds(20 + (roll % 400));
      syscall_value(object_api::SLEEP_US, 1000 + (roll % 5000));
      break;
    }
  }
}

// 周期スレッド。締切のズレを測り、LED をオブジェクト経由で叩く。
uintptr_t blink(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  objects::led_request request{0};
  uint64_t next = BOARD::time_us() + BLINK_PERIOD_US;
  uint64_t late_max = 0;      // 起動来の最大 (一過性の山も残る)
  uint64_t late_window = 0;   // 窓ごとの最大 (今も続いているかが分かる)
  // ★LED 書き込みは**最大と最小の両方**を採る。最大だけ見ていたときは budget に
  //   きれいに追従したので「SPI が重い」と読み違えかけたが、それは呼び出しの
  //   途中でプリエンプトされた時間が混ざっていただけだった。最小 = 誰にも
  //   邪魔されなかった 1 回 ≒ 本当にその操作にかかる時間。
  uint64_t led_window = 0;      // 窓の最大 (プリエンプト込み)
  uint64_t led_min = ~(uint64_t)0; // 窓の最小 (邪魔が入らなかった 1 回)
  uint32_t periods = 0;
  uint32_t sweep = 0;

  while (true) {
    // 絶対グリッドで待つ。相対で待つとズレが積み上がって「遅れ」が測れない。
    const int64_t remaining = (int64_t)(next - BOARD::time_us());
    if (remaining > 0)
      syscall_value(object_api::SLEEP_US, (uintptr_t)remaining);

    const uint64_t now = BOARD::time_us();
    const uint64_t late = now > next ? now - next : 0;
    if (late > late_max)
      late_max = late;
    if (late > late_window)
      late_window = late;

    request.value ^= 1u;
    // ★LED への書き込み時間を別に測る。無線チップ側の LED は SPI 越しなので、
    //   ここが大きいなら「揺らぎ」の一部は周辺デバイスの費用であって
    //   スケジューリングの費用ではない — 混ぜると原因を取り違える。
    const uint64_t led_started = BOARD::time_us();
    const auto written = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                       objects::LED_OBJECT,
                                       (uintptr_t)objects::led_method::WRITE,
                                       (uintptr_t)&request);
    const uint64_t led_took = BOARD::time_us() - led_started;
    if (led_took > led_window)
      led_window = led_took;
    if (led_took < led_min)
      led_min = led_took;
    next += BLINK_PERIOD_US;

    if (++periods >= REPORT_PERIODS) {
      // ★「動いている」ではなく「どれだけ外したか」を出す。窓の値と起動来の値を
      //   分けるのは、回復したのに古い山が残って誤判定するのを避けるため
      //   (参照実装が late(max) だけ見て誤って FAIL と判定した罠)。
      const uint32_t budget_us = BUDGET_SWEEP_US[sweep];
      BOARD::diag_printf(
          "[STRESS] budget=%lucyc(%luus) late win=%luus max=%luus "
          "led_write=%lu/%luus "
          "load=%lu/%lu/%lu err=%lu | selftest=%lu passed/%lu failed "
          "control unpriv=%lu priv=%lu | stopped_threads=%lu last_pc=%08lx\n",
          (unsigned long)(budget_us * BOARD::cycles_per_us()),
          (unsigned long)budget_us, (unsigned long)late_window,
          (unsigned long)late_max, (unsigned long)led_min,
          (unsigned long)led_window,
          (unsigned long)g_load_rounds[0], (unsigned long)g_load_rounds[1],
          (unsigned long)g_load_rounds[2], (unsigned long)written.error,
          (unsigned long)passed, (unsigned long)failed,
          (unsigned long)unprivileged_control,
          (unsigned long)privileged_control,
          (unsigned long)kernel_instance.faults().count,
          (unsigned long)kernel_instance.faults().pc);
      late_window = 0;
      led_window = 0;
      led_min = ~(uint64_t)0;
      periods = 0;
      // 次の窓は別の期限で測る (同じ条件を続けて測るのではなく、条件を変えて
      // 追随するかを見る。追随すれば原因は期限だと言い切れる)。
      sweep = (sweep + 1) % BUDGET_STEPS;
      for (uintptr_t index = 0; index < 3; ++index)
        syscall_value(object_api::SET_BUDGET, g_load_threads[index],
                      BUDGET_SWEEP_US[sweep] * BOARD::cycles_per_us());
    }
  }
}

} // namespace

void stress_launch() {
  syscall_value(object_api::CREATE_OBJECT, OBJECT_BLINK, (uintptr_t)&blink, 0);
  syscall_value(object_api::CREATE_OBJECT, OBJECT_LOAD, (uintptr_t)&load, 0);

  // ★測る前に一度叩いておく。無線チップ側の LED は最初の 1 回でチップの立ち上げ
  //   (ファーム転送) が走り、700ms 近くかかる。これを測定に混ぜると「揺らぎ」ではなく
  //   「一度きりの立ち上げ費用」を見てしまう — 定常の性質を測りたいので分けておく。
  {
    objects::led_request warm{0};
    const uint64_t started = BOARD::time_us();
    ARCH::syscall((uintptr_t)object_api::CALL_METHOD, objects::LED_OBJECT,
                  (uintptr_t)objects::led_method::WRITE, (uintptr_t)&warm);
    BOARD::diag_printf("[STRESS] led first write took %luus (一度きりの立ち上げ)\n",
                       (unsigned long)(BOARD::time_us() - started));
  }

  const uintptr_t blink_thread =
      syscall_value(object_api::SPAWN, OBJECT_BLINK, METHOD_MAIN, 0);
  // ★点滅スレッドの時限は短くする。長い時限は「取り上げられるまでの最悪待ち時間」
  //   そのものなので、周期の精度を決めるのはここ。
  syscall_value(object_api::SET_BUDGET, blink_thread,
                500u * BOARD::cycles_per_us());

  for (uintptr_t kind = 0; kind < 3; ++kind)
    g_load_threads[kind] =
        syscall_value(object_api::SPAWN, OBJECT_LOAD, METHOD_MAIN, kind);

  BOARD::diag_printf("[STRESS] blink thread=%lu, 3 load threads running\n",
                     (unsigned long)blink_thread);
}

} // namespace selftest
} // namespace shizuku
