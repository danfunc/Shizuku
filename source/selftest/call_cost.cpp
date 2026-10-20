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
//  ★サイクルカウンタ (ARM では DWT CYCCNT、RISC-V では PCCR / cycle) を使う。**数えているのはコアのクロック**なので、クロックを変えても
//    サイクル数は変わらない (µs へ直すときだけ board にクロックを訊く)。
//  ★★min を採る。系は動いたまま (SysTick / USB / 2 コア目) 測っているので、
//    平均には割り込みが混ざる。**割り込みの入らなかった 1 回**が知りたい値なので
//    最小値を代表にし、平均も併記して「どれだけ揺れたか」が見えるようにする。
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/service_profile.hpp"
#include "shizuku/kernel_services.hpp"
#if defined(SHIZUKU_C6_STAGE05_TRACE) && SHIZUKU_C6_STAGE05_TRACE
#include "chips/esp32_c6/stage05_trace.h"
#include "chips/esp32_c6/systimer.hpp"
#include "chips/esp32_c6/trap_api.hpp"
#endif

namespace shizuku {
namespace selftest {

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

inline constexpr const uintptr_t &OBJECT_COST_LEAF = object_id::cost_leaf;
inline constexpr const uintptr_t &OBJECT_COST_NEST = object_id::cost_nest;
constexpr uintptr_t METHOD_MAIN = 0;

// ★同期バリア (ARM では isb, RISC-V では fence) で挟む。挟まないとコンパイラとパイプラインが読み出しを前後へ動かせるので、
//   測っている区間が実際の区間とずれる。バリア自身の費用は (1) にも同じだけ乗るので
//   引き算で消える。
template <typename A> inline uint32_t cycles_now() {
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

// ---- 測る相手 ---------------------------------------------------------------
struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
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
static_assert(std::is_same_v<decltype(stat_t::min), uint32_t> &&
              std::is_same_v<decltype(stat_t::mean), uint32_t>,
              "stat_t must have uint32_t min and mean for raw/net reporting");

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

// 引き算した結果を「サイクル / ナノ秒」で 1 行にする (raw/net の min/mean を併記)。
void report(const char *name, stat_t raw, stat_t baseline,
            uint32_t cycles_per_us) {
  const uint32_t net = raw.min > baseline.min ? raw.min - baseline.min : 0;
  const uint32_t net_mean =
      raw.mean > baseline.mean ? raw.mean - baseline.mean : 0;
  // ns = cycle * 1000 / (cycles/µs)。整数のまま計算する (FP を測定路に持ち込まない)。
  const uint32_t ns = cycles_per_us ? (net * 1000u) / cycles_per_us : 0;
  BOARD::diag_printf(
      "[COST] %-22s net min %5lu cyc (%4lu ns)  net mean %5lu cyc  [raw min %lu  raw mean %lu]\n",
      name, (unsigned long)net, (unsigned long)ns, (unsigned long)net_mean,
      (unsigned long)raw.min, (unsigned long)raw.mean);
}

#if defined(SHIZUKU_C6_STAGE05_TRACE) && SHIZUKU_C6_STAGE05_TRACE
static void verify_c6_stage05_clock() {
  // SYSTIMER unit 0 runs at 16 MHz from XTAL
  // Wait for 16,000 SYSTIMER ticks (= 1,000 us = 1 ms) to independently compute HP CPU core frequency
  const uint64_t s_start = shizuku::chips::esp32_c6::systimer_now();
  uint32_t c_start = 0;
#if defined(__riscv)
  asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c_start) :: "memory");
#endif

  while ((shizuku::chips::esp32_c6::systimer_now() - s_start) < 16000ULL) {
    asm volatile("" ::: "memory");
  }

  const uint64_t s_end = shizuku::chips::esp32_c6::systimer_now();
  uint32_t c_end = 0;
#if defined(__riscv)
  asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c_end) :: "memory");
