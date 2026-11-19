// ===========================================================================
//  共有メールボックスのポーリング往復の実測 (実行権を渡す API で回す)
// ===========================================================================
//  呼び手の thread がメールボックスに要求を書き、サービスの thread へ実行権を渡す。
//  サービスは要求を読んで応答を書き (値 + 1 を返すだけの no-op)、実行権を返す。
//  呼び手が応答を読むまでを 1 往復として、2048 回を時間を読まずに回し、前後で
//  CYCCNT を読んで 1 回あたりの平均を出す。空のループ (呼び手が自分でサービスの
//  仕事をするだけで、切替えが無い) を引く。方式は rp2350_u_cost.cpp / call_cost.cpp と同じ。
//
//  測る版 (どれも呼び手・サービスとも特権 = M):
//    RUN_FOR : 呼び手 RUN_FOR(サービス, 十分な量) で貸し、サービスは YIELD(0) で返す。
//              サービスは借り手なので YIELD は trap 内の早期復帰 (grant_unwind)。
//    YIELD_ID: 呼び手 YIELD(サービス ID)、サービス YIELD(呼び手 ID)。
//              ★2026-11-17 時点で a1 (宛先) はカーネルが読まない (dispatch.cpp svc_yield と
//              kernel_object/handler.cpp yield() はどちらも a1 を見ない)。実効は YIELD(0) と同じで、
//              誰が走るかは schedule() 任せ。宛先が効かないことをこの版が数字で示す。
//              応答が来るまで YIELD を繰り返す (上限あり) ので、別の thread に回っても
//              往復は正しく終わり、余分に回した回数を extra として記録する。
//    YIELD_0 : 両方 YIELD(0)。比較用。他に走れる thread がいない前提で測る
//              (extra が 0 ならサービスへ直接渡っている = その前提が保たれていた)。
//
//  ★U 同士の版は作っていない (理由は notes: report1.md)。メールボックスを両方の窓へ
//    開ける仕組み (rp2350_u_cost の共有窓 / C6 の PMP 窓) と、U 用の stub 配置が要るため。
#include "shizuku/kernel.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/method_entry.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/hot.hpp"

// ★JTAG / SWD から読む記録。シンボル名は固定 (C リンケージ)。
//   読み出しは ~/github/Shizuku/.handoff/m2m-switch-20261117/mbox_c6_read.sh。
//   ★途中で止まっても状況が分かるように、stage / progress を逐次書き、各版の結果も
//     その版が終わった時点で書く (最後にまとめて書かない)。
//   stage: 1 開始 / 2 基準の warm-up / 3 基準の計測 / 4 基準 完了 /
//          版 v (0 = RUN_FOR, 1 = YIELD(id), 2 = YIELD(0)) は 10 * (v + 1) + k:
//          k = 0 spawn / 1 warm-up / 2 計測 / 3 停止 / 4 完了 / 5 遅すぎて計測を省いた
//          90 = 全部終わり
extern "C" {
struct shizuku_mbox_version_t {
  uint32_t state;        // 0 未着手 / 1 実行中 / 2 完了 / 3 spawn 失敗 / 4 遅すぎて計測を省いた
  uint32_t warmup;       // warm-up の往復数
  uint32_t rounds;       // 計測の往復数
  uint32_t raw_mean;     // 1 往復あたり (空のループを引く前)
  uint32_t net_mean;     // 空のループを引いたあと
  uint32_t ok;           // 1 = 全往復が正しい応答で終わった
  uint32_t extra;        // 計測区間で、応答待ちに余分に撃った YIELD の総数
  uint32_t other_ready;  // 版の開始時点で READY だった、呼び手とサービス以外の thread の数
  uint32_t spawn_error;  // SPAWN の error (0 = OK)
  uint32_t service_tid;
  uint32_t warm_max;     // warm-up の 1 往復の最大サイクル (遅さの切り分け用)
};
struct shizuku_mbox_cost_record_t {
  uint32_t magic;           // 0x5842424D "MBBX"
  uint32_t version;         // 2
  uint32_t stage;           // 上の stage
  uint32_t progress;        // stage を書くたびに増える。止まっているかの確認用
  uint32_t baseline_rounds;
  uint32_t baseline_mean;   // 空のループ (サービスの仕事を呼び手が自分でやる)
  uint32_t done;            // 1 = 全部終わり
  uint32_t lend_cycles;     // RUN_FOR で貸す量
  uint32_t caller_tid;
  uint32_t thread_count;
  shizuku_mbox_version_t ver[3]; // 0 RUN_FOR / 1 YIELD(id) / 2 YIELD(0)
};
volatile shizuku_mbox_cost_record_t shizuku_mbox_cost_record = {};
}
static_assert(sizeof(shizuku_mbox_version_t) == 44, "mbox version size");
static_assert(sizeof(shizuku_mbox_cost_record_t) == 172, "mbox record size");

