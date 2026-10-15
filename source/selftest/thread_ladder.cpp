// ===========================================================================
//  スレッドと実行権の自己テスト梯子 (DESIGN §10 / §16)
// ===========================================================================
//  1 本起こす → 譲り合う → 時限つきで貸す → **返さない相手を取り上げる**、の順に
//  上る。最後のが本命で、「1 つの暴走が全系を凍らせない」(§10.2) の実証になる。
//
//  ★カウンタは期待値と突き合わせる。前進しているかだけを見ない — 生存カウンタは
//    機能の証拠にならない (DESIGN §16 の CYW43 の実例)。
#include "shizuku/kernel.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

inline constexpr const uintptr_t &OBJECT_WORKER = object_id::worker; // 譲り合う相手
inline constexpr const uintptr_t &OBJECT_HOG = object_id::hog; // 実行権を返さない相手
inline constexpr const uintptr_t &OBJECT_SLEEPER = object_id::sleeper;
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
    record_fail(name, got, want);
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

// ---- 追加 selftest (a)-(d) 用ヘルパ -----------------------------------------
// (a) 無宛先YIELDをborrowerから実行するとlenderへ戻る
volatile uint32_t g_yield_borrower_ran = 0;
uintptr_t yield_borrower(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  g_yield_borrower_ran = 1;
  api(object_api::YIELD);
  g_yield_borrower_ran = 2;
  return 0;
}

// (b) A→B→C入れ子とC予算がB残量でclamp
volatile uint32_t g_nested_c_ran = 0;
volatile uint32_t g_nested_b_saw_c_yield = 0;

uintptr_t nested_c_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  g_nested_c_ran = 1;
  api(object_api::YIELD); // C yields back to B
  return 0;
}

uintptr_t nested_b_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  api(object_api::CREATE_OBJECT, OBJECT_WORKER, (uintptr_t)&nested_c_worker, OBJECT_REPLACE);
  const api_result spawned_c = api(object_api::SPAWN, OBJECT_WORKER, METHOD_MAIN, 0);
  if (spawned_c.error == (uintptr_t)object_error::OK) {
    const api_result granted_c = api(object_api::RUN_FOR, spawned_c.value, 2000u * BOARD::cycles_per_us());
    if (granted_c.error == (uintptr_t)object_error::OK && granted_c.value == 1) {
      g_nested_b_saw_c_yield = 1;
    }
  }
  api(object_api::YIELD);
  return 0;
}

// (b-1) 仕様ケース (i): B に余裕を残して C へ一部貸与し、C reclaim 後に B が復帰・記録する
volatile uint32_t g_nested_b_saw_c_expired = 0;
volatile uint32_t g_nested_b_saw_c_elapsed_us = 0;

uintptr_t nested_b_margin_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const api_result spawned_c = api(object_api::SPAWN, OBJECT_HOG, METHOD_MAIN, 0);
  if (spawned_c.error == (uintptr_t)object_error::OK) {
    const uint32_t lend_c_cycles = 2000u * BOARD::cycles_per_us(); // 2ms 貸出
    const uint64_t started_c = BOARD::time_us();
    const api_result granted_c = api(object_api::RUN_FOR, spawned_c.value, lend_c_cycles);
    const uint64_t elapsed_c = BOARD::time_us() - started_c;
    g_nested_b_saw_c_expired = (granted_c.value == 0) ? 1 : 0;
    g_nested_b_saw_c_elapsed_us = (uint32_t)elapsed_c;
    g_hog_stop = 1;
  }
  api(object_api::YIELD);
  return 0;
}

// (b-2) 仕様ケース (ii): B 残量全部を C へ又貸し (50ms 要求が外側 2ms でクランプ)。
// C 取り上げと同時に B も残量 0 で巻き取られ、B の事後印は残らず、A の戻り値が EXPIRED (0)。
volatile uint32_t g_nested_b_clamp_exhaust_ran = 0;