#endif

  const uint32_t s_delta = (uint32_t)(s_end - s_start);
  const uint32_t c_delta = c_end - c_start;

  shizuku_c6_stage05_record.systimer_ticks = s_delta;
  shizuku_c6_stage05_record.cpu_cycle_delta = c_delta;

  if (s_delta > 0 && c_delta > 0) {
    // MHz = (c_delta * 16) / s_delta
    const uint32_t mhz = (uint32_t)(((uint64_t)c_delta * 16ULL + (s_delta / 2)) / s_delta);
    shizuku_c6_stage05_record.cpu_mhz = mhz;
    // ESP32-C6 CPU clock is typically 80, 120, or 160 MHz
    if (mhz >= 40 && mhz <= 240) {
      shizuku_c6_stage05_record.clock_verified = 1u;
    } else {
      shizuku_c6_stage05_record.clock_verified = 0u;
    }
  } else {
    shizuku_c6_stage05_record.cpu_mhz = 0;
    shizuku_c6_stage05_record.clock_verified = 0u;
  }
}
#endif


// ★ARCH::HAS_CYCLE_COUNTER が false の arch ではこのテンプレートは実体化されない。
//   カウンタの番地も有効化手順も arch 側にあり、ここにはハードウェア固有レジスタを知るコードが無い。
template <typename A> void bench() {
  BOARD::diag_printf("[COST] call cost bench start\n");
  if (!A::cycle_counter_enable()) {
    // ★測れないなら測れないと言う。0 を報告して数字があるように見せない。
    BOARD::diag_printf("[COST] FAIL cycle counter is not counting - no numbers\n");
    record_fail("COST cycle counter is not counting", 0, 1);
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
  static_assert(ROUNDS >= 128, "call_cost sample rounds must be >= 128");
  const stat_t nothing = measure<A>(op_nothing, ROUNDS);
  const stat_t direct = measure<A>(op_direct, ROUNDS);
  const stat_t svc = measure<A>(op_svc, ROUNDS);
  const stat_t call1 = measure<A>(op_call, ROUNDS);
  const stat_t call2 = measure<A>(op_call2, ROUNDS);

  BOARD::diag_printf("[COST] harness baseline = %lu cyc (subtracted below)\n",
                     (unsigned long)nothing.min);
  report("direct call", direct, nothing, cycles_per_us);
  report("svc round trip", svc, nothing, cycles_per_us);
  report("method call (1 hop)", call1, nothing, cycles_per_us);
  report("method call (2 hops)", call2, nothing, cycles_per_us);

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

#if defined(SHIZUKU_C6_STAGE05_TRACE) && SHIZUKU_C6_STAGE05_TRACE
  BOARD::diag_printf("[STAGE0.5] Starting stage 0.5 interval decomposition\n");
  shizuku_c6_stage05_init(
#if defined(SHIZUKU_C6_STAGE05_IRAM) && SHIZUKU_C6_STAGE05_IRAM
      1u
#else
      0u
#endif
  );
  verify_c6_stage05_clock();
  shizuku_c6_stage05_calibrate_hooks();

  // 校正中間値の診断出力
  BOARD::diag_printf("[STAGE0.5] Hook Calibration Details:\n");
  BOARD::diag_printf("[STAGE0.5]   csrr overhead      : min %lu cyc, mean %lu cyc\n",
                     (unsigned long)shizuku_c6_stage05_calib_diag.csrr_min,
                     (unsigned long)shizuku_c6_stage05_calib_diag.csrr_mean);
  BOARD::diag_printf("[STAGE0.5]   hook_trap_entry    : net min %lu cyc (mean %lu cyc) [raw min %lu, mean %lu]\n",
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_te_net_min,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_te_net_mean,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_te_raw_min,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_te_raw_mean);
  BOARD::diag_printf("[STAGE0.5]   hook_svc_dispatch  : net min %lu cyc (mean %lu cyc) [raw min %lu, mean %lu]\n",
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_sd_net_min,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_sd_net_mean,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_sd_raw_min,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_sd_raw_mean);
  BOARD::diag_printf("[STAGE0.5]   hook_handler       : net min %lu cyc (mean %lu cyc) [raw min %lu, mean %lu]\n",
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_hd_net_min,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_hd_net_mean,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_hd_raw_min,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_hd_raw_mean);
  BOARD::diag_printf("[STAGE0.5]   hook_return_prep   : net min %lu cyc (mean %lu cyc) [raw min %lu, mean %lu]\n",
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_rp_net_min,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_rp_net_mean,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_rp_raw_min,
                     (unsigned long)shizuku_c6_stage05_calib_diag.hook_rp_raw_mean);

  // ウォームアップ
  for (uint32_t warm = 0; warm < 16; ++warm) {
    g_sink = op_svc(g_argument);
  }

  shizuku_c6_stage05_active = 1u;
  constexpr uint32_t S05_ROUNDS = SHIZUKU_C6_STAGE05_ROUNDS;
  for (uint32_t r = 0; r < S05_ROUNDS; ++r) {
    shizuku_c6_stage05_current_sample.t0_caller_pre = cycles_now<A>();
    g_sink = op_svc(g_argument);
    shizuku_c6_stage05_current_sample.t7_caller_post = cycles_now<A>();
    shizuku_c6_stage05_current_sample.t6_ret_restore_begin = shizuku_c6_stage05_t6;
    shizuku_c6_stage05_record_sample((const shizuku_c6_stage05_sample_t *)&shizuku_c6_stage05_current_sample);
  }
  shizuku_c6_stage05_active = 0u;

  shizuku_c6_stage05_finish();

  static const char *const kNamesFrom[SHIZUKU_C6_STAGE05_INTERVAL_COUNT] = {
      "caller_pre", "t1_trap_entry", "t2_c_dispatch", "t3_svc_dispatch",
      "t5_return_prep", "t6_restore_begin", "t4_handler", "t1_ret_trap_entry",
      "t2_ret_c_dispatch", "t3_ret_svc_dispatch", "t5_ret_return_prep", "t6_ret_restore_begin"
  };
  static const char *const kNamesTo[SHIZUKU_C6_STAGE05_INTERVAL_COUNT] = {
      "t1_trap_entry", "t2_c_dispatch", "t3_svc_dispatch", "t5_return_prep",
      "t6_restore_begin", "t4_handler", "t1_ret_trap_entry", "t2_ret_c_dispatch",
      "t3_ret_svc_dispatch", "t5_ret_return_prep", "t6_ret_restore_begin", "caller_post"
  };

  BOARD::diag_printf("[STAGE0.5] === C6 Stage 0.5 Interval Breakdown ===\n");
  BOARD::diag_printf("[STAGE0.5] Status: %s (code=%lu)\n",
                     shizuku_c6_stage05_record.status == SHIZUKU_C6_STAGE05_STATUS_PASSED ? "PASSED" : "FAILED",
                     (unsigned long)shizuku_c6_stage05_record.error_code);
  BOARD::diag_printf("[STAGE0.5] Placement: %s\n",
                     shizuku_c6_stage05_record.placement_mode == 1 ? "IRAM" : "FLASH");
  BOARD::diag_printf("[STAGE0.5] HP CPU Clock: %lu MHz (%s, delta_cyc=%lu, delta_systimer=%lu)\n",
                     (unsigned long)shizuku_c6_stage05_record.cpu_mhz,
                     shizuku_c6_stage05_record.clock_verified ? "VERIFIED" : "UNKNOWN",
                     (unsigned long)shizuku_c6_stage05_record.cpu_cycle_delta,
                     (unsigned long)shizuku_c6_stage05_record.systimer_ticks);
  BOARD::diag_printf("[STAGE0.5] Hook Calibration: single raw min %lu cyc (mean %lu cyc), total perturbation %lu cyc\n",
                     (unsigned long)shizuku_c6_stage05_record.hook_single_raw_min,
                     (unsigned long)shizuku_c6_stage05_record.hook_single_raw_mean,
                     (unsigned long)shizuku_c6_stage05_record.total_perturbation);
  BOARD::diag_printf("[STAGE0.5] Valid Samples: %lu / %lu\n",
                     (unsigned long)shizuku_c6_stage05_record.rounds_completed,
                     (unsigned long)shizuku_c6_stage05_record.rounds_requested);
  BOARD::diag_printf("[STAGE0.5] Total Roundtrip: raw min %lu cyc (mean %lu cyc)  net min %lu cyc (mean %lu cyc)\n",
                     (unsigned long)shizuku_c6_stage05_record.total_raw_min,
                     (unsigned long)shizuku_c6_stage05_record.total_raw_mean,
                     (unsigned long)shizuku_c6_stage05_record.total_net_min,
                     (unsigned long)shizuku_c6_stage05_record.total_net_mean);
  for (uint32_t i = 0; i < SHIZUKU_C6_STAGE05_INTERVAL_COUNT; ++i) {
    BOARD::diag_printf("[STAGE0.5]   [%2lu] %-17s -> %-17s : raw min %5lu cyc (mean %5lu)  net min %5lu cyc (mean %5lu)\n",
                       (unsigned long)i,
                       kNamesFrom[i],
                       kNamesTo[i],
                       (unsigned long)shizuku_c6_stage05_record.intervals[i].raw_min,
                       (unsigned long)shizuku_c6_stage05_record.intervals[i].raw_mean,
                       (unsigned long)shizuku_c6_stage05_record.intervals[i].net_min,
                       (unsigned long)shizuku_c6_stage05_record.intervals[i].net_mean);
  }
  BOARD::diag_printf("[STAGE0.5] === End Stage 0.5 Interval Breakdown ===\n");

  if (shizuku_c6_stage05_record.status == SHIZUKU_C6_STAGE05_STATUS_PASSED) {
    ++passed;
    BOARD::diag_printf("[STAGE0.5] PASS stage 0.5 interval breakdown complete and valid\n");
  } else {
    ++failed;
    BOARD::diag_printf("[STAGE0.5] FAIL stage 0.5 interval breakdown failed (code %lu)\n",
                       (unsigned long)shizuku_c6_stage05_record.error_code);
  }
#endif
  BOARD::diag_printf("[COST] call cost bench done\n");
}

} // namespace

