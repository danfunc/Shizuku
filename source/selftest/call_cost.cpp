// ===========================================================================
//  呼び出しコストの実測 (svc 往復 / メソッド呼び出し 1 段)
// ===========================================================================
//  発表資料が「約 600 cycle」と主張してきた数字には、**測定コードも記録も
//  リポジトリに存在しなかった** (docs/04_work_instructions.md:263 が
//  「参照実装と同等のアプリなしベンチ (svc 往復 / CALL 往復のサイクル数) を記録」
//  を未着手の TODO として残している)。これはその TODO を埋めるもの。
//
//  ★測るのは 4 つ。差を取れるように**同じ枠組みで**測る:
//      (1) からっぽ       — 測定そのものの費用 (関数ポインタ経由 + CYCCNT 2 回読み)
//      (2) 直接関数呼び出し — 保護も何も挟まない素の呼び出し (比較の下限)
//      (3) svc 往復       — オブジェクト → トランポリン → ハンドラ → 戻り
//      (4) メソッド呼び出し — (3) に加えて CALL プリミティブで呼び先を起こし、
//                            呼び先が戻り、RETURN で畳むまで (= 「600 cycle」の主張)
//      (5) 2 段ネスト     — (4) の 1 段あたりの限界費用を出すため
//    (1) を引かないと、測定の費用が全部に上乗せされたまま比較することになる。
//
//  ★DWT CYCCNT を使う。**数えているのはコアのクロック**なので、クロックを変えても
//    サイクル数は変わらない (µs へ直すときだけ board にクロックを訊く)。
//  ★★min を採る。系は動いたまま (SysTick / USB / 2 コア目) 測っているので、
//    平均には割り込みが混ざる。**割り込みの入らなかった 1 回**が知りたい値なので
//    最小値を代表にし、平均も併記して「どれだけ揺れたか」が見えるようにする。
#include "shizuku/kernel.hpp"
#include "shizuku/svc_profile.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_COST_LEAF = object_id::cost_leaf;
constexpr uintptr_t OBJECT_COST_NEST = object_id::cost_nest;
constexpr uintptr_t METHOD_MAIN = 0;

// ★isb で挟む。挟まないとコンパイラとパイプラインが読み出しを前後へ動かせるので、
//   測っている区間が実際の区間とずれる。isb 自身の費用は (1) にも同じだけ乗るので
//   引き算で消える。
template <typename A> inline uint32_t cycles_now() {
  __asm__ volatile("isb" ::: "memory");
  const uint32_t cycles = A::cycle_counter_read();
  __asm__ volatile("isb" ::: "memory");
  return cycles;
}

// ---- 測る相手 ---------------------------------------------------------------
struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  SHIZUKU_PROFILE(0);
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  SHIZUKU_PROFILE(9);
  if (result.error & KERNEL_ERROR_MARK)
    return {(uintptr_t)object_error::KERNEL_REFUSED, result.error};
  return {result.error, result.value};
}

api_result call_method(uintptr_t object, uintptr_t argument) {
  return api(object_api::CALL_METHOD, object, METHOD_MAIN, argument);
}

// 呼び先。**何もしない**。ここに仕事を入れると、測っているのが呼び出しの費用なのか
// 仕事の費用なのか分からなくなる。
uintptr_t cost_leaf(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  return argument + 1;
}

// 1 段深いだけの相手。(4) との差が「1 段あたりの限界費用」になる。
uintptr_t cost_nest(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  return call_method(OBJECT_COST_LEAF, argument).value;
}

// 素の関数呼び出しの比較用。★noinline を付けないと消える (消えたことに気づかず
// 「関数呼び出しは 0 サイクル」と報告する事故になる)。
__attribute__((noinline)) uintptr_t plain_callee(uintptr_t argument) {
  __asm__ volatile("" ::: "memory");
  return argument + 1;
}

} // namespace

// ★起動時の診断は**ホストが繋ぐ前に流れて消える**。実際に一度落とした
//   (USB CDC のバッファが溢れ、行の途中で切れた)。なので結果は生存表示へ載せる。
uint32_t cost_baseline = 0;
uint32_t cost_direct = 0;
uint32_t cost_svc = 0;
uint32_t cost_call1 = 0;
uint32_t cost_call2 = 0;

