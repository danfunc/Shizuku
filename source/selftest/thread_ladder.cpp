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
constexpr uintptr_t OBJECT_SLEEPER = 6; // 借りたまま眠ろうとする相手
constexpr uintptr_t METHOD_MAIN = 0;

volatile uint32_t g_worker_rounds = 0;
volatile uint32_t g_worker_done = 0;
volatile uint32_t g_hog_rounds = 0;
volatile uint32_t g_hog_stop = 0;
volatile uint32_t g_sleeper_woke = 0;
constexpr uintptr_t SLEEPER_SLEEP_US = 50000; // 貸す量よりずっと長く眠る

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

// 借りている最中に眠ろうとする相手。★ここが「クロックで数える」ことの穴の
//   受け皿になる: サイクルカウンタはコアが止まっている間は数えないので、借り手が
//   そのまま眠れると予算が減らず、取り上げも起きない = 貸し手へ CPU が戻らない。
//   Shizuku ではこれは機構の穴ではなく**契約の話**として閉じている — 眠るのは
//   カーネルオブジェクトへの**要請**であって、要請した時点で実行権は返る。
//   つまり「眠りに入る = 契約の終了」。それを下で実際に確かめる。
uintptr_t sleeper(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  api(object_api::SLEEP_US, SLEEPER_SLEEP_US);
  g_sleeper_woke = 1;
  return 0;
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

  // ---- 借りたまま眠ろうとしたら、契約はそこで終わる ------------------------
  // ★これを確かめないと「クロックで数える」設計に穴が開く: サイクルカウンタは
  //   コアが止まっている間は数えないので、借り手がそのまま眠れると予算は減らず、
  //   取り上げも起きない。Shizuku ではそれを機構ではなく契約で閉じている
  //   (眠るのはカーネルオブジェクトへの要請で、要請した時点で実行権が返る)。
  //   **契約は、破れていないことを見て初めて契約になる**。
  {
    api(object_api::CREATE_OBJECT, OBJECT_SLEEPER, (uintptr_t)&sleeper, 0);
    const api_result spawned =
        api(object_api::SPAWN, OBJECT_SLEEPER, METHOD_MAIN, 0);
    check("sleep: spawn sleeper", spawned.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned.error, 0);

    const uint32_t lend_cycles = 5000u * BOARD::cycles_per_us(); // ≒5ms 貸す
    const uint64_t started = BOARD::time_us();
    const api_result granted =
        api(object_api::RUN_FOR, spawned.value, lend_cycles);
    const uint64_t elapsed = BOARD::time_us() - started;

    // 1 = 相手が自分から返した。0 (使い切り) なら「眠っても予算が減らない」まま
    // 期限で取り上げたことになり、契約ではなく機構が救ったことになる。
    check("sleep: the borrower gave the grant back", granted.value == 1,
          (unsigned long)granted.value, 1);
    // ★貸した量よりずっと早く戻ること。眠り (50ms) を待たされていないことも兼ねる。
    check("sleep: the lender resumed at once", elapsed < 5000,
          (unsigned long)elapsed, 5000);

    // 眠り終えたら起きること (返したきり忘れられていないか)。
    for (uint32_t guard = 0; guard < 5000 && g_sleeper_woke == 0; ++guard)
      api(object_api::YIELD);
    check("sleep: the sleeper woke up later", g_sleeper_woke == 1,
          (unsigned long)g_sleeper_woke, 1);
  }

  // ---- 取り上げは「期限内」ではなく「期限のあと」に起きる -----------------
  // ★ここを測らないと、貸し借りの保証を言い間違える。予算は「どれだけ**仕事**を
  //   してよいか」なので、取り上げは仕事を使い切った**あと**に起きる。保証できるのは
  //   「期限内に戻る」ではなく「**超過に上界がある**」のほう。その上界を数字で出す。
  //   超過の内訳: 装填の下限による丸め + 割り込みの入り口 + 文脈の退避復元 +
  //   借り手がハンドラの中に居たときの見送り (GRANT_RETRY_CYCLES)。
  {
    g_hog_stop = 0;
    const api_result spawned =
        api(object_api::SPAWN, OBJECT_HOG, METHOD_MAIN, 0);
    check("overshoot: spawn hog", spawned.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned.error, 0);
    constexpr uint32_t ROUNDS = 200;
    constexpr uint32_t BUDGET_US = 200;
    const uint32_t budget_cycles = BUDGET_US * BOARD::cycles_per_us();
    uint64_t worst = 0;
    uint64_t total = 0;
    uint32_t expired = 0;
    for (uint32_t round = 0; round < ROUNDS; ++round) {
      const uint64_t started = BOARD::time_us();
      const api_result granted =
          api(object_api::RUN_FOR, spawned.value, budget_cycles);
      const uint64_t elapsed = BOARD::time_us() - started;
      if (granted.value == 0)
        ++expired;
      const uint64_t over = elapsed > BUDGET_US ? elapsed - BUDGET_US : 0;
      if (over > worst)
        worst = over;
      total += over;
    }
    BOARD::diag_printf(
        "[SELFTEST] overshoot over %lu grants of %luus: mean %luus, worst "
        "%luus (%lu/%lu reclaimed by budget)\n",
        (unsigned long)ROUNDS, (unsigned long)BUDGET_US,
        (unsigned long)(total / ROUNDS), (unsigned long)worst,
        (unsigned long)expired, (unsigned long)ROUNDS);
    // ★毎回きっちり取り上げていること。1 回でも相手が自分から返していたら、
    //   それは hog ではないので測定そのものが無意味になる。
    check("overshoot: every grant was reclaimed", expired == ROUNDS,
          (unsigned long)expired, (unsigned long)ROUNDS);
    // ★上界を主張する。青天井なら「戻ってくる」と言えない。
    check("overshoot: bounded", worst < BUDGET_US, (unsigned long)worst,
          (unsigned long)BUDGET_US);
    g_hog_stop = 1;
    api(object_api::RUN_FOR, spawned.value, budget_cycles);
  }

  // ---- 配れる持ち分が無いなら、貸さずに断る -------------------------------
  // ★装填には下限があるので、下限未満を「丸めて」貸すと借り手は貸された以上に
  //   走れる。少ししか残っていないときに黙って多めに貸すより、断るほうが正しい。
  {
    const api_result spawned =
        api(object_api::SPAWN, OBJECT_WORKER, METHOD_MAIN, 1);
    const api_result granted = api(object_api::RUN_FOR, spawned.value, 8);
    check("grant: a budget too small to honour is refused",
          granted.error == (uintptr_t)object_error::NO_TIME,
          (unsigned long)granted.error, (unsigned long)object_error::NO_TIME);
    // 断ったのだから相手は走っていない = まだ READY のまま片付けられる。
    api(object_api::RUN_FOR, spawned.value,
        2000u * BOARD::cycles_per_us());
  }

  BOARD::diag_printf("[SELFTEST] thread ladder done: %lu passed, %lu failed\n",
                     (unsigned long)passed, (unsigned long)failed);
}

} // namespace selftest
} // namespace shizuku