uintptr_t nested_b_clamp_exhaust_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const api_result spawned_c = api(object_api::SPAWN, OBJECT_HOG, METHOD_MAIN, 0);
  if (spawned_c.error == (uintptr_t)object_error::OK) {
    const uint32_t huge_budget = 50000u * BOARD::cycles_per_us(); // 50ms 要求
    api(object_api::RUN_FOR, spawned_c.value, huge_budget);
    // B の残量は C と共に 0 になるため、仕様上ここは実行されてはならない
    g_nested_b_clamp_exhaust_ran = 1;
  }
  return 0;
}

// (c) A→B→A GRANTでdepth増加なしにAへ戻る
volatile uint32_t g_ancestor_b_ran = 0;
volatile uint32_t g_ancestor_b_resumed = 0;
uintptr_t ancestor_worker(uintptr_t tid_lender, uintptr_t, uintptr_t, uintptr_t) {
  g_ancestor_b_ran = 1;
  const uint32_t lend_cycles = 10000u * BOARD::cycles_per_us();
  const api_result granted =
      api(object_api::RUN_FOR, tid_lender, lend_cycles);
  if (granted.error == (uintptr_t)object_error::OK && granted.value == 1) {
    g_ancestor_b_resumed = 1;
  }
  return 0;
}

// (c-2) 多段 A→B→C→A 祖先返却と保存済み呼出しからの再開 (resume)
volatile uint32_t g_multi_ancestor_c_ran = 0;
volatile uint32_t g_multi_ancestor_b_saw_c_return = 0;
volatile uint32_t g_multi_ancestor_c_resumed = 0;
volatile uint32_t g_multi_ancestor_b_completed = 0;

uintptr_t multi_ancestor_c_worker(uintptr_t tid_a, uintptr_t, uintptr_t, uintptr_t) {
  g_multi_ancestor_c_ran = 1;
  const uint32_t lend_cycles = 10000u * BOARD::cycles_per_us();
  // C が祖先 A へ GRANT。祖先返却で A へ巻き戻り、C のこの呼出しは中断される
  const api_result granted_a = api(object_api::RUN_FOR, tid_a, lend_cycles);
  if (granted_a.error == (uintptr_t)object_error::OK && granted_a.value == 1) {
    g_multi_ancestor_c_resumed = 1;
  }
  api(object_api::YIELD);
  return 0;
}

uintptr_t multi_ancestor_b_worker(uintptr_t tid_a, uintptr_t, uintptr_t, uintptr_t) {
  api(object_api::CREATE_OBJECT, OBJECT_SLEEPER, (uintptr_t)&multi_ancestor_c_worker, OBJECT_REPLACE);
  const api_result spawned_c = api(object_api::SPAWN, OBJECT_SLEEPER, METHOD_MAIN, tid_a);
  if (spawned_c.error == (uintptr_t)object_error::OK) {
    const uint32_t lend_cycles = 10000u * BOARD::cycles_per_us();
    // B が C に貸出。C が A へ返却すると B も unwind され、B のこの呼出しも中断される
    const api_result granted_c = api(object_api::RUN_FOR, spawned_c.value, lend_cycles);
    if (granted_c.error == (uintptr_t)object_error::OK && granted_c.value == 1) {
      g_multi_ancestor_b_saw_c_return = 1;
    }
    // 後続処理: B が C を再貸出して C の saved GRANT 呼出しからの再開を実行
    const api_result resume_c = api(object_api::RUN_FOR, spawned_c.value, lend_cycles);
    if (resume_c.error == (uintptr_t)object_error::OK && resume_c.value == 1) {
      g_multi_ancestor_b_completed = 1;
    }
  }
  api(object_api::YIELD);
  return 0;
}