namespace {

volatile uintptr_t g_argument = 41;
volatile uintptr_t g_sink = 0;

// ★引数は volatile 読みではなく**引数として**渡す。(1) と (2)〜(5) で
//   「関数の入口・出口」だけが比較対象になるように揃えるため。
// ★★全部 noinline。付けないとコンパイラが (1) を measure の中へ展開して
//   「単なる代入」に潰し、**測定枠の費用が 0 に見える** — その結果 (3)(4) から
//   引くべき分が引かれず、全部の値が水増しされる。
using op_t = uintptr_t (*)(uintptr_t);

__attribute__((noinline)) uintptr_t op_nothing(uintptr_t argument) {
  __asm__ volatile("" ::: "memory");
  return argument;
}
__attribute__((noinline)) uintptr_t op_direct(uintptr_t argument) {
  return plain_callee(argument);
}
__attribute__((noinline)) uintptr_t op_svc(uintptr_t) {
  return api(object_api::GET_CURRENT_OBJECT).value;
}
__attribute__((noinline)) uintptr_t op_call(uintptr_t argument) {
  return call_method(OBJECT_COST_LEAF, argument).value;
}
__attribute__((noinline)) uintptr_t op_call2(uintptr_t argument) {
  return call_method(OBJECT_COST_NEST, argument).value;
}

struct stat_t {
  uint32_t min;
  uint32_t mean;
};

// ★op は関数ポインタで呼ぶ。(1) も同じ経路を通るので、ポインタ経由の分は
//   差し引きで消える。インライン展開の差で不公平が出るのも防げる。
template <typename A>
__attribute__((noinline)) stat_t measure(op_t requested, uint32_t rounds) {
  // ★volatile を 1 枚挟む。挟まないと measure(op_nothing, ...) のような
  //   「定数の関数ポインタ」をコンパイラが見抜いて呼び先ごとに特殊化し、
  //   (1) だけ別物の (軽い) コードになって対称性が壊れる。
  volatile op_t slot = requested;
  const op_t op = slot;
  // ★この経路そのものを温める。呼び先を温めるのではなく、**measure の中から
  //   ポインタ経由で呼ぶ道**を温めないと、1 回目の分岐予測ミスが min に残る。
  for (uint32_t warm = 0; warm < 32; ++warm)
    g_sink = op(g_argument);
  uint32_t best = 0xFFFFFFFFu;
  uint64_t total = 0;
  for (uint32_t round = 0; round < rounds; ++round) {
    const uint32_t started = cycles_now<A>();
    g_sink = op(g_argument);
    const uint32_t ended = cycles_now<A>();
    const uint32_t elapsed = ended - started; // 32bit の巻き戻りはこれで正しい
    if (elapsed < best)
      best = elapsed;
    total += elapsed;
  }
  return {best, (uint32_t)(total / rounds)};
}

#ifdef SHIZUKU_PROFILE_ACTIVE
// A separate, instrumented sample: sums of adjacent timestamp intervals by
// destination stage. Nested calls remain ordered; no single-slot overwrites.
// Pacing is outside every timed sample. CDC drops whole lines on FIFO full.
void cost_output_pace() {
  const uint64_t started = BOARD::time_us();
  while (BOARD::time_us() - started < 10000) { __asm__ volatile("" ::: "memory"); }
}
void profile_report(const char *name, op_t op) {
  const uint32_t core = BOARD::core_num();
  uint32_t best[13], count[13]{};
  uint64_t sum[13]{};
  for (auto &v : best) v = UINT32_MAX;
  uint32_t overflow = 0, events_min = UINT32_MAX, events_max = 0;
  uint32_t mpu_min = UINT32_MAX, updates = 0, writes = 0;
  uint64_t mpu_sum = 0, trace_sum = 0;
  uint32_t trace_min = UINT32_MAX, hits[13]{};
  for (unsigned round = 0; round < 128; ++round) {
    svc_profile_count[core] = 0;
    svc_profile_mpu_cycles[core] = svc_profile_mpu_updates[core] = svc_profile_mpu_writes[core] = 0;
    svc_profile_enabled[core] = 1;
    __asm__ volatile("" ::: "memory");
    g_sink = op(g_argument);
    __asm__ volatile("" ::: "memory");
    svc_profile_enabled[core] = 0;
    const uint32_t n = svc_profile_count[core];
    if (n < events_min) events_min = n;
    if (n > events_max) events_max = n;
    const uint32_t mpu = svc_profile_mpu_cycles[core];
    if (mpu < mpu_min) mpu_min = mpu;
    mpu_sum += mpu;
    updates = svc_profile_mpu_updates[core]; writes = svc_profile_mpu_writes[core];
    if (n == 256) { ++overflow; continue; }
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t stage = svc_profile_events[core][i].stage;
      if (stage < 13) ++hits[stage];
    }
    if (n > 1) {
      const uint32_t elapsed = svc_profile_events[core][n-1].cycle - svc_profile_events[core][0].cycle;
      if (elapsed < trace_min) trace_min = elapsed;
      trace_sum += elapsed;
    }
    uint32_t totals[13]{};
    for (uint32_t i = 1; i < n; ++i) {
      const uint32_t stage = svc_profile_events[core][i].stage;
      if (stage < 13)
        totals[stage] += svc_profile_events[core][i].cycle -
                         svc_profile_events[core][i-1].cycle;
    }
    for (unsigned i = 0; i < 13; ++i) {
      if (!totals[i]) continue;
      if (totals[i] < best[i]) best[i] = totals[i];
      sum[i] += totals[i]; ++count[i];
    }
  }
  const char *labels[] = {"caller_pre", "exception_entry", "context_saved",
      "dispatch", "push_begin", "push_end", "handler_begin", "handler_end",
      "return_stub", "caller_resume", "mpu_begin", "mpu_end", "restore_end"};
  for (unsigned i = 0; i < 13; ++i)
    if (count[i]) {
      cost_output_pace();
      BOARD::diag_printf(
        "[COST] profile %s to=%s min=%lu mean=%lu cyc samples=%lu overflow=%lu\n",
        name, labels[i], (unsigned long)best[i],
        (unsigned long)(sum[i]/count[i]), (unsigned long)count[i],
        (unsigned long)overflow);
    }
  cost_output_pace();
  BOARD::diag_printf("[COST] coverage %s events_min=%lu events_max=%lu overflow=%lu\n", name,
      (unsigned long)events_min, (unsigned long)events_max, (unsigned long)overflow);
  cost_output_pace();
  BOARD::diag_printf("[COST] hits %s stages0..12=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n", name,
      (unsigned long)(hits[0]/128), (unsigned long)(hits[1]/128), (unsigned long)(hits[2]/128),
      (unsigned long)(hits[3]/128), (unsigned long)(hits[4]/128), (unsigned long)(hits[5]/128),
      (unsigned long)(hits[6]/128), (unsigned long)(hits[7]/128), (unsigned long)(hits[8]/128),
      (unsigned long)(hits[9]/128), (unsigned long)(hits[10]/128), (unsigned long)(hits[11]/128), (unsigned long)(hits[12]/128));
  cost_output_pace();
  BOARD::diag_printf("[COST] trace_total %s min=%lu mean=%lu cyc (enabled hook overhead included)\n", name,
      (unsigned long)trace_min, (unsigned long)(trace_sum/128));
  cost_output_pace();
  BOARD::diag_printf("[COST] mpu_body %s min=%lu mean=%lu cyc updates_last=%lu mmio_writes_last=%lu dsb=0 isb=0\n", name,
      (unsigned long)mpu_min, (unsigned long)(mpu_sum/128), (unsigned long)updates, (unsigned long)writes);
}
#endif

