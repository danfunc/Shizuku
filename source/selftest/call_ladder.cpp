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
#if defined(SHIZUKU_DEEP_DIAG) && defined(SHIZUKU_RP2040)
#include "pico/time.h"
#endif
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
extern "C" void shizuku_selftest_progress_mark(uint32_t, uint32_t, uint32_t);
#define CL_PROG(n) shizuku_selftest_progress_mark((n), 0, 0)
#include "pico.h"
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
// 記述子の試験用。番号は objects.list の名前から振られる (OBJECT_COUNT の外の番号は使えない)。
constexpr uintptr_t OBJECT_DESC_TEST = object_id::desc_test;
constexpr uintptr_t OBJECT_DESC_OWNER = object_id::desc_owner;
constexpr uintptr_t OBJECT_DESC_RELAY = object_id::desc_relay;
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

// 記述子での呼出し。riscv では trap 内の直接処理に届く (a2..a7 = 1..6 を渡して、
// 呼ばれた側に引数が全部届くことも検査する)。それ以外では UNKNOWN_API が返る。
api_result call_descriptor(uintptr_t index) {
#if defined(__riscv)
  const auto result = ARCH::syscall((uintptr_t)object_api::CALL_METHOD_DESCRIPTOR,
                                    index, 1, 2, 3, 4, 5, 6);
  if (result.error & KERNEL_ERROR_MARK)
    return {(uintptr_t)object_error::KERNEL_REFUSED, result.error};
  return {result.error, result.value};
#else
  return api(object_api::CALL_METHOD_DESCRIPTOR, index);
#endif
}

// ---- 1 段プローブ: 引数の受け渡しと identity の突き合わせ ---------------------
uintptr_t g_leaf_self = 0;
uintptr_t g_leaf_caller = 0;

uintptr_t leaf(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  g_leaf_self = api(object_api::GET_CURRENT_OBJECT).value;
  g_leaf_caller = api(object_api::GET_CALLER_OBJECT).value;
  return argument + 1;
}

// ---- 方法記述子: 直接 trap 呼出しの検査対象 -----------------------------------
uint32_t g_desc_calls = 0;
uintptr_t g_desc_self = 0;
uintptr_t g_desc_caller = 0;

uintptr_t desc_target_fn(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
  ++g_desc_calls;
  g_desc_self = api(object_api::GET_CURRENT_OBJECT).value;
  g_desc_caller = api(object_api::GET_CALLER_OBJECT).value;
  return a + b + c + d + e + f;
}

// 記述子の対象オブジェクトの main。走った先で自分の表に desc_target_fn を export する。
// ★EXPORT_METHOD は current_object (呼び出し中のオブジェクト) の表に入る。ルートから
//   撃つとルート自身の表に入るので、対象オブジェクトの中から撃たせる。
uintptr_t desc_export_fn(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return api(object_api::EXPORT_METHOD, 1, (uintptr_t)&desc_target_fn).value;
}

// 別オブジェクトの main。自分の表に、対象の 1 番を指す記述子を作って番号を返す。
uintptr_t desc_make_index(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const api_result made = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
  return made.error == (uintptr_t)object_error::OK ? made.value
                                                   : 0xDEAD0000u | (uint32_t)made.error;
}

// 中継オブジェクト B の main。ルートから呼ばれ、自分の表に C (desc_test) の 1 番への記述子を作って
// 直接 trap で C を撃つ。B 自身の identity と、呼んだ側 (ルートのはず) を記録する。
// ★記録は riscv 以外でも行う (記述子の作成と直接呼出しだけが riscv 限定)。
uintptr_t g_relay_self = 0;
uintptr_t g_relay_caller = 0;
uint32_t g_relay_depth_before = 0;
uint32_t g_relay_depth_after = 0;
uintptr_t g_relay_call_value = 0;
uintptr_t g_relay_call_error = (uintptr_t)object_error::UNKNOWN_API;

