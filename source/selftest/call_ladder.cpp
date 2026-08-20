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
#include "shizuku/object_api.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {

uint32_t passed = 0;
uint32_t failed = 0;

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_LEAF = 1;
constexpr uintptr_t OBJECT_NEST = 2;
constexpr uintptr_t OBJECT_DEEP = 3;
constexpr uintptr_t METHOD_MAIN = 0;

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
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

uintptr_t deep(uintptr_t depth, uintptr_t, uintptr_t, uintptr_t) {
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
    check("call/1: caller identity", g_leaf_caller == 0,
          (unsigned long)g_leaf_caller, 0);
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

  // N 段ネスト (6 段)。6+5+4+3+2+1 = 21。
  {
    g_nest_levels = 0;
    const api_result result = call_method(OBJECT_NEST, 6);
    check("call/6 nested: value", result.value == 21,
          (unsigned long)result.value, 21);
    check("call/6 nested: depth restored",
          kernel_instance.current_depth() == 0,
          (unsigned long)kernel_instance.current_depth(), 0);

    // ★各層の identity と深さを突き合わせる。層をまたいで混ざっていないこと。
    check("nested: levels entered", g_nest_levels == 7,
          (unsigned long)g_nest_levels, 7);
    uint32_t identity_bad = 0;
    uint32_t depth_bad = 0;
    for (uint32_t level = 0; level < g_nest_levels; ++level) {
      // 呼び先は毎層 OBJECT_NEST。呼び出し元は 1 層目だけ根 (0)、以降は自分自身。
      const uintptr_t expected_caller = level == 0 ? 0 : OBJECT_NEST;
      if (g_nest_self[level] != OBJECT_NEST ||
          g_nest_caller[level] != expected_caller)
        ++identity_bad;
      // 呼び出し 1 段はフレーム 2 枚 (呼び先の枠 + その中の svc を運ぶ枠)。
      if (g_nest_depth[level] != 2u * (level + 1u))
        ++depth_bad;
    }
    check("nested: identity at every level", identity_bad == 0,
          (unsigned long)identity_bad, 0);
    check("nested: depth grows by 2 per level", depth_bad == 0,
          (unsigned long)depth_bad, 0);
  }

  // 未知の API 番号と未生成オブジェクト。黙って消えず、エラーで返ること。
  {
    const auto unknown = ARCH::syscall(0xDEAD, 0, 0, 0);
    check("unknown api: rejected",
          unknown.error == (uintptr_t)object_error::UNKNOWN_API,
          (unsigned long)unknown.error,
          (unsigned long)object_error::UNKNOWN_API);
    const api_result absent = call_method(OBJECT_DEEP + 10, 0);
    check("absent object: rejected",
          absent.error == (uintptr_t)object_error::BAD_OBJECT,
          (unsigned long)absent.error, (unsigned long)object_error::BAD_OBJECT);
  }

  // スタックを掘り切る。panic でも無音ロックアップでもなく NO_STACK が返ること。
  {
    const api_result result = call_method(OBJECT_DEEP, 0);
    check("stack exhaustion: returned NO_STACK",
          result.error == (uintptr_t)object_error::OK && g_deep_max > 0,
          (unsigned long)g_deep_max, 1);
    check("stack exhaustion: depth restored",
          kernel_instance.current_depth() == 0,
          (unsigned long)kernel_instance.current_depth(), 0);
    BOARD::diag_printf("[SELFTEST] max nesting before NO_STACK: %lu\n",
                       (unsigned long)g_deep_max);
  }

  BOARD::diag_printf("[SELFTEST] call ladder done: %lu passed, %lu failed\n",
                     (unsigned long)passed, (unsigned long)failed);
}

} // namespace selftest
} // namespace shizuku
