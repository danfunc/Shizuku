// ===========================================================================
//  メソッド呼び出しの自己テスト梯子 (DESIGN §16 / docs 04 Phase 2 の受け入れ条件)
// ===========================================================================
//  1 段の最小プローブ → FP 活性 → N 段ネスト → 異常系 (未知番号・未 export・
//  スタック不足) の順に上る。**いきなり多段を試さない** — 参照実装は 6 段ネストを
//  一気に試して 1 段目の不具合と往復の不具合を切り分けられず長時間を失っている。
//
//  ★identity は end-to-end で突き合わせる。呼ばれた側に「誰に呼ばれたか」を
//    申告させて期待値と比べないと、§12.1 型の「黙って化ける」バグを検出できない。
//  ★使うのはオブジェクトランドの API だけ。カーネルのプリミティブはオブジェクトから
//    撃てないので、テストも撃たない = 実経路をそのまま検査することになる。
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/selftest.hpp"
#if defined(SHIZUKU_DEEP_DIAG)
#include "pico/time.h"
#endif
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
extern "C" void shizuku_selftest_progress_mark(uint32_t, uint32_t, uint32_t);
#define CL_PROG(n) shizuku_selftest_progress_mark((n), 0, 0)
#include "pico/platform.h"
namespace {
struct deep_trace_t { uint32_t magic, depth, sp, calls; };
volatile deep_trace_t __uninitialized_ram(g_deep_trace);
}
extern "C" bool shizuku_selftest_deep_read(uint32_t *out) {
  if (g_deep_trace.magic != 0x44454550u) return false;
  out[0] = g_deep_trace.depth; out[1] = g_deep_trace.sp; out[2] = g_deep_trace.calls;
  g_deep_trace.magic = 0;
  return true;
}
#define DEEP_TRACE(d) do { \
    if ((d) == 0) { g_deep_trace.calls = 0; } \
    g_deep_trace.magic = 0x44454550u; g_deep_trace.depth = (d); \
    g_deep_trace.sp = (uint32_t)__builtin_frame_address(0); \
    g_deep_trace.calls = g_deep_trace.calls + 1u; } while (0)
#else
#define CL_PROG(n) ((void)0)
#define DEEP_TRACE(d) ((void)0)
#endif

namespace shizuku {
namespace selftest {

uint32_t passed = 0;
uint32_t failed = 0;

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_LEAF = object_id::leaf;
constexpr uintptr_t OBJECT_NEST = object_id::nest;
constexpr uintptr_t OBJECT_DEEP = object_id::deep;
constexpr uintptr_t OBJECT_NAMER = object_id::namer;
// 居ないことが確かな番号 (割り当ての最後より後ろ)。
constexpr uintptr_t OBJECT_COUNT_PROBE = object_id::count;
constexpr uintptr_t METHOD_MAIN = 0;

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    record_fail(name, got, want);
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got,
                       want);
  }
}

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  // ★カーネルが直接返した答え (印つき) は自分の語彙で読まない。取り違えると
  //   「別のエラーが起きた」ように見えて原因を見失う。
  if (result.error & KERNEL_ERROR_MARK)
    return {(uintptr_t)object_error::KERNEL_REFUSED, result.error};
  return {result.error, result.value};
}

// 自分で名乗るだけのオブジェクト。
uintptr_t namer(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return api(object_api::DECLARE_NAME, (uintptr_t) "namer").error;
}

api_result call_method(uintptr_t object, uintptr_t argument) {
  return api(object_api::CALL_METHOD, object, METHOD_MAIN, argument);
}

// ---- 1 段プローブ: 引数の受け渡しと identity の突き合わせ ---------------------
uintptr_t g_leaf_self = 0;
uintptr_t g_leaf_caller = 0;

uintptr_t leaf(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  g_leaf_self = api(object_api::GET_CURRENT_OBJECT).value;
  g_leaf_caller = api(object_api::GET_CALLER_OBJECT).value;
  return argument + 1;
}

uintptr_t leaf_dirty_scratch(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
#if defined(__arm__) || defined(__thumb__)
  __asm volatile("mov ip, %0" : : "r"(0xDEADBEEFu) : "ip");
#endif
  return argument + 100;
}