uintptr_t desc_relay_fn(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  g_relay_self = api(object_api::GET_CURRENT_OBJECT).value;
  g_relay_caller = api(object_api::GET_CALLER_OBJECT).value;
  g_relay_depth_before = kernel_instance.current_depth();
  // ★呼出し前の値は「呼ばれていない」を表すように置いておく。riscv 以外ではこれが残る。
  g_relay_call_value = 0;
  g_relay_call_error = (uintptr_t)object_error::UNKNOWN_API;
#if defined(__riscv)
  // 記述子は B 自身の表に作られる (呼び出し元の表からだけ撃てる、の確認も兼ねる)。
  const api_result made = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
  if (made.error != (uintptr_t)object_error::OK) {
    g_relay_depth_after = kernel_instance.current_depth();
    return 0xDEAD0000u | (uint32_t)made.error;
  }
  const api_result called = call_descriptor(made.value);
  g_relay_call_value = called.value;
  g_relay_call_error = called.error;
#endif
  g_relay_depth_after = kernel_instance.current_depth();
  return 0;
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
extern "C" volatile uint32_t shizuku_push_diag_kind;
extern "C" volatile uint32_t shizuku_push_diag_reject_kind;
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
    // [DD0] の 200ms 待ちは出力を流し切るための間と推定 (コードに明記なし)。
    // C6 の diag_printf は診断リングに積むだけなので、待ちは RP2040 のみ行う。
    for (int i = 0; i < 6; ++i) {
#if defined(SHIZUKU_RP2040)
      busy_wait_us(200000);
#endif
      BOARD::diag_printf("[DD0] wait %d\n", i);
    }
    BOARD::diag_printf("[DD1] err=%lu max=%lu n=%lu\n",(unsigned long)result.error,(unsigned long)g_deep_max,(unsigned long)shizuku_push_diag_n);
    BOARD::diag_printf("[DD2] callee=%08lx limit=%08lx\n",(unsigned long)shizuku_push_diag[0],(unsigned long)shizuku_push_diag[1]);
    BOARD::diag_printf("[DD3] reserve=%lu hdr=%lu depth=%lu\n",(unsigned long)shizuku_push_diag[2],(unsigned long)shizuku_push_diag[3],(unsigned long)shizuku_push_diag[4]);
    BOARD::diag_printf("[DD4] push_kind=%lu reject_kind=%lx (1=do_call 2=svc_dispatch 3=descriptor, 0x100=ledger full)\n",(unsigned long)shizuku_push_diag_kind,(unsigned long)shizuku_push_diag_reject_kind);
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
    //   「[KOBJ] object <generated namer ID> is already taken by 'namer'」が出ていること。
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

  // ---- V2a Method Descriptors & Direct Trap Call (APIs 32 & 33) ----
  // ★記述子は**呼び出し元自身の表**に作られ、呼び出し元の表からだけ撃てる。
  //   対象 (desc_test) の 1 番は対象自身の中で export させる (上の注釈を参照)。
  {
    const api_result target_made =
        api(object_api::CREATE_OBJECT, OBJECT_DESC_TEST, (uintptr_t)&desc_export_fn);
    check("descriptor: create target object",
          target_made.error == (uintptr_t)object_error::OK,
          (unsigned long)target_made.error, 0);
    const api_result exported = call_method(OBJECT_DESC_TEST, 0);
    check("descriptor: target exports method 1 into its own table",
          exported.error == (uintptr_t)object_error::OK &&
              exported.value == OBJECT_DESC_TEST,
          (unsigned long)exported.value, (unsigned long)OBJECT_DESC_TEST);

    // 別オブジェクトが自分の表に作った記述子の番号。ルートの表はまだ空 (ここまで
    // ルートは記述子を作っていない) なので、ルートの k 番を撃つと拒否されるのが正しい。
    const api_result owner_made =
        api(object_api::CREATE_OBJECT, OBJECT_DESC_OWNER, (uintptr_t)&desc_make_index);
    check("descriptor: create owner object",
          owner_made.error == (uintptr_t)object_error::OK,
          (unsigned long)owner_made.error, 0);
    const api_result owner = call_method(OBJECT_DESC_OWNER, 0);
    check("descriptor: owner object makes its own descriptor",
          owner.error == (uintptr_t)object_error::OK && owner.value == 0,
          (unsigned long)owner.value, 0);
#if defined(__riscv)
    const api_result foreign = call_descriptor(owner.value);
    check("descriptor: another object's index rejected with BAD_DESCRIPTOR (22)",
          foreign.error == (uintptr_t)object_error::BAD_DESCRIPTOR,
          (unsigned long)foreign.error, (unsigned long)object_error::BAD_DESCRIPTOR);
#endif

    const api_result created = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
    check("descriptor: create valid descriptor",
          created.error == (uintptr_t)object_error::OK && created.value < 16,
          (unsigned long)created.value, 0);
    const uintptr_t desc_idx = created.value;

#if defined(__riscv)
    // 直接 trap 呼出し。戻り値・台帳・呼び出し回数・呼び先の identity を突き合わせる。
    {
      const uint32_t calls_before = g_desc_calls;
      const auto depth_before = kernel_instance.current_depth();
      const api_result desc_call = call_descriptor(desc_idx);
      check("descriptor: direct return error is OK",
            desc_call.error == (uintptr_t)object_error::OK,
            (unsigned long)desc_call.error, 0);
      // ★1..6 の和。a2..a7 のどれかが落ちていれば合計がずれる。
      check("descriptor: direct trap call returns sum 21",
            desc_call.value == 21, (unsigned long)desc_call.value, 21);
      check("descriptor: direct return restores the ledger",
            kernel_instance.current_depth() == depth_before,
            (unsigned long)kernel_instance.current_depth(),
            (unsigned long)depth_before);
      check("descriptor: callee ran exactly once",
            g_desc_calls == calls_before + 1,
            (unsigned long)g_desc_calls, (unsigned long)(calls_before + 1));
      check("descriptor: callee runs as the target object",
            g_desc_self == OBJECT_DESC_TEST,
            (unsigned long)g_desc_self, (unsigned long)OBJECT_DESC_TEST);
      check("descriptor: callee's caller is the root",
            g_desc_caller == KERNEL_OBJECT::ROOT_OBJECT,
            (unsigned long)g_desc_caller,
            (unsigned long)KERNEL_OBJECT::ROOT_OBJECT);
    }
#else
    const api_result unknown_desc = call_descriptor(desc_idx);
    check("descriptor: non-riscv rejects CALL_METHOD_DESCRIPTOR with UNKNOWN_API (1)",
          unknown_desc.error == (uintptr_t)object_error::UNKNOWN_API,
          (unsigned long)unknown_desc.error, (unsigned long)object_error::UNKNOWN_API);
#endif

    const api_result bad_obj = api(object_api::CREATE_METHOD_DESCRIPTOR, 9999, 1);
    check("descriptor: BAD_OBJECT on invalid target",
          bad_obj.error == (uintptr_t)object_error::BAD_OBJECT,
          (unsigned long)bad_obj.error, (unsigned long)object_error::BAD_OBJECT);

    const api_result bad_mth = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 999);
    check("descriptor: BAD_METHOD on out of bounds method",
          bad_mth.error == (uintptr_t)object_error::BAD_METHOD,
          (unsigned long)bad_mth.error, (unsigned long)object_error::BAD_METHOD);

    const api_result undecl_mth = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 5);
    check("descriptor: UNDECLARED_METHOD on null method",
          undecl_mth.error == (uintptr_t)object_error::UNDECLARED_METHOD,
          (unsigned long)undecl_mth.error, (unsigned long)object_error::UNDECLARED_METHOD);

