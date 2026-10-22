// ===========================================================================
//  呼び出しコストの実測 (svc 往復 / メソッド呼び出し 1 段)
// ===========================================================================
//  発表資料が「約 600 cycle」と主張してきた数字には、**測定コードも記録も
//  リポジトリに存在しなかった** (削除済みの docs/04_work_instructions.md:263 (ce5b304) が
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
#include "shizuku/object_api.hpp"
#include "shizuku/method_entry.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"
#if !defined(__riscv)
#include "shizuku/service_profile.hpp"
#include "shizuku/kernel_services.hpp"
#endif

namespace shizuku {
namespace selftest {

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_COST_LEAF = object_id::cost_leaf;
constexpr uintptr_t OBJECT_COST_NEST = object_id::cost_nest;
constexpr uintptr_t METHOD_MAIN = 0;
// 1 回の測定の標本数 (min と mean の母数)。記述子・作成の測定も同じ数に揃える。
constexpr uint32_t ROUNDS = 2048;
static_assert(ROUNDS >= 128, "call_cost sample rounds must be >= 128");

// ★fence/isb で挟む。挟まないとコンパイラとパイプラインが読み出しを前後へ動かせるので、
//   測っている区間が実際の区間とずれる。バリア自身の費用は (1) にも同じだけ乗るので
//   引き算で消える。
template <typename A> inline uint32_t cycles_now() {
#if defined(__riscv)
  __asm__ volatile("fence" ::: "memory");
  const uint32_t cycles = A::cycle_counter_read();
  __asm__ volatile("fence" ::: "memory");
  return cycles;
#else
  __asm__ volatile("isb" ::: "memory");
  const uint32_t cycles = A::cycle_counter_read();
  __asm__ volatile("isb" ::: "memory");
  return cycles;
#endif
}

// ---- 測る相手 ---------------------------------------------------------------
struct api_result {
  uintptr_t error;
  uintptr_t value;
};

// ★always_inline: ecall の caller 側 (ARCH::syscall) は s1–s11 を clobber 宣言している。普通の関数だと
//   その関数自身が呼ばれるたびに s1–s11 を退避・復元する (psABI)。呼出し地点へ展開すれば、
//   退避は展開先の関数の入口で 1 回。
[[gnu::always_inline]] inline api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
                                             uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  if (result.error & KERNEL_ERROR_MARK)
    return {(uintptr_t)object_error::KERNEL_REFUSED, result.error};
  return {result.error, result.value};
}

[[gnu::always_inline]] inline api_result call_method(uintptr_t object, uintptr_t argument) {
  return api(object_api::CALL_METHOD, object, METHOD_MAIN, argument);
}

// 呼び先。**何もしない**。ここに仕事を入れると、測っているのが呼び出しの費用なのか
// 仕事の費用なのか分からなくなる。
SHIZUKU_ZERO_USED_REGS uintptr_t cost_leaf(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  return argument + 1;
}

// 1 段深いだけの相手。(4) との差が「1 段あたりの限界費用」になる。
SHIZUKU_ZERO_USED_REGS uintptr_t cost_nest(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
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

#if defined(__riscv)
extern "C" {
struct shizuku_c6_call_cost_record_t {
  uint32_t magic;           // 0x434F5354 ("COST")
  uint32_t version;         // 2
  uint32_t rounds;          // 2048
  uint32_t baseline_min;    // op_nothing
  uint32_t baseline_mean;
  uint32_t direct_raw_min;  // direct call
  uint32_t direct_raw_mean;
  uint32_t direct_net_min;
  uint32_t direct_net_mean;
  uint32_t svc_raw_min;     // svc round trip
  uint32_t svc_raw_mean;
  uint32_t svc_net_min;
  uint32_t svc_net_mean;
  uint32_t call1_raw_min;   // method call (1 hop)
  uint32_t call1_raw_mean;
  uint32_t call1_net_min;
  uint32_t call1_net_mean;
  uint32_t call2_raw_min;   // method call (2 hops)
  uint32_t call2_raw_mean;
  uint32_t call2_net_min;
  uint32_t call2_net_mean;
  uint32_t checked_ok;      // 1 if checked returns 42, else 0
  uint32_t desc1_raw_min;   // descriptor call (1 hop): D -> cost_dleaf
  uint32_t desc1_raw_mean;
  uint32_t desc1_net_min;
  uint32_t desc1_net_mean;
  uint32_t desc2_raw_min;   // descriptor call (2 hops): D -> cost_dnest -> cost_dinner
  uint32_t desc2_raw_mean;
  uint32_t desc2_net_min;
  uint32_t desc2_net_mean;
  uint32_t create_desc_raw_min;  // create method descriptor (API 32), table emptied per sample
  uint32_t create_desc_raw_mean;
  uint32_t create_desc_net_min;
  uint32_t create_desc_net_mean;
  uint32_t create_desc_rounds;   // samples for create_desc
  uint32_t desc_checked_ok;      // 1 if both descriptor calls return 42 and setup/create all succeeded
};
// ★レイアウトを固定する。記録を読む側 (ホスト) はこの並びを前提にしている。
static_assert(sizeof(shizuku_c6_call_cost_record_t) == 144, "call_cost record size");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, magic) == 0, "magic");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, version) == 4, "version");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, rounds) == 8, "rounds");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, baseline_min) == 12, "baseline_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, baseline_mean) == 16, "baseline_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, direct_raw_min) == 20, "direct_raw_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, direct_raw_mean) == 24, "direct_raw_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, direct_net_min) == 28, "direct_net_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, direct_net_mean) == 32, "direct_net_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, svc_raw_min) == 36, "svc_raw_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, svc_raw_mean) == 40, "svc_raw_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, svc_net_min) == 44, "svc_net_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, svc_net_mean) == 48, "svc_net_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, call1_raw_min) == 52, "call1_raw_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, call1_raw_mean) == 56, "call1_raw_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, call1_net_min) == 60, "call1_net_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, call1_net_mean) == 64, "call1_net_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, call2_raw_min) == 68, "call2_raw_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, call2_raw_mean) == 72, "call2_raw_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, call2_net_min) == 76, "call2_net_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, call2_net_mean) == 80, "call2_net_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, checked_ok) == 84, "checked_ok");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, desc1_raw_min) == 88, "desc1_raw_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, desc1_raw_mean) == 92, "desc1_raw_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, desc1_net_min) == 96, "desc1_net_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, desc1_net_mean) == 100, "desc1_net_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, desc2_raw_min) == 104, "desc2_raw_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, desc2_raw_mean) == 108, "desc2_raw_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, desc2_net_min) == 112, "desc2_net_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, desc2_net_mean) == 116, "desc2_net_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, create_desc_raw_min) == 120, "create_desc_raw_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, create_desc_raw_mean) == 124, "create_desc_raw_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, create_desc_net_min) == 128, "create_desc_net_min");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, create_desc_net_mean) == 132, "create_desc_net_mean");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, create_desc_rounds) == 136, "create_desc_rounds");
static_assert(__builtin_offsetof(shizuku_c6_call_cost_record_t, desc_checked_ok) == 140, "desc_checked_ok");
volatile shizuku_c6_call_cost_record_t shizuku_c6_call_cost_record = {};
}
#endif

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