// 引き算した結果を「サイクル / ナノ秒」で 1 行にする。
void report(const char *name, stat_t raw, uint32_t baseline,
            uint32_t cycles_per_us) {
  const uint32_t net = raw.min > baseline ? raw.min - baseline : 0;
  const uint32_t net_mean = raw.mean > baseline ? raw.mean - baseline : 0;
  // ns = cycle * 1000 / (cycles/µs)。整数のまま計算する (FP を測定路に持ち込まない)。
  const uint32_t ns = cycles_per_us ? (net * 1000u) / cycles_per_us : 0;
#ifdef SHIZUKU_PROFILE_ACTIVE
  cost_output_pace();
#endif
  BOARD::diag_printf(
      "[COST] %-22s min %5lu cyc (%4lu ns)  mean %5lu cyc  [raw min %lu]\n",
      name, (unsigned long)net, (unsigned long)ns, (unsigned long)net_mean,
      (unsigned long)raw.min);
}


// ★ARCH::HAS_CYCLE_COUNTER が false の arch ではこのテンプレートは実体化されない。
//   CYCCNT の番地も有効化手順も arch 側にあり、ここには DWT を知るコードが無い。
template <typename A> void bench() {
  BOARD::diag_printf("[COST] call cost bench start\n");
  if (!A::cycle_counter_enable()) {
    // ★測れないなら測れないと言う。0 を報告して数字があるように見せない。
    BOARD::diag_printf("[COST] FAIL DWT CYCCNT is not counting - no numbers\n");
    record_fail("COST DWT CYCCNT is not counting", 0, 1);
    ++failed;
    return;
  }
  ++passed;

  const uint32_t cycles_per_us = BOARD::cycles_per_us();
  BOARD::diag_printf("[COST] clk_sys = %lu cycles/us\n",
                     (unsigned long)cycles_per_us);

  struct entry_t {
    uintptr_t object;
    uintptr_t (*main)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
  } const entries[] = {{OBJECT_COST_LEAF, cost_leaf},
                       {OBJECT_COST_NEST, cost_nest}};
  for (const entry_t &entry : entries) {
    const api_result created =
        api(object_api::CREATE_OBJECT, entry.object, (uintptr_t)entry.main);
    if (created.error != (uintptr_t)object_error::OK) {
      BOARD::diag_printf("[COST] FAIL create object %lu: error %lu\n",
                         (unsigned long)entry.object,
                         (unsigned long)created.error);
      ++failed;
      return;
    }
  }

  // ★ウォームアップは measure の中で行う (呼び出し経路そのものを温めるため)。

  constexpr uint32_t ROUNDS = 2048;
  const stat_t nothing = measure<A>(op_nothing, ROUNDS);
  const stat_t direct = measure<A>(op_direct, ROUNDS);
  const stat_t svc = measure<A>(op_svc, ROUNDS);
  const stat_t call1 = measure<A>(op_call, ROUNDS);
  const stat_t call2 = measure<A>(op_call2, ROUNDS);

  BOARD::diag_printf("[COST] harness baseline = %lu cyc (subtracted below)\n",
                     (unsigned long)nothing.min);
  report("direct call", direct, nothing.min, cycles_per_us);
  report("svc round trip", svc, nothing.min, cycles_per_us);
  report("method call (1 hop)", call1, nothing.min, cycles_per_us);
  report("method call (2 hops)", call2, nothing.min, cycles_per_us);

  const auto net = [&](stat_t value) -> uint32_t {
    return value.min > nothing.min ? value.min - nothing.min : 0;
  };
  cost_baseline = nothing.min;
  cost_direct = net(direct);
  cost_svc = net(svc);
  cost_call1 = net(call1);
  cost_call2 = net(call2);

  // 1 段あたりの限界費用。1 段目には呼び出し元側の svc が 1 回だけ余分に入るので、
  // 差を取ると「純粋にもう 1 段積む費用」が出る。
  if (call2.min > call1.min)
    BOARD::diag_printf("[COST] marginal per extra hop = %lu cyc\n",
                       (unsigned long)(call2.min - call1.min));

  // 検算: 呼び出しが正しい答えを返していること。速いだけで壊れていては意味がない。
  const api_result checked = call_method(OBJECT_COST_LEAF, 41);
  if (checked.error == (uintptr_t)object_error::OK && checked.value == 42) {
    ++passed;
    BOARD::diag_printf("[COST] PASS method call still returns 42\n");
  } else {
    ++failed;
    BOARD::diag_printf("[COST] FAIL method call: error %lu value %lu\n",
                       (unsigned long)checked.error,
                       (unsigned long)checked.value);
  }
#ifdef SHIZUKU_PROFILE_ACTIVE
  BOARD::diag_printf("[COST] profile adjacent intervals include hook overhead and IRQs; per-stage minima are not additive\n");
  profile_report("svc", op_svc);
  profile_report("call1", op_call);
  profile_report("call2", op_call2);
#endif
  BOARD::diag_printf("[COST] call cost bench done\n");
}

} // namespace

void call_cost() {
  if constexpr (ARCH::HAS_CYCLE_COUNTER) {
    bench<ARCH>();
  } else {
    BOARD::diag_printf("[COST] SKIP no DWT cycle counter on this arch\n");
  }
}

} // namespace selftest
} // namespace shizuku