#if defined(__riscv)
    // 範囲外と未公開の番号。未公開は 1 番 (ルートの表はまだ 0 番しか使っていない)。
    check("descriptor: index 16 rejected with BAD_DESCRIPTOR (22)",
          call_descriptor(16).error == (uintptr_t)object_error::BAD_DESCRIPTOR,
          (unsigned long)call_descriptor(16).error,
          (unsigned long)object_error::BAD_DESCRIPTOR);
    check("descriptor: index 0xFFFFFFFF rejected with BAD_DESCRIPTOR (22)",
          call_descriptor(0xFFFFFFFFu).error == (uintptr_t)object_error::BAD_DESCRIPTOR,
          (unsigned long)call_descriptor(0xFFFFFFFFu).error,
          (unsigned long)object_error::BAD_DESCRIPTOR);
    check("descriptor: unpublished index rejected with BAD_DESCRIPTOR (22)",
          call_descriptor(1).error == (uintptr_t)object_error::BAD_DESCRIPTOR,
          (unsigned long)call_descriptor(1).error,
          (unsigned long)object_error::BAD_DESCRIPTOR);
#endif

    // 世代の不一致。対象を差し替えると古い記述子は STALE、差し替え後に作り直したものは通る。
    const api_result replaced =
        api(object_api::CREATE_OBJECT, OBJECT_DESC_TEST, (uintptr_t)&desc_export_fn, OBJECT_REPLACE);
    check("descriptor: replace target bumps generation",
          replaced.error == (uintptr_t)object_error::OK,
          (unsigned long)replaced.error, 0);

#if defined(__riscv)
    {
      const uint32_t calls_before = g_desc_calls;
      const api_result stale = call_descriptor(desc_idx);
      check("descriptor: stale descriptor rejected with STALE_DESCRIPTOR (23)",
            stale.error == (uintptr_t)object_error::STALE_DESCRIPTOR,
            (unsigned long)stale.error, (unsigned long)object_error::STALE_DESCRIPTOR);
      // ★拒否されたら古い入口は走っていないこと。
      check("descriptor: stale rejection does not run the callee",
            g_desc_calls == calls_before,
            (unsigned long)g_desc_calls, (unsigned long)calls_before);
    }
#endif

    const api_result fresh = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
    check("descriptor: new descriptor after replace is created",
          fresh.error == (uintptr_t)object_error::OK && fresh.value < 16,
          (unsigned long)fresh.value, 0);
#if defined(__riscv)
    const api_result fresh_call = call_descriptor(fresh.value);
    check("descriptor: new descriptor after replace runs (sum 21)",
          fresh_call.error == (uintptr_t)object_error::OK && fresh_call.value == 21,
          (unsigned long)fresh_call.value, 21);
