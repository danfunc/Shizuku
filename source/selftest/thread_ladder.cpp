// ===========================================================================
//  スレッドと実行権の自己テスト梯子 (DESIGN §10 / §16)
// ===========================================================================
//  1 本起こす → 譲り合う → 時限つきで貸す → **返さない相手を取り上げる**、の順に
//  上る。最後のが本命で、「1 つの暴走が全系を凍らせない」(§10.2) の実証になる。
//
//  ★カウンタは期待値と突き合わせる。前進しているかだけを見ない — 生存カウンタは
//    機能の証拠にならない (DESIGN §16 の CYW43 の実例)。
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_WORKER = 4; // 譲り合う相手
constexpr uintptr_t OBJECT_HOG = 5;    // 実行権を返さない相手
constexpr uintptr_t METHOD_MAIN = 0;

volatile uint32_t g_worker_rounds = 0;
volatile uint32_t g_worker_done = 0;
volatile uint32_t g_hog_rounds = 0;
volatile uint32_t g_hog_stop = 0;

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

// 譲り合う相手。決められた回数だけ実行権を返しては戻ってくる。
uintptr_t worker(uintptr_t rounds, uintptr_t, uintptr_t, uintptr_t) {
  for (uintptr_t index = 0; index < rounds; ++index) {
    g_worker_rounds = (uint32_t)(index + 1);
    api(object_api::YIELD); // 誰でもいいから次の人へ
  }
  g_worker_done = 1;
  return rounds; // return するとスレッドが終わる (戻り先が無い = exit)
}

// 実行権を返さない相手。**自分からは絶対に譲らない**。取り上げが効かなければ
// ここで系が止まる = この梯子が失敗したことがそのまま分かる。
uintptr_t hog(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  while (g_hog_stop == 0)
    g_hog_rounds = g_hog_rounds + 1;
  return 0;
}

} // namespace

void thread_ladder() {
  BOARD::diag_printf("[SELFTEST] thread ladder start\n");

  api(object_api::CREATE_OBJECT, OBJECT_WORKER, (uintptr_t)&worker, 0);
  api(object_api::CREATE_OBJECT, OBJECT_HOG, (uintptr_t)&hog, 0);

  // 1 本起こして、譲り合いで進むこと。
  {
    const api_result spawned =
        api(object_api::SPAWN, OBJECT_WORKER, METHOD_MAIN, 3);
    check("spawn: error", spawned.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned.error, 0);
    check("spawn: thread id", spawned.value != 0,
          (unsigned long)spawned.value, 1);

    // 相手が 3 回まわって終わるまで、こちらも譲り続ける。
    for (uint32_t guard = 0; guard < 100 && g_worker_done == 0; ++guard)
      api(object_api::YIELD);
    check("yield: worker finished", g_worker_done == 1,
          (unsigned long)g_worker_done, 1);
    check("yield: worker rounds", g_worker_rounds == 3,
          (unsigned long)g_worker_rounds, 3);
  }

  // 時限つきで貸す。相手は返さないので**期限で取り上げられる**はず。
  {
    const api_result spawned =
        api(object_api::SPAWN, OBJECT_HOG, METHOD_MAIN, 0);
    check("grant: spawn hog", spawned.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned.error, 0);

    // ★貸す量は**クロック数**。壁時計で測るのはこちら (試験する側) の都合なので、
    //   期待される経過時間はここで換算する。カーネルは換算を持たない。
    const uint32_t lend_cycles = 2000u * BOARD::cycles_per_us(); // ≒2ms 相当
    const uint64_t expected_us = lend_cycles / BOARD::cycles_per_us();
    const uint32_t before = g_hog_rounds;
    const uint64_t started = BOARD::time_us();
    const api_result granted = api(object_api::RUN_FOR, spawned.value,
                                   lend_cycles);
    const uint64_t elapsed = BOARD::time_us() - started;

    check("grant: returned", granted.error == (uintptr_t)object_error::OK,
          (unsigned long)granted.error, 0);
    // 0 = 使い切って取り上げ。相手は自分から返さないのでこれが期待値。
    check("grant: reclaimed by budget", granted.value == 0,
          (unsigned long)granted.value, 0);
    // ★取り上げまでの時間が貸した量の桁に収まっていること。青天井なら取り上げは
    //   効いていない (「動いた」だけでは証拠にならない)。
    //   ★下限も見る: クロックで数えているのだから、2ms ぶん貸したのに 0.5ms で
    //     戻ってきたら「数え方が壊れている」証拠になる (上限だけ見ていると
    //     早すぎる取り上げを見逃す)。
    check("grant: within the lent budget",
          elapsed >= expected_us / 2 && elapsed < expected_us * 10,
          (unsigned long)elapsed, (unsigned long)expected_us);
    // 相手が実際に走ったこと (貸したのに走っていないなら別の壊れ方)。
    check("grant: borrower ran", g_hog_rounds > before,
          (unsigned long)(g_hog_rounds - before), 1);

    g_hog_stop = 1; // 片付け: 次に走ったら自分で終わる
    api(object_api::RUN_FOR, spawned.value, lend_cycles);
  }

  BOARD::diag_printf("[SELFTEST] thread ladder done: %lu passed, %lu failed\n",
                     (unsigned long)passed, (unsigned long)failed);
}

} // namespace selftest
} // namespace shizuku
