// ===========================================================================
//  2 コア目が本当にスレッドを走らせているかを確かめる (DESIGN §16)
// ===========================================================================
//  ★「起こした」は主張であって証拠ではない。core1 が multicore_launch_core1 から
//    戻ってきただけでも「起きた」ように見えるし、アイドルが空回りしているだけでも
//    「動いている」ように見える。**普通のスレッドが core1 の上で走った**ことを、
//    そのスレッド自身に申告させる (非特権プローブと同じ作法 — §11.2.0 の教訓)。
#include "shizuku/kernel.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_COUNTER = object_id::counter;
constexpr uintptr_t METHOD_MAIN = 0;
constexpr uintptr_t WORKERS = 4;
constexpr uintptr_t ROUNDS = 200;

// ★スロットは 1 スレッドにつき 1 つで、書くのはその本人だけ。共有カウンタを
//   2 コアから増やすと、それ自体が競合になって「試験が試験を壊す」。
volatile uint32_t g_cores_seen[WORKERS];
volatile uint32_t g_done[WORKERS];

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

// 走るたびに「今どのコアに居るか」を自分の欄へ足す。
uintptr_t counter(uintptr_t slot, uintptr_t, uintptr_t, uintptr_t) {
  for (uintptr_t round = 0; round < ROUNDS; ++round) {
    g_cores_seen[slot] |= 1u << BOARD::core_num();
    api(object_api::YIELD);
  }
  g_done[slot] = 1;
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

} // namespace

void multicore_probe() {
  if (KERNEL::CORE_COUNT < 2) {
    BOARD::diag_printf("[SELFTEST] multicore probe skipped (1 core)\n");
    return;
  }
  BOARD::diag_printf("[SELFTEST] multicore probe start\n");

  api(object_api::CREATE_OBJECT, OBJECT_COUNTER, (uintptr_t)&counter, 0);
  uintptr_t spawned = 0;
  for (uintptr_t slot = 0; slot < WORKERS; ++slot) {
    g_cores_seen[slot] = 0;
    g_done[slot] = 0;
    // ★affinity は指定しない = どちらのコアでも走ってよい。**どちらで走るかを
    //   こちらが決めないこと**が要点 — 決めてしまうと「渡す機構が効いている」
    //   ではなく「決めた通りに動いた」しか確かめられない。
    const api_result result =
        api(object_api::SPAWN, OBJECT_COUNTER, METHOD_MAIN, slot);
    if (result.error == 0)
      ++spawned;
  }
  check("multicore: workers spawned", spawned == WORKERS,
        (unsigned long)spawned, (unsigned long)WORKERS);

  // 全員が終わるまで譲る。★ここで固まるなら「2 コア目が実行権を返してこない」
  //   という別の壊れ方なので、上限を切って抜ける。
  for (uint32_t guard = 0; guard < 20000; ++guard) {
    uintptr_t finished = 0;
    for (uintptr_t slot = 0; slot < WORKERS; ++slot)
      finished += g_done[slot];
    if (finished == WORKERS)
      break;
    api(object_api::YIELD);
  }

  uint32_t cores = 0;
  uintptr_t finished = 0;
  for (uintptr_t slot = 0; slot < WORKERS; ++slot) {
    cores |= g_cores_seen[slot];
    finished += g_done[slot];
  }
  check("multicore: every worker finished", finished == WORKERS,
        (unsigned long)finished, (unsigned long)WORKERS);
  // ★本命。core1 の上で**普通のスレッドが走った**ことを、そのスレッド自身が
  //   申告した値で確かめる。core0 だけなら bit1 は立たない。
  check("multicore: work ran on the second core", (cores & 0b10) != 0,
        (unsigned long)cores, 0b10);
  check("multicore: work ran on the first core too", (cores & 0b01) != 0,
        (unsigned long)cores, 0b01);
  BOARD::diag_printf("[SELFTEST] multicore probe done (cores seen: 0x%lx)\n",
                     (unsigned long)cores);
}

} // namespace selftest
} // namespace shizuku
