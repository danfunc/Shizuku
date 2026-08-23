// ===========================================================================
//  自己ホスト型デバッグの機構が動くか (DESIGN §16 / docs D40)
// ===========================================================================
//  ★プローブを繋がずに、**ファーム自身が**ブレークポイントを掛けて止め、また
//    走らせられるか。halting debug と違って DebugMonitor は優先度を持つ普通の
//    例外なので、「そのスレッドだけ止めて他は走り続ける」ができるはず — それを
//    確かめる。できていれば、この上に GDB の stub を載せられる。
//  ★見るのは「止まった」だけではない。**他が走り続けていること**まで見ないと、
//    halting debug と同じものを作っただけになる。
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_TARGET = object_id::debug_target;
constexpr uintptr_t METHOD_MAIN = 0;

volatile uint32_t g_rounds = 0;
volatile uint32_t g_stop = 0;

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

// ★ブレークポイントを置く先。inline されるとアドレスが無くなるので noinline。
[[gnu::noinline]] void breakpoint_site() { g_rounds = g_rounds + 1; }

uintptr_t debug_target(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  while (g_stop == 0) {
    breakpoint_site();
    api(object_api::YIELD);
  }
  return 0;
}

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

// しばらく譲って、相手に進む機会を与える。
uint32_t spin(uint32_t rounds) {
  for (uint32_t index = 0; index < rounds; ++index)
    api(object_api::YIELD);
  return g_rounds;
}

} // namespace

void debug_ladder() {
  using state_t = KERNEL::THREAD::state_t;
  BOARD::diag_printf("[SELFTEST] debug ladder start\n");

  // ★「立てた」ではなく「立ったか」を読む。プローブが繋がっていると
  //   C_DEBUGEN が勝って MON_EN は立たない — そのときは静かに諦める。
  ARCH::debug_enable(true);
  check("debug: self-hosted debug is available", ARCH::debug_enabled(), 1, 1);
  if (!ARCH::debug_enabled()) {
    BOARD::diag_printf("[SELFTEST] debug ladder skipped (probe attached?)\n");
    return;
  }
  // ★数はデータシートではなく**実装から読む**。
  const uint32_t comparators = ARCH::breakpoint_count();
  BOARD::diag_printf("[SELFTEST] hardware breakpoints available: %lu\n",
                     (unsigned long)comparators);
  check("debug: at least one hardware breakpoint", comparators >= 1,
        (unsigned long)comparators, 1);
  if (comparators == 0)
    return;

  api(object_api::CREATE_OBJECT, OBJECT_TARGET, (uintptr_t)&debug_target, 0);
  const api_result spawned =
      api(object_api::SPAWN, OBJECT_TARGET, METHOD_MAIN, 0);
  check("debug: target spawned", spawned.error == 0,
        (unsigned long)spawned.error, 0);

  // まず素で走ることを確かめる (止まったことが分かるための基準)。
  const uint32_t before_arm = spin(50);
  check("debug: the target runs before we arm anything", before_arm > 0,
        (unsigned long)before_arm, 1);

  // ---- ブレークポイントを掛ける -------------------------------------------
  const uint32_t events_before = kernel_instance.debug_event_count();
  ARCH::breakpoint_enable(true);
  ARCH::breakpoint_set(0, (uintptr_t)&breakpoint_site);
  for (uint32_t guard = 0;
       guard < 4000 && kernel_instance.debug_event_count() == events_before;
       ++guard)
    api(object_api::YIELD);
  const auto &event = kernel_instance.debug_event();
  ARCH::breakpoint_clear(0);

  check("debug: the breakpoint was hit",
        kernel_instance.debug_event_count() > events_before,
        (unsigned long)kernel_instance.debug_event_count(),
        (unsigned long)(events_before + 1));
  check("debug: it was a breakpoint, not something else",
        (event.reason & ARCH::DFSR_BKPT) != 0, (unsigned long)event.reason,
        (unsigned long)ARCH::DFSR_BKPT);
  check("debug: it stopped the target thread", event.thread == spawned.value,
        (unsigned long)event.thread, (unsigned long)spawned.value);
  check("debug: the stop landed on the breakpoint site",
        (event.pc & ~1u) == ((uintptr_t)&breakpoint_site & ~1u),
        (unsigned long)event.pc,
        (unsigned long)((uintptr_t)&breakpoint_site & ~1u));
  check("debug: the thread is suspended, not dead",
        kernel_instance.thread_state((uint32_t)spawned.value) ==
            state_t::SUSPENDED,
        (unsigned long)kernel_instance.thread_state((uint32_t)spawned.value),
        (unsigned long)state_t::SUSPENDED);

  // ★★本命。**止まっているのはそのスレッドだけ**で、こちらは走り続けている。
  //   halting debug ならここまで来られない (コアごと止まる)。
  const uint32_t frozen = g_rounds;
  const uint32_t still_frozen = spin(200);
  check("debug: the stopped thread makes no progress", still_frozen == frozen,
        (unsigned long)still_frozen, (unsigned long)frozen);
  check("debug: the rest of the system kept running", true, 1, 1);

  // ---- 再開する -----------------------------------------------------------
  // ★止めた文脈は生きたままなので、そのまま続きから走る (終わらせたのではない)。
  kernel_instance.resume((uint32_t)spawned.value);
  const uint32_t after_resume = spin(200);
  check("debug: it resumes where it stopped", after_resume > frozen,
        (unsigned long)after_resume, (unsigned long)(frozen + 1));

  // ---- 1 命令だけ実行する -------------------------------------------------
  // ★MON_STEP は**コア単位**でスレッド単位ではない。次にそのコアで実行される
  //   1 命令が対象になるので、狙ったスレッドを刻みたいなら「そのコアで他が
  //   走らない」状態を作る必要がある。ここでは自分自身で試す — 自分は復帰先なので
  //   カーネルは渡す先を見つけられず、記録だけ残して走らせ続ける (その経路の確認も
  //   兼ねる)。
  const uint32_t step_before = kernel_instance.debug_event_count();
  ARCH::debug_step(true);
  // ★"memory" を付ける。付けないと、この後の「回数を読む」がここより**上へ
  //   吊り上げられ**、事象が起きているのに古い値で判定してしまう。
  //   実際に踏んだ: got 2 want 2 なのに FAIL という、条件と表示がずれた形で出た。
  __asm__ volatile("nop" ::: "memory");
  const auto &stepped = kernel_instance.debug_event();
  check("debug: single-stepping produced an event",
        kernel_instance.debug_event_count() > step_before,
        (unsigned long)kernel_instance.debug_event_count(),
        (unsigned long)(step_before + 1));
  check("debug: the step was reported as a halt, not a breakpoint",
        (stepped.reason & ARCH::DFSR_HALTED) != 0,
        (unsigned long)stepped.reason, (unsigned long)ARCH::DFSR_HALTED);
  check("debug: a thread with nowhere to hand over keeps running",
        (stepped.reason & KERNEL::DEBUG_NOT_STOPPED) != 0,
        (unsigned long)(stepped.reason >> 31), 1);

  g_stop = 1; // 片付け
  spin(100);
  BOARD::diag_printf("[SELFTEST] debug ladder done\n");
}

} // namespace selftest
} // namespace shizuku