// ---- N 段ネスト: 各層が自分の 1 枚だけを落とす (I-6) --------------------------
// ★ネストの各層で「自分は誰か」「誰に呼ばれたか」「今どれだけ深いか」を記録する。
//   これが無いと、層をまたいで情報が入れ替わる類のバグ (§12.1 の「黙って化ける」) を
//   検出できない。設計文書が identity の end-to-end 検証を必須にしているのはこのため。
constexpr uint32_t MAX_LEVELS = 8;
uintptr_t g_nest_self[MAX_LEVELS];
uintptr_t g_nest_caller[MAX_LEVELS];
uint32_t g_nest_depth[MAX_LEVELS];
uint32_t g_nest_levels = 0;

uintptr_t nest(uintptr_t remaining, uintptr_t, uintptr_t, uintptr_t) {
  const uint32_t level = g_nest_levels;
  if (level < MAX_LEVELS) {
    g_nest_self[level] = api(object_api::GET_CURRENT_OBJECT).value;
    g_nest_caller[level] = api(object_api::GET_CALLER_OBJECT).value;
    g_nest_depth[level] = kernel_instance.current_depth();
    g_nest_levels = level + 1;
  }
  if (remaining == 0)
    return 0;
  const api_result result = call_method(OBJECT_NEST, remaining - 1);
  if (result.error != (uintptr_t)object_error::OK)
    return 0xDEAD0000u | (uint32_t)result.error;
  return result.value + remaining;
}

// ---- 異常系: スタックを掘り切ってもエラーで返ること --------------------------
uint32_t g_deep_max = 0;
#if defined(SHIZUKU_DEEP_DIAG)
}
extern "C" volatile uint32_t shizuku_push_diag[6];
extern "C" volatile uint32_t shizuku_push_diag_n;
namespace {
#endif

uintptr_t deep(uintptr_t depth, uintptr_t, uintptr_t, uintptr_t) {
  DEEP_TRACE(depth);
  const api_result result = call_method(OBJECT_DEEP, depth + 1);
  if (result.error == (uintptr_t)object_error::NO_STACK) {
    g_deep_max = (uint32_t)depth;
    return depth;
  }
  if (result.error != (uintptr_t)object_error::OK)
    return 0xBAD00000u | (uint32_t)result.error;
  return result.value;
}

} // namespace