// 引き算した結果を「サイクル」で 1 行にする。
void report(const char *name, stat_t raw, stat_t baseline,
            uint32_t cycles_per_us) {
  const uint32_t net_min = raw.min > baseline.min ? raw.min - baseline.min : 0;
#if defined(__riscv)
  // C6 / RISC-V: CPU周波数160MHz仮定のns換算をせず、cycles中心でraw/net min/meanを明示出力
  // net mean は raw.mean - baseline.mean (underflow は 0 clamp、RAM record と完全一致)
  const uint32_t net_mean = raw.mean > baseline.mean ? raw.mean - baseline.mean : 0;
  (void)cycles_per_us;
  BOARD::diag_printf(
      "[COST] %-22s net min %5lu cyc  mean %5lu cyc  [raw min %lu, mean %lu]\n",
      name, (unsigned long)net_min, (unsigned long)net_mean,
      (unsigned long)raw.min, (unsigned long)raw.mean);
#else
  // 既存 ARM 他 arch: 従来通り baseline.min を引く挙動を維持
  const uint32_t net_mean = raw.mean > baseline.min ? raw.mean - baseline.min : 0;
  // ns = cycle * 1000 / (cycles/µs)。整数のまま計算する (FP を測定路に持ち込まない)。
  const uint32_t ns = cycles_per_us ? (net_min * 1000u) / cycles_per_us : 0;
  BOARD::diag_printf(
      "[COST] %-22s min %5lu cyc (%4lu ns)  mean %5lu cyc  [raw min %lu, mean %lu]\n",
      name, (unsigned long)net_min, (unsigned long)ns, (unsigned long)net_mean,
      (unsigned long)raw.min, (unsigned long)raw.mean);
#endif
}