#endif

    // 旧 CALL_METHOD (グローバル経路) は従来どおり同じ入口に届くこと。
    {
      const uint32_t calls_before = g_desc_calls;
      const api_result legacy_call = api(object_api::CALL_METHOD, OBJECT_DESC_TEST, 1, 42);
      check("descriptor: legacy CALL_METHOD compatibility",
            legacy_call.error == (uintptr_t)object_error::OK,
            (unsigned long)legacy_call.error, 0);
      check("descriptor: legacy CALL_METHOD reaches the same entry",
            g_desc_calls == calls_before + 1,
            (unsigned long)g_desc_calls, (unsigned long)(calls_before + 1));
    }

    // ---- 呼び出し元の識別: A (ルート) -> B (desc_relay) -> C (desc_test, 記述子で直接呼出し) ----
    // ★C の g_desc_* に残る値が「B 経由で撃たれた」ものだと言えるように、先に空けておく。
    //   ルートは B しか呼ばず、B の表の記述子だけで C に届く。C の 1 番は上の export のまま生きている。
    {
      const api_result relay_made =
          api(object_api::CREATE_OBJECT, OBJECT_DESC_RELAY, (uintptr_t)&desc_relay_fn);
      check("descriptor A->B->C: create relay object",
            relay_made.error == (uintptr_t)object_error::OK,
            (unsigned long)relay_made.error, 0);

      g_desc_self = 0;
      g_desc_caller = 0;
      const auto root_depth_before = kernel_instance.current_depth();
      const api_result relay = call_method(OBJECT_DESC_RELAY, 0);
      check("descriptor A->B->C: root call into B returns OK",
            relay.error == (uintptr_t)object_error::OK,
            (unsigned long)relay.error, 0);
#if defined(__riscv)
      // C の identity。B の中から撃たれたので、呼んだ側は B (ルートでも C 自身でもない)。
      check("descriptor A->B->C: C self is C (desc_test)",
            g_desc_self == OBJECT_DESC_TEST,
            (unsigned long)g_desc_self, (unsigned long)OBJECT_DESC_TEST);
      check("descriptor A->B->C: C caller is B (desc_relay)",
            g_desc_caller == OBJECT_DESC_RELAY,
            (unsigned long)g_desc_caller, (unsigned long)OBJECT_DESC_RELAY);
      // C の戻り値。B が C から受け取ったものを記録させている。
      check("descriptor A->B->C: C returned sum 21",
            g_relay_call_value == 21,
            (unsigned long)g_relay_call_value, 21);
      check("descriptor A->B->C: C returned OK",
            g_relay_call_error == (uintptr_t)object_error::OK,
            (unsigned long)g_relay_call_error, (unsigned long)object_error::OK);
#endif
      // B の identity。B を呼んだのはルート、B 自身は desc_relay として走る。
      check("descriptor A->B->C: B caller is ROOT",
            g_relay_caller == KERNEL_OBJECT::ROOT_OBJECT,
            (unsigned long)g_relay_caller,
            (unsigned long)KERNEL_OBJECT::ROOT_OBJECT);
      check("descriptor A->B->C: B self is B (desc_relay)",
            g_relay_self == OBJECT_DESC_RELAY,
            (unsigned long)g_relay_self, (unsigned long)OBJECT_DESC_RELAY);
      // ★C から戻った直後の台帳 (depth) が、B が C を呼ぶ前と等しいこと。
      check("descriptor A->B->C: depth in B restored after C returns",
            g_relay_depth_after == g_relay_depth_before,
            (unsigned long)g_relay_depth_after, (unsigned long)g_relay_depth_before);
      // ルートへ戻った後の台帳。試験前と等しいこと。
      check("descriptor A->B->C: depth restored at root",
            kernel_instance.current_depth() == root_depth_before,
            (unsigned long)kernel_instance.current_depth(),
            (unsigned long)root_depth_before);
    }

    // 容量 16 超過。記述子に解放 API は無いので、ここまでに作った分 (2 本) も数に入れて埋める。
    // ★最後に行う (以後はこの表に記述子を作れない)。
    uint32_t filled = 2;
    for (int i = 0; i < 16; ++i) {
      if (api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1).error !=
          (uintptr_t)object_error::OK)
        break;
      ++filled;
    }
    check("descriptor: fill capacity up to 16", filled == 16,
          (unsigned long)filled, 16);

    const api_result over_cap = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
    check("descriptor: NO_DESCRIPTOR (21) when capacity exceeded",
          over_cap.error == (uintptr_t)object_error::NO_DESCRIPTOR,
          (unsigned long)over_cap.error, (unsigned long)object_error::NO_DESCRIPTOR);
  }

  BOARD::diag_printf("[SELFTEST] call ladder done: %lu passed, %lu failed\n",
                     (unsigned long)passed, (unsigned long)failed);
}

} // namespace selftest
} // namespace shizuku