void call_ladder() {
  BOARD::diag_printf("[SELFTEST] call ladder start\n");

  // オブジェクトを作る。最初のメソッド (main) は生成側が与える。
  struct entry_t {
    uintptr_t object;
    uintptr_t (*main)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
  } const entries[] = {
      {OBJECT_LEAF, leaf}, {OBJECT_NEST, nest}, {OBJECT_DEEP, deep}};

  for (const entry_t &entry : entries) {
    const api_result created =
        api(object_api::CREATE_OBJECT, entry.object, (uintptr_t)entry.main);
    check("create object", created.error == (uintptr_t)object_error::OK,
          (unsigned long)created.error, 0);
  }

  // 1 段目。ここが通らないうちは上へ行かない。
  {
    const api_result result = call_method(OBJECT_LEAF, 41);
    check("call/1: error", result.error == (uintptr_t)object_error::OK,
          (unsigned long)result.error, 0);
    check("call/1: value", result.value == 42, (unsigned long)result.value, 42);
    check("call/1: callee identity", g_leaf_self == OBJECT_LEAF,
          (unsigned long)g_leaf_self, (unsigned long)OBJECT_LEAF);
    check("call/1: caller identity", g_leaf_caller == KERNEL_OBJECT::ROOT_OBJECT,
          (unsigned long)g_leaf_caller,
          (unsigned long)KERNEL_OBJECT::ROOT_OBJECT);
    check("call/1: depth restored", kernel_instance.current_depth() == 0,
          (unsigned long)kernel_instance.current_depth(), 0);
  }

  // FP 活性で同じことをやる。例外フレームが拡張形 (104B) になるので、幾何を
  // 取り違えていればここで落ちる (I-4 / I-5)。
  {
    volatile float value = 1.5f;
    value *= 2.0f;
    const api_result result = call_method(OBJECT_LEAF, 1);
    value += 0.5f;
    check("call/1 (fp): value", result.value == 2, (unsigned long)result.value,
          2);
    check("call/1 (fp): float preserved", value == 3.5f,
          (unsigned long)(value * 10.0f), 35);
  }

  // return_stub 回帰テスト: スクラッチレジスタ (r12/ip) 汚染下での通常 return
  {
    const api_result rep_res = api(object_api::CREATE_OBJECT, OBJECT_LEAF,
                                   (uintptr_t)&leaf_dirty_scratch, OBJECT_REPLACE);
    check("return_stub: replace leaf with dirty scratch",
          rep_res.error == (uintptr_t)object_error::OK,
          (unsigned long)rep_res.error, 0);

    const api_result result = call_method(OBJECT_LEAF, 41);
    check("return_stub: dirty scratch return error",
          result.error == (uintptr_t)object_error::OK,
          (unsigned long)result.error, 0);
    check("return_stub: dirty scratch return value", result.value == 141,
          (unsigned long)result.value, 141);
    check("return_stub: depth restored", kernel_instance.current_depth() == 0,
          (unsigned long)kernel_instance.current_depth(), 0);

    // leaf を元に戻す (復元成功も検査)
    const api_result rst_res =
        api(object_api::CREATE_OBJECT, OBJECT_LEAF, (uintptr_t)&leaf, OBJECT_REPLACE);
    check("return_stub: restore leaf",
          rst_res.error == (uintptr_t)object_error::OK,
          (unsigned long)rst_res.error, 0);
  }

  // N 段ネスト (6 段)。6+5+4+3+2+1 = 21。
  {
    g_nest_levels = 0;
    const api_result result = call_method(OBJECT_NEST, 6);
    check("call/6 nested: value", result.value == 21,
          (unsigned long)result.value, 21);
    check("call/6 nested: depth restored",
          kernel_instance.current_depth() == 0,
          (unsigned long)kernel_instance.current_depth(), 0);

    CL_PROG(800);
    // ★各層の identity と深さを突き合わせる。層をまたいで混ざっていないこと。
    check("nested: levels entered", g_nest_levels == 7,
          (unsigned long)g_nest_levels, 7);
    CL_PROG(801);
    uint32_t identity_bad = 0;
    uint32_t depth_bad = 0;
    for (uint32_t level = 0; level < g_nest_levels; ++level) {
      // 呼び先は毎層 OBJECT_NEST。呼び出し元は 1 層目だけ根 (ROOT_OBJECT)、以降は自分自身。
      const uintptr_t expected_caller = level == 0 ? KERNEL_OBJECT::ROOT_OBJECT : OBJECT_NEST;
      if (g_nest_self[level] != OBJECT_NEST ||
          g_nest_caller[level] != expected_caller)
        ++identity_bad;
      // 呼び出し 1 段はフレーム 2 枚 (呼び先の枠 + その中の svc を運ぶ枠)。
      if (g_nest_depth[level] != 2u * (level + 1u))
        ++depth_bad;
    }
    CL_PROG(802);
    check("nested: identity at every level", identity_bad == 0,
          (unsigned long)identity_bad, 0);
    CL_PROG(803);
    check("nested: depth grows by 2 per level", depth_bad == 0,
          (unsigned long)depth_bad, 0);
  }

    CL_PROG(804);
  // 未知の API 番号と未生成オブジェクト。黙って消えず、エラーで返ること。
  {
    CL_PROG(805);
    const auto unknown = ARCH::syscall(0xDEAD, 0, 0, 0);
    check("unknown api: rejected",
          unknown.error == (uintptr_t)object_error::UNKNOWN_API,
          (unsigned long)unknown.error,
          (unsigned long)object_error::UNKNOWN_API);
    CL_PROG(806);
    const api_result absent = call_method(OBJECT_DEEP + 10, 0);
    check("absent object: rejected",
          absent.error == (uintptr_t)object_error::BAD_OBJECT,
          (unsigned long)absent.error, (unsigned long)object_error::BAD_OBJECT);
  }

    CL_PROG(807);
  // スタックを掘り切る。panic でも無音ロックアップでもなく NO_STACK が返ること。
  {
    CL_PROG(808);
    const api_result result = call_method(OBJECT_DEEP, 0);
    check("stack exhaustion: returned NO_STACK",
          result.error == (uintptr_t)object_error::OK && g_deep_max > 0,
          (unsigned long)g_deep_max, 1);
    check("stack exhaustion: depth restored",
          kernel_instance.current_depth() == 0,
          (unsigned long)kernel_instance.current_depth(), 0);
    CL_PROG(809);
#if defined(SHIZUKU_DEEP_DIAG)
    for (int i = 0; i < 6; ++i) { busy_wait_us(200000); BOARD::diag_printf("[DD0] wait %d\n", i); }
    BOARD::diag_printf("[DD1] err=%lu max=%lu n=%lu\n",(unsigned long)result.error,(unsigned long)g_deep_max,(unsigned long)shizuku_push_diag_n);
    BOARD::diag_printf("[DD2] callee=%08lx limit=%08lx\n",(unsigned long)shizuku_push_diag[0],(unsigned long)shizuku_push_diag[1]);
    BOARD::diag_printf("[DD3] reserve=%lu hdr=%lu depth=%lu\n",(unsigned long)shizuku_push_diag[2],(unsigned long)shizuku_push_diag[3],(unsigned long)shizuku_push_diag[4]);
#endif
    BOARD::diag_printf("[SELFTEST] max nesting before NO_STACK: %lu\n",
                       (unsigned long)g_deep_max);
  }

    CL_PROG(810);
  // ---- 名乗り (System Object が起動するまでの暫定の宿) --------------------
  // ★名前は診断のためだけではない。番号の衝突が**声を出す**ようになるのが本命で、
  //   今日はそれが無かったせいで 2 回とも黙って別物が動いた。
  {
    api(object_api::CREATE_OBJECT, OBJECT_NAMER, (uintptr_t)&namer, 0);
    const api_result named = api(object_api::CALL_METHOD, OBJECT_NAMER, 0, 0);
    check("name: the object named itself",
          named.error == 0 && named.value == 0, (unsigned long)named.value, 0);
    const api_result read = api(object_api::OBJECT_NAME, OBJECT_NAMER);
    check("name: it can be read back", read.value != 0 &&
              ((const char *)read.value)[0] == 'n',
          (unsigned long)read.value, 1);
    // ★付け直しは断ること。控えた名が別物を指すようになると、名前を頼りにした側が
    //   静かに間違った相手を掴む — 番号の衝突とまったく同じ壊れ方になる。
    const api_result again = api(object_api::CALL_METHOD, OBJECT_NAMER, 0, 0);
    check("name: renaming is refused",
          again.value == (uintptr_t)object_error::ALREADY_NAMED,
          (unsigned long)again.value,
          (unsigned long)object_error::ALREADY_NAMED);
    // ★衝突が**声を出す**ことの確認。今日の 2 件はどちらもここが黙っていたせいで
    //   別物が動き続けた。上のログに
    //   「[KOBJ] object 15 is already taken by 'namer'」が出ていること。
    const api_result clash =
        api(object_api::CREATE_OBJECT, OBJECT_NAMER, (uintptr_t)&namer, 0);
    check("name: a colliding id is refused, loudly",
          clash.error == (uintptr_t)object_error::ALREADY_EXISTS,
          (unsigned long)clash.error,
          (unsigned long)object_error::ALREADY_EXISTS);
    // 居ないオブジェクトの名は無い。
    const api_result absent = api(object_api::OBJECT_NAME, OBJECT_COUNT_PROBE);
    check("name: an absent object has no name",
          absent.error == (uintptr_t)object_error::BAD_OBJECT,
          (unsigned long)absent.error,
          (unsigned long)object_error::BAD_OBJECT);
  }

  BOARD::diag_printf("[SELFTEST] call ladder done: %lu passed, %lu failed\n",
                     (unsigned long)passed, (unsigned long)failed);
}

} // namespace selftest
} // namespace shizuku