#if defined(__riscv)
// ---- 記述子の計測 (riscv 限定。CALL_METHOD_DESCRIPTOR は trap 内の直接処理にだけある) --
// 記述子は**呼び手 (current object) の表**から引かれ、作れるのは「自分自身・作成者・
// 直接の子」への記述子だけ。root の表は call_ladder が 16 枠を埋め切っているので使えない。
// そこで呼び手は専用の D (cost_driver) にし、D が子 cost_dleaf / cost_dnest を作って、
// その先へ記述子を張る。2 段目の cost_dnest はさらに自分の子 cost_dinner へ張る。
constexpr uintptr_t OBJECT_COST_DRIVER = object_id::cost_driver;
constexpr uintptr_t OBJECT_COST_OWNER = object_id::cost_owner;
constexpr uintptr_t OBJECT_COST_DLEAF = object_id::cost_dleaf;
constexpr uintptr_t OBJECT_COST_DNEST = object_id::cost_dnest;
constexpr uintptr_t OBJECT_COST_DINNER = object_id::cost_dinner;
// main の引数の取り決め。測定の引数 (0 / 41) と衝突しない値を使う。
constexpr uintptr_t MODE_SETUP = UINTPTR_MAX;
constexpr uintptr_t MODE_MEASURE = UINTPTR_MAX - 1;

// 下の index は呼び手の表を指す (g_dnest_index だけは cost_dnest の表)。
volatile uintptr_t g_desc1_index = 0; // D -> cost_dleaf
volatile uintptr_t g_desc2_index = 0; // D -> cost_dnest
volatile uintptr_t g_dnest_index = 0; // cost_dnest -> cost_dinner
volatile uintptr_t g_setup_error = (uintptr_t)object_error::OK;
volatile uintptr_t g_desc1_value = 0;
volatile uintptr_t g_desc2_value = 0;
stat_t g_desc1_stat{};
stat_t g_desc2_stat{};
volatile uint32_t g_create_cycles = 0;
volatile uintptr_t g_create_error = (uintptr_t)object_error::OK;

// 記述子の呼出し。a2 だけ引数を渡し、残りは 0 (計測では 0、検算では 41)。
[[gnu::always_inline]] inline api_result desc_call(uintptr_t index, uintptr_t argument) {
  const auto result = ARCH::syscall((uintptr_t)object_api::CALL_METHOD_DESCRIPTOR,
                                    index, argument, 0, 0, 0, 0, 0);
  if (result.error & KERNEL_ERROR_MARK)
    return {(uintptr_t)object_error::KERNEL_REFUSED, result.error};
  return {result.error, result.value};
}

// 設定の最初の失敗だけを残す (後の成功で上書きしない)。
void note_setup(uintptr_t error) {
  if (error != (uintptr_t)object_error::OK &&
      g_setup_error == (uintptr_t)object_error::OK)
    g_setup_error = error;
}

// 記述子 1 段 / 2 段の 1 回ぶん (呼び手は D)。
__attribute__((noinline)) uintptr_t op_desc1(uintptr_t) {
  return desc_call(g_desc1_index, 0).value;
}
__attribute__((noinline)) uintptr_t op_desc2(uintptr_t) {
  return desc_call(g_desc2_index, 0).value;
}

// 2 段目の受け手。SETUP では自分の子 cost_dinner と、それへの記述子 (自分の表) を作る。
// それ以外は記述子で cost_dinner を呼び、引数をそのまま渡す。
SHIZUKU_ZERO_USED_REGS uintptr_t cost_dnest(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  if (argument == MODE_SETUP) {
    note_setup(api(object_api::CREATE_OBJECT, OBJECT_COST_DINNER,
                   method_entry<ARCH, &cost_leaf>()).error);
    const api_result made =
        api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_COST_DINNER, 0);
    note_setup(made.error);
    g_dnest_index = made.value;
    return 0;
  }
  return desc_call(g_dnest_index, argument).value;
}

