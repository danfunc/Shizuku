// ===========================================================================
//  CALL / RETURN の自己テスト梯子 (DESIGN §16 / docs 04 Phase 2 の受け入れ条件)
// ===========================================================================
//  1 段の最小プローブ → FP 活性 → N 段ネスト → 異常系 (段数の申告ミス、スタック
//  不足) の順に上る。**いきなり多段を試さない** — 参照実装は 6 段ネストを一気に
//  試して 1 段目の不具合と往復の不具合を切り分けられず長時間を失っている。
//
//  ★identity は end-to-end で突き合わせる。呼ばれた側に「誰に呼ばれたか」を
//    申告させて期待値と比べないと、§12.1 型の「黙って化ける」バグを検出できない。
#include "shizuku/kernel.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t COOKIE_LEAF = 0x1EAF0001;
constexpr uintptr_t COOKIE_NEST = 0x0E570002;
constexpr uintptr_t COOKIE_BOGUS = 0xB0605003;
constexpr uintptr_t COOKIE_DEEP = 0xDEE04004;

uint32_t g_pass = 0;
uint32_t g_fail = 0;

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++g_pass;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++g_fail;
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got,
                       want);
  }
}

// 呼び先の共通形。カーネルが lr に戻り口を載せるので、普通の C 関数として書ける。
using method_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

struct call_result {
  uintptr_t error;
  uintptr_t value;
};

call_result call(method_t entry, uintptr_t callee_cookie, uintptr_t a0,
                 uintptr_t a1 = 0, uintptr_t a2 = 0, uintptr_t a3 = 0) {
  call_request request{};
  request.entry_pc = (uintptr_t)entry;
  request.callee_cookie = callee_cookie;
  // ★既定は CALL_STRICT — 呼び出し元 identity をそのまま渡す (中継者に化けさせない)。
  request.caller_cookie = kernel_instance.current_cookie();
  request.protection = PROTECTION_TRUSTED; // このテストは信頼側で回す
  request.args[0] = a0;
  request.args[1] = a1;
  request.args[2] = a2;
  request.args[3] = a3;
  const auto result =
      ARCH::syscall((uintptr_t)primitive::CALL, (uintptr_t)&request);
  return {result.error, result.value};
}

// ---- 1 段プローブ: 引数 4 本の受け渡しと identity の突き合わせ ----------------
uintptr_t g_leaf_caller = 0;
uintptr_t g_leaf_cookie = 0;
uint32_t g_leaf_depth = 0;

uintptr_t leaf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
  g_leaf_caller = kernel_instance.current_caller_cookie();
  g_leaf_cookie = kernel_instance.current_cookie();
  g_leaf_depth = kernel_instance.current_depth();
  return a0 + a1 + a2 + a3;
}

// ---- N 段ネスト: 各層が自分の 1 枚だけを落とす (I-6) --------------------------
uintptr_t nest(uintptr_t remaining, uintptr_t sum, uintptr_t, uintptr_t) {
  if (remaining == 0)
    return sum;
  const call_result result =
      call(nest, COOKIE_NEST, remaining - 1, sum + remaining);
  if (result.error != (uintptr_t)kernel_error::OK)
    return 0xDEAD0000u | (uint32_t)result.error;
  return result.value;
}

// ---- 異常系: 段数の申告ミスで系が死なないこと (I-9) --------------------------
uintptr_t g_bogus_error = 0;
uintptr_t g_bogus_depth = 0;

uintptr_t bogus_return(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // わざと嘘のネスト数を申告する。カーネルは 1 枚も落とさずエラーで返すはず。
  const auto result = ARCH::syscall((uintptr_t)primitive::RETURN, 1, 0xBADBAD,
                                    0, 0x9999);
  g_bogus_error = result.error;
  g_bogus_depth = result.value;
  return 0x600D; // 普通に戻れた = 巻き戻されていないし系も生きている
}

// ---- 異常系: スタックを掘り切ってもエラーで返ること --------------------------
uint32_t g_deep_max = 0;