void call_cost() {
#if SHIZUKU_KERNEL_SERVICE_PROFILE && !defined(__riscv)
  service_profile::reset();
  BOARD::diag_printf("[V3GW] COST includes compiled hooks; use normal build for untraced comparison\n");
#endif
  if constexpr (ARCH::HAS_CYCLE_COUNTER) {
    bench<ARCH>();
  } else {
    BOARD::diag_printf("[COST] SKIP no cycle counter on this arch\n");
  }
#if SHIZUKU_KERNEL_SERVICE_PROFILE && !defined(__riscv)
  // HAS_CYCLE_COUNTER が false の arch (M0+ 等) でも、us timer を使って同じ syscall 経路をサンプリングする。
  if constexpr (!ARCH::HAS_CYCLE_COUNTER)
    for(unsigned i=0;i<128;++i) ARCH::syscall((uintptr_t)object_api::GET_CURRENT_OBJECT);
  const auto v=service_profile::read();
  BOARD::diag_printf("[V3GW] SVC software max=%lu samples=%lu unit=%s clock_ok=%u\n",
      (unsigned long)v.handler_max,(unsigned long)v.handler_samples,
      service_profile::cycles()?"cyc":"us",service_profile::available()?1u:0u);
  const auto irq=shizuku::kernel_services::irq_save();
  shizuku::kernel_services::irq_restore(irq);
  const auto m=service_profile::read();
  BOARD::diag_printf("[V3GW] PRIMASK envelope max=%lu samples=%lu unit=%s (hooks included)\n",
      (unsigned long)m.mask_max,(unsigned long)m.mask_samples,
      service_profile::cycles()?"cyc":"us");
#endif
}

} // namespace selftest
} // namespace shizuku