// 呼び手 (D)。SETUP では子 cost_dleaf / cost_dnest と、自分の表への記述子 2 本を作る。
// MEASURE では D の中から記述子を撃って測る (撃つのが D なので、呼び手は D になる)。
SHIZUKU_ZERO_USED_REGS uintptr_t cost_driver(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  if (argument == MODE_SETUP) {
    note_setup(api(object_api::CREATE_OBJECT, OBJECT_COST_DLEAF,
                   method_entry<ARCH, &cost_leaf>()).error);
    note_setup(api(object_api::CREATE_OBJECT, OBJECT_COST_DNEST,
                   method_entry<ARCH, &cost_dnest>()).error);
    const api_result desc1 =
        api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_COST_DLEAF, 0);
    note_setup(desc1.error);
    g_desc1_index = desc1.value;
    // 子の cost_dnest に自分の子と記述子を作らせる (その作成者は cost_dnest になる)。
    note_setup(call_method(OBJECT_COST_DNEST, MODE_SETUP).error);
    const api_result desc2 =
        api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_COST_DNEST, 0);
    note_setup(desc2.error);
    g_desc2_index = desc2.value;
    return 0;
  }
  if (argument != MODE_MEASURE)
    return 0;
  g_desc1_stat = measure<ARCH>(op_desc1, ROUNDS);
  g_desc2_stat = measure<ARCH>(op_desc2, ROUNDS);
  // 検算: 引数 41 が両方の段を通って 42 に戻ること。
  g_desc1_value = desc_call(g_desc1_index, 41).value;
  g_desc2_value = desc_call(g_desc2_index, 41).value;
  return 0;
}

// 作成の測定対象。中で**自分自身への記述子**を 1 つ作り、その API 往復の区間だけを測る
// (自分自身への作成は作成権限で常に通る)。表は呼ぶ前に root が差し替えで空にしておく。
SHIZUKU_ZERO_USED_REGS uintptr_t cost_owner(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const uint32_t started = cycles_now<ARCH>();
  g_create_error =
      api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_COST_OWNER, 0).error;
  const uint32_t ended = cycles_now<ARCH>();
  g_create_cycles = ended - started;
  return 0;
}

// 作成 1 回ぶん。差し替え (表を空にする) と呼び出しは計測の外。区間は cost_owner の中。
uint32_t create_once(bool &ok) {
  const api_result fresh = api(object_api::CREATE_OBJECT, OBJECT_COST_OWNER,
                               method_entry<ARCH, &cost_owner>(), OBJECT_REPLACE);
  const api_result ran = call_method(OBJECT_COST_OWNER, 0);
  if (fresh.error != (uintptr_t)object_error::OK ||
      ran.error != (uintptr_t)object_error::OK ||
      g_create_error != (uintptr_t)object_error::OK)
    ok = false;
  return g_create_cycles;
}

// 作成の min / mean。標本ごとに空の表で作るので、枠の埋まり具合に依らない。
stat_t measure_create(bool &ok) {
  for (uint32_t warm = 0; warm < 32; ++warm)
    create_once(ok);
  uint32_t best = 0xFFFFFFFFu;
  uint64_t total = 0;
  for (uint32_t round = 0; round < ROUNDS; ++round) {
    const uint32_t elapsed = create_once(ok);
    if (elapsed < best)
      best = elapsed;
    total += elapsed;
  }
  return {best, (uint32_t)(total / ROUNDS)};
}
#endif

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

#if defined(__riscv)
  // C6 / RISC-V: BOARD::cycles_per_us() は SYSTIMER (16MHz) 基準であり、CPU周波数 (160MHz) と乖離するため、
  // CPUサイクル周波数に見える表記を避け、単位を明確に systimer ticks/us と表示 (cycles中心方針、160MHz仮定なし)
  const uint32_t systimer_ticks_per_us = BOARD::cycles_per_us();
  BOARD::diag_printf("[COST] systimer = %lu ticks/us (cycles-based; CPU clock unassumed)\n",
                     (unsigned long)systimer_ticks_per_us);
  const uint32_t cycles_per_us = 0;
#else
  const uint32_t cycles_per_us = BOARD::cycles_per_us();
  BOARD::diag_printf("[COST] clk_sys = %lu cycles/us\n",
                     (unsigned long)cycles_per_us);