// (d) mutex lock失敗時GRANT(owner)して戻り後CAS再確認しlockが取得できる
volatile uint32_t g_mutex_word = 0;
volatile uint32_t g_contender_cas1_failed = 0;
volatile uint32_t g_contender_recheck_cas_ok = 0;
uintptr_t mutex_contender(uintptr_t tid_owner, uintptr_t, uintptr_t, uintptr_t) {
  const uint32_t self = kernel_instance.current_thread_id();
  const uint32_t my_token = self + 1;
  if (!ARCH::cas32(&g_mutex_word, 0, my_token)) {
    g_contender_cas1_failed = 1;
  }
  const uint32_t lend_cycles = 10000u * BOARD::cycles_per_us();
  api(object_api::RUN_FOR, tid_owner, lend_cycles);

  if (ARCH::cas32(&g_mutex_word, 0, my_token)) {
    g_contender_recheck_cas_ok = 1;
    ARCH::cas32(&g_mutex_word, my_token, 0);
  }
  api(object_api::YIELD);
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
    // ★待ちは回数ではなく時間で区切る。YIELD 1 回の所要時間は CPU 速度で変わる
    //   (速いコアでは 5000 回が眠り (50ms) より先に尽きる)。眠りの 20 倍 (1s) まで待つ。
    for (const uint64_t wake_deadline = BOARD::time_us() + 20u * SLEEPER_SLEEP_US;
         g_sleeper_woke == 0 && BOARD::time_us() < wake_deadline;)
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

  // ---- (a) 無宛先YIELDをborrowerから実行するとlenderへ戻る -----------------
  {
    g_yield_borrower_ran = 0;
    api(object_api::CREATE_OBJECT, OBJECT_WORKER,
        (uintptr_t)&yield_borrower, OBJECT_REPLACE);
    const api_result spawned =
        api(object_api::SPAWN, OBJECT_WORKER, METHOD_MAIN, 0);
    check("yield borrower: spawn", spawned.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned.error, 0);

    const uint32_t lend_cycles = 10000u * BOARD::cycles_per_us(); // 10ms
    const uint64_t started = BOARD::time_us();
    const api_result granted =
        api(object_api::RUN_FOR, spawned.value, lend_cycles);
    const uint64_t elapsed = BOARD::time_us() - started;

    check("yield borrower: returned to lender",
          granted.error == (uintptr_t)object_error::OK && granted.value == 1,
          (unsigned long)granted.value, 1);
    check("yield borrower: ran before yield",
          g_yield_borrower_ran == 1,
          (unsigned long)g_yield_borrower_ran, 1);
    check("yield borrower: returned immediately (not expired)",
          elapsed < 5000,
          (unsigned long)elapsed, 5000);

    // Borrower cleanup and resume check: 再貸出で YIELD の直後から再開することを確認
    api(object_api::RUN_FOR, spawned.value, lend_cycles);
    check("yield borrower: resumed after yield",
          g_yield_borrower_ran == 2,
          (unsigned long)g_yield_borrower_ran, 2);
  }

  // ---- (b) A→B→C入れ子とC予算がB残量でclamp -------------------------------
  {
    // Part 1: Nesting A -> B -> C and YIELD unwinds C -> B -> A
    g_nested_c_ran = 0;
    g_nested_b_saw_c_yield = 0;
    api(object_api::CREATE_OBJECT, OBJECT_SLEEPER, (uintptr_t)&nested_b_worker, OBJECT_REPLACE);
    const api_result spawned_b =
        api(object_api::SPAWN, OBJECT_SLEEPER, METHOD_MAIN, 0);
    check("nested A->B->C: spawn B", spawned_b.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned_b.error, 0);

    const uint32_t lend_cycles = 10000u * BOARD::cycles_per_us();
    const api_result granted_b =
        api(object_api::RUN_FOR, spawned_b.value, lend_cycles);
    check("nested A->B->C: B returned to A",
          granted_b.error == (uintptr_t)object_error::OK && granted_b.value == 1,
          (unsigned long)granted_b.value, 1);
    check("nested A->B->C: C actually ran", g_nested_c_ran == 1,
          (unsigned long)g_nested_c_ran, 1);
    check("nested A->B->C: C yielded back to B", g_nested_b_saw_c_yield == 1,
          (unsigned long)g_nested_b_saw_c_yield, 1);

    // Part 2: Margin check (仕様ケース i).
    // A lends B 10ms. B lends C 2ms.
    // C runs until budget exhausted (~2ms). B still has remaining margin (~8ms),
    // so upon C's reclamation, B resumes, records completion, and yields back to A.
    g_hog_stop = 0;
    g_nested_b_saw_c_expired = 0;
    g_nested_b_saw_c_elapsed_us = 0;
    api(object_api::CREATE_OBJECT, OBJECT_SLEEPER, (uintptr_t)&nested_b_margin_worker, OBJECT_REPLACE);
    const api_result spawned_b_margin =
        api(object_api::SPAWN, OBJECT_SLEEPER, METHOD_MAIN, 0);
    check("nested margin: spawn B", spawned_b_margin.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned_b_margin.error, 0);

    const uint32_t lend_margin_cycles = 10000u * BOARD::cycles_per_us(); // 10ms
    const uint64_t expected_c_us = 2000u; // 2000us
    const uint64_t started_margin = BOARD::time_us();
    const api_result granted_margin =
        api(object_api::RUN_FOR, spawned_b_margin.value, lend_margin_cycles);
    const uint64_t elapsed_margin = BOARD::time_us() - started_margin;

    check("nested margin: C reclaimed by budget", g_nested_b_saw_c_expired == 1,
          (unsigned long)g_nested_b_saw_c_expired, 1);
    check("nested margin: C elapsed bounded by B lend (~2ms)",
          g_nested_b_saw_c_elapsed_us >= expected_c_us / 2 && g_nested_b_saw_c_elapsed_us < expected_c_us * 2,
          (unsigned long)g_nested_b_saw_c_elapsed_us, (unsigned long)expected_c_us);
    check("nested margin: B returned to A",
          granted_margin.error == (uintptr_t)object_error::OK && granted_margin.value == 1,
          (unsigned long)granted_margin.value, 1);
    check("nested margin: total elapsed within B budget",
          elapsed_margin >= expected_c_us / 2 && elapsed_margin < 15000u,
          (unsigned long)elapsed_margin, (unsigned long)expected_c_us);

    // Part 3: Clamp exhaustion check (仕様ケース ii).
    // A lends B 2ms. B asks 50ms for C, clamped to B's remaining budget (<= 2ms).
    // When C expires, all levels have budget exhausted. B must be unwound immediately
    // without running post-grant user code, and A must observe grant_end::EXPIRED (0).
    g_hog_stop = 0;
    g_nested_b_clamp_exhaust_ran = 0;
    api(object_api::CREATE_OBJECT, OBJECT_SLEEPER, (uintptr_t)&nested_b_clamp_exhaust_worker, OBJECT_REPLACE);
    const api_result spawned_b_clamp =
        api(object_api::SPAWN, OBJECT_SLEEPER, METHOD_MAIN, 0);
    check("nested clamp: spawn B clamp", spawned_b_clamp.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned_b_clamp.error, 0);

    const uint32_t lend_clamp_cycles = 2000u * BOARD::cycles_per_us(); // 2ms
    const uint64_t expected_b_us = lend_clamp_cycles / BOARD::cycles_per_us(); // 2000us
    const uint64_t started_clamp = BOARD::time_us();
    const api_result granted_clamp =
        api(object_api::RUN_FOR, spawned_b_clamp.value, lend_clamp_cycles);
    const uint64_t elapsed_clamp = BOARD::time_us() - started_clamp;

    // B の事後印は残らず (残量 0 のため再開されず巻き取られる)
    check("nested clamp: B left no mark after budget exhaustion", g_nested_b_clamp_exhaust_ran == 0,
          (unsigned long)g_nested_b_clamp_exhaust_ran, 0);
    // A の戻り値が EXPIRED (0) であること
    check("nested clamp: B unwound to A with budget expired",
          granted_clamp.error == (uintptr_t)object_error::OK && granted_clamp.value == 0,
          (unsigned long)granted_clamp.value, 0);
    // 全体の経過時間も B の貸出予算 (~2ms) のオーダに収まること (50ms ではない)
    check("nested clamp: total elapsed bounded by B budget (~2ms, not 50ms)",
          elapsed_clamp >= expected_b_us / 2 && elapsed_clamp < expected_b_us * 3,
          (unsigned long)elapsed_clamp, (unsigned long)expected_b_us);
  }

  // ---- (c) A→B→A および A→B→C→A 祖先返却と保存済み呼出しからの再開 --------
  {
    // Part 1: A -> B -> A 祖先返却と depth 未増加の検証
    g_ancestor_b_ran = 0;
    g_ancestor_b_resumed = 0;
    api(object_api::CREATE_OBJECT, OBJECT_WORKER,
        (uintptr_t)&ancestor_worker, OBJECT_REPLACE);
    const uint32_t tid_a = kernel_instance.current_thread_id();
    const api_result spawned_b =
        api(object_api::SPAWN, OBJECT_WORKER, METHOD_MAIN, tid_a);
    check("ancestor grant: spawn B", spawned_b.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned_b.error, 0);

    const uint32_t lend_cycles = 10000u * BOARD::cycles_per_us();
    const uint64_t started = BOARD::time_us();
    const api_result granted =
        api(object_api::RUN_FOR, spawned_b.value, lend_cycles);
    const uint64_t elapsed = BOARD::time_us() - started;

    check("ancestor grant: B returned to A via ancestor unwind",
          granted.error == (uintptr_t)object_error::OK && granted.value == 1,
          (unsigned long)granted.value, 1);
    check("ancestor grant: B actually ran", g_ancestor_b_ran == 1,
          (unsigned long)g_ancestor_b_ran, 1);
    check("ancestor grant: returned immediately", elapsed < 5000,
          (unsigned long)elapsed, 5000);
    // 祖先返却はフレームを積まずに unwind するため、A 復帰時の grant depth は 0。
    // 通常貸付けなら depth 2 (A->B, B->A) となり grant_active() が true になるが、
    // ここでは depth 0 に巻き戻るため grant_active() が false になる。
    check("ancestor grant: grant depth not increased (grant_active false)",
          !kernel_instance.grant_active(),
          (unsigned long)kernel_instance.grant_active(), 0);

    // Re-grant to B to let it finish
    api(object_api::RUN_FOR, spawned_b.value, lend_cycles);
    check("ancestor grant: B resumed after ancestor unwind",
          g_ancestor_b_resumed == 1,
          (unsigned long)g_ancestor_b_resumed, 1);

    // Part 2: A -> B -> C -> A 多段祖先返却と B/C 両方の saved call からの再開
    g_multi_ancestor_c_ran = 0;
    g_multi_ancestor_b_saw_c_return = 0;
    g_multi_ancestor_c_resumed = 0;
    g_multi_ancestor_b_completed = 0;

    api(object_api::CREATE_OBJECT, OBJECT_WORKER,
        (uintptr_t)&multi_ancestor_b_worker, OBJECT_REPLACE);
    const api_result spawned_multi_b =
        api(object_api::SPAWN, OBJECT_WORKER, METHOD_MAIN, tid_a);
    check("multi ancestor: spawn B", spawned_multi_b.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned_multi_b.error, 0);

    const api_result granted_multi =
        api(object_api::RUN_FOR, spawned_multi_b.value, lend_cycles);
    check("multi ancestor: returned to A via multi-unwind",
          granted_multi.error == (uintptr_t)object_error::OK && granted_multi.value == 1,
          (unsigned long)granted_multi.value, 1);
    check("multi ancestor: C ran before unwind", g_multi_ancestor_c_ran == 1,
          (unsigned long)g_multi_ancestor_c_ran, 1);
    // A 復帰時点で、B も C も保存済み呼出しで中断されており後続処理は未実行
    check("multi ancestor: B post-grant logic not executed yet",
          g_multi_ancestor_b_saw_c_return == 0,
          (unsigned long)g_multi_ancestor_b_saw_c_return, 0);
    check("multi ancestor: C post-grant logic not executed yet",
          g_multi_ancestor_c_resumed == 0,
          (unsigned long)g_multi_ancestor_c_resumed, 0);
    check("multi ancestor: depth unwound to 0",
          !kernel_instance.grant_active(),
          (unsigned long)kernel_instance.grant_active(), 0);

    // A が B を再貸出し。B は saved GRANT(C) から再開し、後続処理で C を再貸出し、
    // C も saved GRANT(A) から再開して双方が正常完了することを確認
    const api_result resumed_multi_b =
        api(object_api::RUN_FOR, spawned_multi_b.value, lend_cycles);
    check("multi ancestor: B returned after resume sequence",
          resumed_multi_b.error == (uintptr_t)object_error::OK && resumed_multi_b.value == 1,
          (unsigned long)resumed_multi_b.value, 1);
    check("multi ancestor: B resumed from saved GRANT(C)",
          g_multi_ancestor_b_saw_c_return == 1,
          (unsigned long)g_multi_ancestor_b_saw_c_return, 1);
    check("multi ancestor: C resumed from saved GRANT(A)",
          g_multi_ancestor_c_resumed == 1,
          (unsigned long)g_multi_ancestor_c_resumed, 1);
    check("multi ancestor: full sequence completed",
          g_multi_ancestor_b_completed == 1,
          (unsigned long)g_multi_ancestor_b_completed, 1);
  }

  // ---- (d) mutex lock失敗時GRANT(owner)して戻り後CAS再確認しlockが取得できる
  {
    g_mutex_word = 0;
    g_contender_cas1_failed = 0;
    g_contender_recheck_cas_ok = 0;
    const uint32_t tid_a = kernel_instance.current_thread_id();
    const uint32_t token_a = tid_a + 1;

    // 1. Owner A acquires lock
    const bool a_locked = ARCH::cas32(&g_mutex_word, 0, token_a);
    check("mutex: owner A acquires lock", a_locked && g_mutex_word == token_a,
          (unsigned long)g_mutex_word, (unsigned long)token_a);

    api(object_api::CREATE_OBJECT, OBJECT_WORKER,
        (uintptr_t)&mutex_contender, OBJECT_REPLACE);
    const api_result spawned_b =
        api(object_api::SPAWN, OBJECT_WORKER, METHOD_MAIN, tid_a);
    check("mutex: spawn contender B", spawned_b.error == (uintptr_t)object_error::OK,
          (unsigned long)spawned_b.error, 0);

    const uint32_t lend_cycles = 10000u * BOARD::cycles_per_us();
    // 2. Lend to Contender B. Contender will fail CAS, then GRANT to owner A (unwinds to A).
    const api_result granted1 =
        api(object_api::RUN_FOR, spawned_b.value, lend_cycles);
    check("mutex: contender returned to owner A via GRANT(owner)",
          granted1.error == (uintptr_t)object_error::OK && granted1.value == 1,
          (unsigned long)granted1.value, 1);
    check("mutex: contender initial CAS failed while held by A",
          g_contender_cas1_failed == 1,
          (unsigned long)g_contender_cas1_failed, 1);
    // 自動待ちキューを作らない協調方式: contender は GRANT(owner) で停止しており、
    // owner が unlock する前に後続のロック取得処理が走っていないことを検証
    check("mutex: lock still held by owner after contender returned",
          g_mutex_word == token_a,
          (unsigned long)g_mutex_word, (unsigned long)token_a);
    check("mutex: contender post-grant logic not executed before unlock",
          g_contender_recheck_cas_ok == 0,
          (unsigned long)g_contender_recheck_cas_ok, 0);

    // 3. Owner A releases lock
    const bool a_unlocked = ARCH::cas32(&g_mutex_word, token_a, 0);
    check("mutex: owner A releases lock", a_unlocked && g_mutex_word == 0,
          (unsigned long)g_mutex_word, 0);

    // 4. Lend to Contender B again. Contender resumes from saved GRANT call,
    //    re-checks CAS, acquires and releases lock.
    const api_result granted2 =
        api(object_api::RUN_FOR, spawned_b.value, lend_cycles);
    check("mutex: contender returned after lock acquisition",
          granted2.error == (uintptr_t)object_error::OK && granted2.value == 1,
          (unsigned long)granted2.value, 1);
    check("mutex: contender re-check CAS succeeded after owner unlock",
          g_contender_recheck_cas_ok == 1,
          (unsigned long)g_contender_recheck_cas_ok, 1);
    check("mutex: final mutex is unlocked", g_mutex_word == 0,
          (unsigned long)g_mutex_word, 0);

    // Restore OBJECT_WORKER and OBJECT_SLEEPER to clean state
    api(object_api::CREATE_OBJECT, OBJECT_WORKER, (uintptr_t)&worker, OBJECT_REPLACE);
    api(object_api::CREATE_OBJECT, OBJECT_SLEEPER, (uintptr_t)&sleeper, OBJECT_REPLACE);
  }

  BOARD::diag_printf("[SELFTEST] thread ladder done: %lu passed, %lu failed\n",
                     (unsigned long)passed, (unsigned long)failed);
}

} // namespace selftest
} // namespace shizuku