namespace shizuku {
namespace selftest {

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_MBOX_SERVICE = object_id::mbox_service;
constexpr uintptr_t METHOD_MAIN = 0;
constexpr uint32_t BASE_ROUNDS = 2048;
constexpr uint32_t BASE_WARMUP = 256;
// RUN_FOR は 1 往復が軽いので 2048 回。YIELD の 2 版は schedule() が他の READY thread へ
// 渡して 1 往復が ms 単位 (tick 縛り) になりうるので、回数を減らす。
constexpr uint32_t RUN_FOR_ROUNDS = 2048;
constexpr uint32_t RUN_FOR_WARMUP = 64;
constexpr uint32_t YIELD_ROUNDS = 64;
constexpr uint32_t YIELD_WARMUP = 8;
constexpr uint32_t MAX_EXTRA_YIELDS = 16; // 1 往復で応答が来ないときの打切り

enum : uintptr_t { MODE_RUN_FOR = 1, MODE_YIELD_ID = 2, MODE_YIELD_0 = 3 };

// ---- メールボックス (呼び手とサービスで共有。型はどちらも uint32) -------------
volatile uint32_t g_req_seq = 0;
volatile uint32_t g_req_val = 0;
volatile uint32_t g_rsp_seq = 0;
volatile uint32_t g_rsp_val = 0;
volatile uint32_t g_stop = 0;
volatile uint32_t g_done = 0;
volatile uint32_t g_caller_tid = 0;
volatile uint32_t g_lend_cycles = 0; // RUN_FOR で貸す量 (100 ms 分のクロック)

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

[[gnu::always_inline]] inline api_result api(object_api number, uintptr_t a1 = 0,
                                             uintptr_t a2 = 0, uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  if (result.error & KERNEL_ERROR_MARK)
    return {(uintptr_t)object_error::KERNEL_REFUSED, result.error};
  return {result.error, result.value};
}

template <typename A> [[gnu::always_inline]] inline uint32_t cycles_now() {
#if defined(__riscv)
  __asm__ volatile("fence" ::: "memory");
  const uint32_t cycles = A::cycle_counter_read();
  __asm__ volatile("fence" ::: "memory");
#else
  __asm__ volatile("isb" ::: "memory");
  const uint32_t cycles = A::cycle_counter_read();
  __asm__ volatile("isb" ::: "memory");
#endif
  return cycles;
}

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

// サービスの仕事。要求があれば応答を書く (値を受けて定数を足して返すだけ)。
[[gnu::always_inline]] inline void serve_once() {
  const uint32_t seq = g_req_seq;
  if (seq != g_rsp_seq) {
    g_rsp_val = g_req_val + 1;
    g_rsp_seq = seq;
  }
}

// サービスの thread。a0 = 版 (MODE_*)。stop が立つまで、仕事 → 実行権を返す、を繰り返す。
//   ★SHIZUKU_HOT: C6 では .iram1 (flash の cache miss を計測に混ぜない)。
SHIZUKU_HOT uintptr_t mbox_service(uintptr_t mode, uintptr_t, uintptr_t, uintptr_t) {
  for (;;) {
    serve_once();
    if (g_stop != 0)
      break;
    if (mode == MODE_YIELD_ID)
      api(object_api::YIELD, g_caller_tid);
    else
      api(object_api::YIELD); // RUN_FOR 版は借り手の早期復帰、YIELD_0 版は schedule()
  }
  g_done = 1;
  return 0; // thread が終わる (借り手なら貸し手へ巻き戻る)
}

// 1 往復。戻り値 = 応答が正しかったか。extra には余分に撃った YIELD を足す。
template <uintptr_t MODE>
[[gnu::always_inline]] inline bool one_round(uint32_t i, uint32_t service_tid, uint32_t &extra) {
  g_req_val = i;
  g_req_seq = i + 1;
  if constexpr (MODE == MODE_RUN_FOR) {
    api(object_api::RUN_FOR, service_tid, g_lend_cycles); // 100 ms 分。サービスが早期復帰するので届かない
  } else {
    uint32_t tries = 0;
    do {
      api(object_api::YIELD, MODE == MODE_YIELD_ID ? service_tid : 0);
      if (g_rsp_seq == g_req_seq)
        break;
      ++extra;
    } while (++tries < MAX_EXTRA_YIELDS);
  }
  return g_rsp_seq == g_req_seq && g_rsp_val == i + 1;
}

// 空のループ。サービスの仕事を呼び手が自分でやるだけ (切替えなし)。
uint32_t run_baseline(uint32_t rounds, bool &ok) {
  bool all_ok = true;
  const uint32_t start = cycles_now<ARCH>();
  for (uint32_t i = 0; i < rounds; ++i) {
    g_req_val = i;
    g_req_seq = g_req_seq + 1;
    serve_once();
    all_ok = all_ok && (g_rsp_seq == g_req_seq && g_rsp_val == i + 1);
  }
  const uint32_t end = cycles_now<ARCH>();
  ok = all_ok;
  return end - start;
}

template <uintptr_t MODE>
uint32_t run_rounds(uint32_t rounds, uint32_t service_tid, bool &ok, uint32_t &extra) {
  bool all_ok = true;
  const uint32_t start = cycles_now<ARCH>();
  for (uint32_t i = 0; i < rounds; ++i)
    all_ok = one_round<MODE>(i, service_tid, extra) && all_ok;
  const uint32_t end = cycles_now<ARCH>();
  ok = all_ok;
  return end - start;
}

void stage(uint32_t value) {
  shizuku_mbox_cost_record.stage = value;
  shizuku_mbox_cost_record.progress = shizuku_mbox_cost_record.progress + 1;
}

// 呼び手とサービス以外で READY の thread の数 (YIELD の 2 版は、これが 0 でないと schedule() が
// そちらへ渡って往復が遅くなる)。
uint32_t count_other_ready(uint32_t self, uint32_t service) {
  uint32_t count = 0;
  const uint32_t total = kernel_instance.thread_count();
  for (uint32_t i = 0; i < total; ++i) {
    if (i == self || i == service)
      continue;
    if (kernel_instance.thread_state(i) == KERNEL::THREAD::state_t::READY)
      ++count;
  }
  return count;
}

// warm-up。1 往復ずつ時間を読んで、最大を記録する (計測区間の外。遅さの切り分け用)。
// 往復ごとに progress を進める。
template <uintptr_t MODE>
uint32_t warm_up(uint32_t rounds, uint32_t service_tid, bool &ok, uint32_t &extra) {
  uint32_t worst = 0;
  bool all_ok = true;
  for (uint32_t i = 0; i < rounds; ++i) {
    const uint32_t start = cycles_now<ARCH>();
    all_ok = one_round<MODE>(i, service_tid, extra) && all_ok;
    const uint32_t spent = cycles_now<ARCH>() - start;
    if (spent > worst)
      worst = spent;
    shizuku_mbox_cost_record.progress = shizuku_mbox_cost_record.progress + 1;
  }
  ok = all_ok;
  return worst;
}

// 1 版を走らせる。spawn → warm-up → 計測 → サービスを止める。結果は終わった時点で記録へ書く。
//   idx = 0 RUN_FOR / 1 YIELD(id) / 2 YIELD(0)。戻り値 = spawn できたか。
template <uintptr_t MODE>
bool measure(uint32_t idx, uint32_t warmup, uint32_t rounds, uint32_t baseline_mean,
             bool &ok_out, bool &finished_out) {
  auto &rec = shizuku_mbox_cost_record;
  auto &ver = rec.ver[idx];
  const uint32_t base = 10 * (idx + 1);
  ver.warmup = warmup;
  ver.rounds = rounds;
  ver.state = 1;
  g_stop = 0;
  g_done = 0;
  const uint32_t self = kernel_instance.current_thread_id();
  g_caller_tid = self;
  stage(base + 0);
  const api_result spawned = api(object_api::SPAWN, OBJECT_MBOX_SERVICE, METHOD_MAIN, MODE);
  ver.spawn_error = (uint32_t)spawned.error;
  if (spawned.error != (uintptr_t)object_error::OK) {
    ver.state = 3;
    return false;
  }
  const uint32_t tid = (uint32_t)spawned.value;
  ver.service_tid = tid;
  ver.other_ready = count_other_ready(self, tid);
  // warm-up (数値は使わない)。req_seq = i + 1 に揃えるため、前後で seq を 0 に戻す。
  stage(base + 1);
  g_req_seq = 0;
  g_rsp_seq = 0;
  uint32_t warm_extra = 0;
  bool warm_ok = true;
  ver.warm_max = warm_up<MODE>(warmup, tid, warm_ok, warm_extra);
  g_req_seq = 0;
  g_rsp_seq = 0;
  // RUN_FOR 版で warm-up の 1 往復が 2 ms 超なら、早期復帰が効いていない (貸した量が尽きるまで
  // 走っている) ので、2048 回の計測は省く (数分かかる)。
  const bool too_slow = (MODE == MODE_RUN_FOR) && ver.warm_max > 2000u * BOARD::cycles_per_us();
  uint32_t total = 0;
  bool ok = warm_ok;
  uint32_t extra = 0;
  if (too_slow) {
    stage(base + 5);
    ver.state = 4;
  } else {
    stage(base + 2);
    bool timed_ok = true;
    total = run_rounds<MODE>(rounds, tid, timed_ok, extra);
    ok = ok && timed_ok;
    const uint32_t mean = total / rounds;
    ver.raw_mean = mean;
    ver.net_mean = mean > baseline_mean ? mean - baseline_mean : 0u;
    ver.extra = extra;
    ver.ok = ok ? 1u : 0u;
  }
  // サービスを止める。stop を立ててもう 1 度実行権を渡す。
  stage(base + 3);
  g_stop = 1;
  if constexpr (MODE == MODE_RUN_FOR) {
    api(object_api::RUN_FOR, tid, g_lend_cycles);
  } else {
    for (uint32_t tries = 0; tries < MAX_EXTRA_YIELDS && g_done == 0; ++tries)
      api(object_api::YIELD, MODE == MODE_YIELD_ID ? tid : 0);
  }
  finished_out = g_done != 0;
  ok_out = ok && !too_slow;
  if (!too_slow)
    ver.state = 2;
  stage(base + 4);
  return true;
}

void print_version(const char *name, const volatile shizuku_mbox_version_t &v, uint32_t baseline) {
  BOARD::diag_printf(
      "[COST-MBOX] %-24s net mean %5lu cyc  [raw mean %lu, baseline mean %lu, rounds %lu, "
      "extra yields %lu, other ready %lu, warm max %lu, state %lu]\n",
      name, (unsigned long)v.net_mean, (unsigned long)v.raw_mean, (unsigned long)baseline,
      (unsigned long)v.rounds, (unsigned long)v.extra, (unsigned long)v.other_ready,
      (unsigned long)v.warm_max, (unsigned long)v.state);
}

} // namespace

void mailbox_switch() {
  auto &rec = shizuku_mbox_cost_record;
  rec.magic = 0x5842424Du;
  rec.version = 2;
  stage(1);
  BOARD::diag_printf("[COST-MBOX] mailbox polling bench start (M<->M)\n");
  g_lend_cycles = 100000u * BOARD::cycles_per_us();
  rec.lend_cycles = g_lend_cycles;
  rec.caller_tid = kernel_instance.current_thread_id();
  rec.thread_count = kernel_instance.thread_count();
  api(object_api::CREATE_OBJECT, OBJECT_MBOX_SERVICE, method_entry<ARCH, &mbox_service>(), 0);

  // 基準 (空のループ)。
  bool base_ok = true;
  stage(2);
  run_baseline(BASE_WARMUP, base_ok);
  g_req_seq = 0;
  g_rsp_seq = 0;
  stage(3);
  const uint32_t baseline_total = run_baseline(BASE_ROUNDS, base_ok);
  const uint32_t baseline_mean = baseline_total / BASE_ROUNDS;
  rec.baseline_rounds = BASE_ROUNDS;
  rec.baseline_mean = baseline_mean;
  stage(4);

  bool ok_run_for = false, ok_yield_id = false, ok_yield_0 = false;
  bool fin_run_for = false, fin_yield_id = false, fin_yield_0 = false;
  const bool spawned_run_for = measure<MODE_RUN_FOR>(0, RUN_FOR_WARMUP, RUN_FOR_ROUNDS,
                                                     baseline_mean, ok_run_for, fin_run_for);
  print_version("RUN_FOR / YIELD(0)", rec.ver[0], baseline_mean);
  const bool spawned_yield_id = measure<MODE_YIELD_ID>(1, YIELD_WARMUP, YIELD_ROUNDS,
                                                       baseline_mean, ok_yield_id, fin_yield_id);
  print_version("YIELD(id) / YIELD(id)", rec.ver[1], baseline_mean);
  const bool spawned_yield_0 = measure<MODE_YIELD_0>(2, YIELD_WARMUP, YIELD_ROUNDS,
                                                     baseline_mean, ok_yield_0, fin_yield_0);
  print_version("YIELD(0) / YIELD(0)", rec.ver[2], baseline_mean);

  check("mbox: service thread spawned", spawned_run_for && spawned_yield_id && spawned_yield_0,
        (spawned_run_for ? 1ul : 0ul) + (spawned_yield_id ? 2ul : 0ul) + (spawned_yield_0 ? 4ul : 0ul), 7);
  check("mbox: RUN_FOR round trips all answered correctly", base_ok && ok_run_for,
        ok_run_for ? 1ul : 0ul, 1);
  check("mbox: YIELD(id) round trips all answered correctly", ok_yield_id,
        ok_yield_id ? 1ul : 0ul, 1);
  check("mbox: YIELD(0) round trips all answered correctly", ok_yield_0,
        ok_yield_0 ? 1ul : 0ul, 1);
  check("mbox: service threads finished", fin_run_for && fin_yield_id && fin_yield_0,
        (fin_run_for ? 1ul : 0ul) + (fin_yield_id ? 2ul : 0ul) + (fin_yield_0 ? 4ul : 0ul), 7);
  stage(90);
  rec.done = 1;
  BOARD::diag_printf("[COST-MBOX] done\n");
}

} // namespace selftest
} // namespace shizuku