#endif

  struct entry_t {
    uintptr_t object;
    uintptr_t main; // 入口 stub (method_entry) のアドレス
  } const entries[] = {{OBJECT_COST_LEAF, method_entry<ARCH, &cost_leaf>()},
                       {OBJECT_COST_NEST, method_entry<ARCH, &cost_nest>()}};
  for (const entry_t &entry : entries) {
    const api_result created =
        api(object_api::CREATE_OBJECT, entry.object, entry.main);
    if (created.error != (uintptr_t)object_error::OK) {
      BOARD::diag_printf("[COST] FAIL create object %lu: error %lu\n",
                          (unsigned long)entry.object,
                          (unsigned long)created.error);
      ++failed;
      return;
    }
  }

  // ★ウォームアップは measure の中で行う (呼び出し経路そのものを温めるため)。

  const stat_t nothing = measure<A>(op_nothing, ROUNDS);
  const stat_t direct = measure<A>(op_direct, ROUNDS);
  const stat_t svc = measure<A>(op_svc, ROUNDS);
  const stat_t call1 = measure<A>(op_call, ROUNDS);
  const stat_t call2 = measure<A>(op_call2, ROUNDS);

#if defined(__riscv)
  // 記述子の計測 (riscv 限定)。呼び手は D。作成は cost_owner を毎回差し替えて空の表で測る。
  const api_result driver =
      api(object_api::CREATE_OBJECT, OBJECT_COST_DRIVER, method_entry<ARCH, &cost_driver>());
  const api_result owner =
      api(object_api::CREATE_OBJECT, OBJECT_COST_OWNER, method_entry<ARCH, &cost_owner>());
  if (driver.error != (uintptr_t)object_error::OK ||
      owner.error != (uintptr_t)object_error::OK) {
    BOARD::diag_printf("[COST] FAIL create descriptor bench objects: error %lu / %lu\n",
                       (unsigned long)driver.error, (unsigned long)owner.error);
    ++failed;
    return;
  }
  call_method(OBJECT_COST_DRIVER, MODE_SETUP);
  const uintptr_t setup_error = g_setup_error;
  call_method(OBJECT_COST_DRIVER, MODE_MEASURE);
  bool create_ok = true;
  const stat_t desc1 = g_desc1_stat;
  const stat_t desc2 = g_desc2_stat;
  const stat_t create = measure_create(create_ok);
#endif

  BOARD::diag_printf("[COST] harness baseline = %lu cyc (subtracted below)\n",
                     (unsigned long)nothing.min);
  report("direct call", direct, nothing, cycles_per_us);
  report("svc round trip", svc, nothing, cycles_per_us);
  report("method call (1 hop)", call1, nothing, cycles_per_us);
  report("method call (2 hops)", call2, nothing, cycles_per_us);
#if defined(__riscv)
  report("desc call (1 hop)", desc1, nothing, cycles_per_us);
  report("desc call (2 hops)", desc2, nothing, cycles_per_us);
  report("create desc", create, nothing, cycles_per_us);
#endif

  const auto net = [&](stat_t value) -> uint32_t {
    return value.min > nothing.min ? value.min - nothing.min : 0;
  };
  cost_baseline = nothing.min;
  cost_direct = net(direct);
  cost_svc = net(svc);
  cost_call1 = net(call1);
  cost_call2 = net(call2);

#if defined(__riscv)
  const auto net_min = [&](stat_t value) -> uint32_t {
    return value.min > nothing.min ? value.min - nothing.min : 0;
  };
  const auto net_mean = [&](stat_t value) -> uint32_t {
    return value.mean > nothing.mean ? value.mean - nothing.mean : 0;
  };
  shizuku_c6_call_cost_record.magic = 0x434F5354u;
  shizuku_c6_call_cost_record.version = 2;
  shizuku_c6_call_cost_record.rounds = ROUNDS;
  shizuku_c6_call_cost_record.baseline_min = nothing.min;
  shizuku_c6_call_cost_record.baseline_mean = nothing.mean;
  shizuku_c6_call_cost_record.direct_raw_min = direct.min;
  shizuku_c6_call_cost_record.direct_raw_mean = direct.mean;
  shizuku_c6_call_cost_record.direct_net_min = net_min(direct);
  shizuku_c6_call_cost_record.direct_net_mean = net_mean(direct);
  shizuku_c6_call_cost_record.svc_raw_min = svc.min;
  shizuku_c6_call_cost_record.svc_raw_mean = svc.mean;
  shizuku_c6_call_cost_record.svc_net_min = net_min(svc);
  shizuku_c6_call_cost_record.svc_net_mean = net_mean(svc);
  shizuku_c6_call_cost_record.call1_raw_min = call1.min;
  shizuku_c6_call_cost_record.call1_raw_mean = call1.mean;
  shizuku_c6_call_cost_record.call1_net_min = net_min(call1);
  shizuku_c6_call_cost_record.call1_net_mean = net_mean(call1);
  shizuku_c6_call_cost_record.call2_raw_min = call2.min;
  shizuku_c6_call_cost_record.call2_raw_mean = call2.mean;
  shizuku_c6_call_cost_record.call2_net_min = net_min(call2);
  shizuku_c6_call_cost_record.call2_net_mean = net_mean(call2);
  shizuku_c6_call_cost_record.desc1_raw_min = desc1.min;
  shizuku_c6_call_cost_record.desc1_raw_mean = desc1.mean;
  shizuku_c6_call_cost_record.desc1_net_min = net_min(desc1);
  shizuku_c6_call_cost_record.desc1_net_mean = net_mean(desc1);
  shizuku_c6_call_cost_record.desc2_raw_min = desc2.min;
  shizuku_c6_call_cost_record.desc2_raw_mean = desc2.mean;
  shizuku_c6_call_cost_record.desc2_net_min = net_min(desc2);
  shizuku_c6_call_cost_record.desc2_net_mean = net_mean(desc2);
  shizuku_c6_call_cost_record.create_desc_raw_min = create.min;
  shizuku_c6_call_cost_record.create_desc_raw_mean = create.mean;
  shizuku_c6_call_cost_record.create_desc_net_min = net_min(create);
  shizuku_c6_call_cost_record.create_desc_net_mean = net_mean(create);
  shizuku_c6_call_cost_record.create_desc_rounds = ROUNDS;