uintptr_t deep(uintptr_t depth, uintptr_t, uintptr_t, uintptr_t) {
  const call_result result = call(deep, COOKIE_DEEP, depth + 1);
  if (result.error == (uintptr_t)kernel_error::NO_STACK) {
    g_deep_max = (uint32_t)depth;
    return depth;
  }
  if (result.error != (uintptr_t)kernel_error::OK)
    return 0xBAD00000u | (uint32_t)result.error;
  return result.value;
}

} // namespace

void call_ladder() {
  BOARD::diag_printf("[SELFTEST] call ladder start (boot cookie=%08lx)\n",
                     (unsigned long)kernel_instance.current_cookie());

  // 1 段目。ここが通らないうちは上へ行かない。
  {
    const call_result result = call(leaf, COOKIE_LEAF, 1, 2, 3, 4);
    check("call/1: error", result.error == (uintptr_t)kernel_error::OK,
          (unsigned long)result.error, 0);
    check("call/1: value", result.value == 10, (unsigned long)result.value, 10);
    check("call/1: caller identity", g_leaf_caller == BOOT_COOKIE,
          (unsigned long)g_leaf_caller, (unsigned long)BOOT_COOKIE);
    check("call/1: callee cookie", g_leaf_cookie == COOKIE_LEAF,
          (unsigned long)g_leaf_cookie, (unsigned long)COOKIE_LEAF);
    check("call/1: depth", g_leaf_depth == 1, (unsigned long)g_leaf_depth, 1);
    check("call/1: cookie restored",
          kernel_instance.current_cookie() == BOOT_COOKIE,
          (unsigned long)kernel_instance.current_cookie(),
          (unsigned long)BOOT_COOKIE);
    check("call/1: depth restored", kernel_instance.current_depth() == 0,
          (unsigned long)kernel_instance.current_depth(), 0);
  }

  // FP 活性で同じことをやる。例外フレームが拡張形 (104B) になるので、幾何を
  // 取り違えていればここで落ちる (I-4 / I-5)。
  {
    volatile float value = 1.5f;
    value *= 2.0f; // FP 文脈を起こしてから呼ぶ
    const call_result result = call(leaf, COOKIE_LEAF, 5, 6, 7, 8);
    value += 0.5f; // 呼び出しを跨いで FP 文脈が壊れていないこと
    check("call/1 (fp): value", result.value == 26, (unsigned long)result.value,
          26);
    check("call/1 (fp): float preserved", value == 3.5f,
          (unsigned long)(value * 10.0f), 35);
  }

  // N 段ネスト (6 段)。1+2+3+4+5+6 = 21。
  {
    const call_result result = call(nest, COOKIE_NEST, 6, 0);
    check("call/6 nested: value", result.value == 21, (unsigned long)result.value,
          21);
    check("call/6 nested: depth restored",
          kernel_instance.current_depth() == 0,
          (unsigned long)kernel_instance.current_depth(), 0);
  }

  // 段数の申告ミス。エラーが返り、巻き戻されず、系が生きていること。
  {
    const call_result result = call(bogus_return, COOKIE_BOGUS, 0);
    check("bad depth claim: rejected",
          g_bogus_error == (uintptr_t)kernel_error::DEPTH_MISMATCH,
          (unsigned long)g_bogus_error,
          (unsigned long)kernel_error::DEPTH_MISMATCH);
    check("bad depth claim: actual depth reported", g_bogus_depth == 1,
          (unsigned long)g_bogus_depth, 1);
    check("bad depth claim: survived", result.value == 0x600D,
          (unsigned long)result.value, 0x600D);
  }

  // スタックを掘り切る。panic でも無音ロックアップでもなく NO_STACK が返ること。
  {
    const call_result result = call(deep, COOKIE_DEEP, 0);
    check("stack exhaustion: returned NO_STACK",
          result.error == (uintptr_t)kernel_error::OK && g_deep_max > 0,
          (unsigned long)g_deep_max, 1);
    check("stack exhaustion: depth restored",
          kernel_instance.current_depth() == 0,
          (unsigned long)kernel_instance.current_depth(), 0);
    BOARD::diag_printf("[SELFTEST] max nesting before NO_STACK: %lu\n",
                       (unsigned long)g_deep_max);
  }

  BOARD::diag_printf("[SELFTEST] call ladder done: %lu passed, %lu failed\n",
                     (unsigned long)g_pass, (unsigned long)g_fail);
}

} // namespace selftest
} // namespace shizuku