#endif

  // 1 段あたりの限界費用。1 段目には呼び出し元側の svc が 1 回だけ余分に入るので、
  // 差を取ると「純粋にもう 1 段積む費用」が出る。
  if (call2.min > call1.min)
    BOARD::diag_printf("[COST] marginal per extra hop = %lu cyc\n",
                       (unsigned long)(call2.min - call1.min));

  // 検算: 呼び出しが正しい答えを返していること。速いだけで壊れていては意味がない。
  const api_result checked = call_method(OBJECT_COST_LEAF, 41);
  if (checked.error == (uintptr_t)object_error::OK && checked.value == 42) {
    ++passed;
#if defined(__riscv)
    shizuku_c6_call_cost_record.checked_ok = 1;
#endif
    BOARD::diag_printf("[COST] PASS method call still returns 42\n");
  } else {
    ++failed;
    record_fail("COST method call still returns 42", checked.value, 42);
#if defined(__riscv)
    shizuku_c6_call_cost_record.checked_ok = 0;
#endif
    BOARD::diag_printf("[COST] FAIL method call: error %lu value %lu\n",
                       (unsigned long)checked.error,
                       (unsigned long)checked.value);
  }
#if defined(__riscv)
  // 記述子の戻り値検証 (checked_ok と同じ流儀)。41 を渡して 42 が返ること、
  // 設定と作成がすべて成功したこと。
  const bool desc_ok = create_ok && setup_error == (uintptr_t)object_error::OK &&
                       g_desc1_value == 42 && g_desc2_value == 42;
  if (desc_ok) {
    ++passed;
    BOARD::diag_printf("[COST] PASS descriptor calls return 42, create ok\n");
  } else {
    ++failed;
    record_fail("COST descriptor call returns 42", g_desc1_value, 42);
    BOARD::diag_printf("[COST] FAIL descriptor: setup %lu desc1 %lu desc2 %lu create_ok %u\n",
                       (unsigned long)setup_error, (unsigned long)g_desc1_value,
                       (unsigned long)g_desc2_value, create_ok ? 1u : 0u);
  }
  shizuku_c6_call_cost_record.desc_checked_ok = desc_ok ? 1u : 0u;
#endif
  BOARD::diag_printf("[COST] call cost bench done\n");
}

} // namespace

void call_cost() {
#if !defined(__riscv)
  service_profile::reset();
#if SHIZUKU_KERNEL_SERVICE_PROFILE
  BOARD::diag_printf("[V3GW] COST includes compiled hooks; use normal build for untraced comparison\n");
#endif
#endif
  if constexpr (ARCH::HAS_CYCLE_COUNTER) {
    bench<ARCH>();
  } else {
    BOARD::diag_printf("[COST] SKIP no DWT cycle counter on this arch\n");
  }
#if !defined(__riscv) && SHIZUKU_KERNEL_SERVICE_PROFILE
  // M0+ has no DWT, but still sample the same syscall path using the us timer.
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
