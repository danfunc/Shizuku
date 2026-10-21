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

// Descriptor assertion outcomes live outside the diagnostic ring so a wrapped
// ring cannot erase which checks ran or passed. The test names in check() are
// hashed in source order. Each slot maps to the corresponding descriptor-prefixed
// check() occurrence in this file; name_hash is FNV-1a of that assertion name.
// 96 slots: the descriptor-prefixed check() occurrences are 89 (the riscv build runs
// all of them; the host build runs fewer). Growing this adds 160 bytes of .noinit RAM.
constexpr uint32_t kDescriptorRecordSlots = 96;
struct descriptor_test_record_t {
  uint32_t magic;
  uint32_t version;
  uint32_t executed_count;
  uint32_t completed;
  uint32_t passed_count;
  uint32_t failed_count;
  uint32_t checksum;
  uint32_t overflow_count;
  uint32_t collision_count;
  uint32_t name_hash[kDescriptorRecordSlots];
  uint8_t status[kDescriptorRecordSlots]; // 0=not run, 3=in progress, 1=PASS, 2=FAIL, 4=SKIPPED (version 3)
  // 失敗の詳細 (診断専用, version 2)。first_* は最初の FAIL、second_* は 2 番目の FAIL。
  // FAIL の総数は failed_count。hash は name の FNV-1a。got/want は下位 32 bit。
  uint32_t first_fail_hash;
  uint32_t first_fail_got;
  uint32_t first_fail_want;
  uint32_t second_fail_hash;
  uint32_t second_fail_got;
  uint32_t second_fail_want;
};
extern "C" {
#if defined(__ELF__)
__attribute__((used, aligned(4), section(".noinit")))
#endif
volatile descriptor_test_record_t shizuku_descriptor_test_record;
}

// 診断専用 (挙動不変)。中継 A→B→C の作成前後の値を sysbus から読むための口。
// [0] 開始印 0xD1A60001, [1..6] CREATE_OBJECT 直後の値, [7] 終了印 0xD1A60002。
extern "C" {
__attribute__((used)) volatile uint32_t shizuku_desc_relay_diag[8];
}

// 診断専用 (挙動不変)。深さの試験 (desc_recur_fn) の各段の局所変数アドレスを段番号ごとに残す。
// 隣接する段のアドレスの差が 1 段あたりの実消費バイト (sp の代用)。
// [0] 到達した段数 (最後に入った段の番号)
// [1..33] 段 k の局所変数アドレス (k は 1 始まり。33 段目までを記録)
// [34] 試験の前に読んだ thread のスタック下限 (ARCH::stack_limit)。読めなければ 0
// [35] 最後に NO_STACK を観測した段の番号 (無ければ 0)
// [36] 完了印 0xD1A60003 (試験ブロックを抜けた印。riscv 以外では 0 のまま)
// [37] 深さ上限の厳密な試験を実行しなかった理由 (0=実行した, 1=下限が読めない,
//      2=段あたり消費を測れない, 3=余白が足りない)
// [38] 測った 1 段あたりの消費 (バイト), [39] 最初の段の上の余白 (バイト: 段1 - 下限)
extern "C" {
__attribute__((used)) volatile uintptr_t shizuku_desc_recur_diag[40];
}

uint32_t passed = 0;
uint32_t failed = 0;

// 旧 4 引数 CALL_METHOD の callee が捕まえた a4/a5 (riscv の naked probe が書く)。
// 無名 namespace の中に置くと内部リンケージになり、asm の la が同じ TU の局所シンボルに
// 頼る形になる。外部リンケージ (shizuku::selftest 直下、extern "C" で名前は修飾されない)
// に出して、名前解決を普通のグローバル参照にしてある。
extern "C" {
uintptr_t g_desc_legacy_a4 = UINTPTR_MAX;
uintptr_t g_desc_legacy_a5 = UINTPTR_MAX;
}
#if defined(__riscv)
extern "C" __attribute__((naked)) uintptr_t desc_legacy_probe(
    uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  __asm volatile("la t0, g_desc_legacy_a4\n"
                 "sw a4, 0(t0)\n"
                 "la t0, g_desc_legacy_a5\n"
                 "sw a5, 0(t0)\n"
                 "li a0, 77\n"
                 "ret\n");
}
#endif

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uint32_t DESCRIPTOR_TEST_RECORD_MAGIC = 0x44535452u; // "DSTR"
constexpr uint32_t DESCRIPTOR_TEST_RECORD_VERSION = 3;
static_assert(sizeof(descriptor_test_record_t) == 540,
              "descriptor record layout changed; re-audit C6 noinit RAM map");
static_assert(__builtin_offsetof(descriptor_test_record_t, status) == 420,
              "descriptor record status offset moved");
static_assert(__builtin_offsetof(descriptor_test_record_t, first_fail_hash) == 516,
              "descriptor record first_fail_hash offset moved");
static_assert(__builtin_offsetof(descriptor_test_record_t, first_fail_got) == 520,
              "descriptor record first_fail_got offset moved");
static_assert(__builtin_offsetof(descriptor_test_record_t, first_fail_want) == 524,
              "descriptor record first_fail_want offset moved");
static_assert(__builtin_offsetof(descriptor_test_record_t, second_fail_hash) == 528,
              "descriptor record second_fail_hash offset moved");
static_assert(__builtin_offsetof(descriptor_test_record_t, second_fail_got) == 532,
              "descriptor record second_fail_got offset moved");
static_assert(__builtin_offsetof(descriptor_test_record_t, second_fail_want) == 536,
              "descriptor record second_fail_want offset moved");
static_assert(kDescriptorRecordSlots >= 89,
              "descriptor record must hold every current assertion");

void reset_descriptor_test_record() {
  shizuku_descriptor_test_record.magic = 0;
  shizuku_descriptor_test_record.version = DESCRIPTOR_TEST_RECORD_VERSION;
  shizuku_descriptor_test_record.executed_count = 0;
  shizuku_descriptor_test_record.completed = 0;
  shizuku_descriptor_test_record.passed_count = 0;
  shizuku_descriptor_test_record.failed_count = 0;
  shizuku_descriptor_test_record.checksum = 0;
  shizuku_descriptor_test_record.overflow_count = 0;
  shizuku_descriptor_test_record.collision_count = 0;
  shizuku_descriptor_test_record.first_fail_hash = 0;
  shizuku_descriptor_test_record.first_fail_got = 0;
  shizuku_descriptor_test_record.first_fail_want = 0;
  shizuku_descriptor_test_record.second_fail_hash = 0;
  shizuku_descriptor_test_record.second_fail_got = 0;
  shizuku_descriptor_test_record.second_fail_want = 0;
  for (unsigned i = 0; i < kDescriptorRecordSlots; ++i) {
    shizuku_descriptor_test_record.name_hash[i] = 0;
    shizuku_descriptor_test_record.status[i] = 0;
  }
  shizuku_descriptor_test_record.magic = DESCRIPTOR_TEST_RECORD_MAGIC;
}

void record_descriptor_check(const char *name, bool ok, unsigned long got,
                             unsigned long want) {
  if (!(name[0] == 'd' && name[1] == 'e' && name[2] == 's' &&
        name[3] == 'c' && name[4] == 'r' && name[5] == 'i' &&
        name[6] == 'p' && name[7] == 't' && name[8] == 'o' &&
        name[9] == 'r'))
    return;
  const uint32_t slot = shizuku_descriptor_test_record.executed_count;
  if (slot >= kDescriptorRecordSlots) {
    shizuku_descriptor_test_record.overflow_count =
        shizuku_descriptor_test_record.overflow_count + 1;
    return;
  }
  uint32_t hash = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    hash = (hash ^ *p) * 16777619u;
  for (uint32_t i = 0; i < slot; ++i) {
    if (shizuku_descriptor_test_record.name_hash[i] == hash) {
      shizuku_descriptor_test_record.collision_count =
          shizuku_descriptor_test_record.collision_count + 1;
      break;
    }
  }
  shizuku_descriptor_test_record.name_hash[slot] = hash;
  shizuku_descriptor_test_record.status[slot] = 3;
  shizuku_descriptor_test_record.executed_count = slot + 1;
  shizuku_descriptor_test_record.status[slot] = ok ? 1 : 2;
  if (ok)
    shizuku_descriptor_test_record.passed_count =
        shizuku_descriptor_test_record.passed_count + 1;
  else {
    // 診断専用: 1 本目と 2 本目の FAIL の詳細を残す (数える処理は下の failed_count が担う)。
    if (shizuku_descriptor_test_record.failed_count == 0) {
      shizuku_descriptor_test_record.first_fail_hash = hash;
      shizuku_descriptor_test_record.first_fail_got = (uint32_t)got;
      shizuku_descriptor_test_record.first_fail_want = (uint32_t)want;
    } else if (shizuku_descriptor_test_record.failed_count == 1) {
      shizuku_descriptor_test_record.second_fail_hash = hash;
      shizuku_descriptor_test_record.second_fail_got = (uint32_t)got;
      shizuku_descriptor_test_record.second_fail_want = (uint32_t)want;
    }
    shizuku_descriptor_test_record.failed_count =
        shizuku_descriptor_test_record.failed_count + 1;
  }
  shizuku_descriptor_test_record.checksum =
      (shizuku_descriptor_test_record.checksum << 5) ^ hash ^ (ok ? 1u : 2u);
}

// 走らせなかった試験の印 (status=4)。PASS/FAIL には数えない。名前の付け方は check() と同じ。
void record_descriptor_skip(const char *name) {
  if (!(name[0] == 'd' && name[1] == 'e' && name[2] == 's' &&
        name[3] == 'c' && name[4] == 'r' && name[5] == 'i' &&
        name[6] == 'p' && name[7] == 't' && name[8] == 'o' &&
        name[9] == 'r'))
    return;
  const uint32_t slot = shizuku_descriptor_test_record.executed_count;
  if (slot >= kDescriptorRecordSlots) {
    shizuku_descriptor_test_record.overflow_count =
        shizuku_descriptor_test_record.overflow_count + 1;
    return;
  }
  uint32_t hash = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    hash = (hash ^ *p) * 16777619u;
  shizuku_descriptor_test_record.name_hash[slot] = hash;
  shizuku_descriptor_test_record.status[slot] = 4;
  shizuku_descriptor_test_record.executed_count = slot + 1;
  shizuku_descriptor_test_record.checksum =
      (shizuku_descriptor_test_record.checksum << 5) ^ hash ^ 4u;
}

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
#if defined(__riscv)
// 中継 B が自分の子として作る C (B の関数内で CREATE_OBJECT する)。
constexpr uintptr_t OBJECT_DESC_RELAY_TARGET = object_id::desc_relay_target;
#endif
// 作成権限の試験用。KERNEL (役付き) / CHILD と GRANDCHILD (OWNER の子と孫)。
constexpr uintptr_t OBJECT_DESC_KERNEL = object_id::desc_kernel;
constexpr uintptr_t OBJECT_DESC_CHILD = object_id::desc_child;
constexpr uintptr_t OBJECT_DESC_GRANDCHILD = object_id::desc_grandchild;
// 記述子経路の深さ上限の試験用 (自分自身への記述子を撃ち続ける)。
constexpr uintptr_t OBJECT_DESC_RECUR = object_id::desc_recur;
constexpr uintptr_t METHOD_MAIN = 0;

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  record_descriptor_check(name, ok, got, want);
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
uint32_t g_desc_v2_calls = 0;
uintptr_t g_desc_self = 0;
uintptr_t g_desc_caller = 0;
bool g_desc_export_v2 = false;
bool g_owner_probe_old_index = false;
uintptr_t g_owner_old_index_error = (uintptr_t)object_error::UNKNOWN_API;

uintptr_t desc_target_fn(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
  ++g_desc_calls;
  g_desc_self = api(object_api::GET_CURRENT_OBJECT).value;
  g_desc_caller = api(object_api::GET_CALLER_OBJECT).value;
  return a + b + c + d + e + f;
}

uintptr_t desc_target_fn_v2(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,
                            uintptr_t e, uintptr_t f) {
  ++g_desc_v2_calls;
  g_desc_self = api(object_api::GET_CURRENT_OBJECT).value;
  g_desc_caller = api(object_api::GET_CALLER_OBJECT).value;
  return a + b + c + d + e + f + 1000;
}

// riscv の naked 版は上 (外部リンケージ) にある。ここは riscv 以外の代用だけ。
#if !defined(__riscv)
uintptr_t desc_legacy_probe(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return 77;
}
#endif

// 記述子の対象オブジェクトの main。走った先で自分の表に desc_target_fn を export する。
// ★EXPORT_METHOD は current_object (呼び出し中のオブジェクト) の表に入る。ルートから
//   撃つとルート自身の表に入るので、対象オブジェクトの中から撃たせる。
uintptr_t desc_export_fn(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const uintptr_t entry = (uintptr_t)(g_desc_export_v2 ? &desc_target_fn_v2
                                                        : &desc_target_fn);
  const api_result exported = api(object_api::EXPORT_METHOD, 1, entry);
  const api_result legacy = api(object_api::EXPORT_METHOD, 2,
                                (uintptr_t)&desc_legacy_probe);
  return exported.error == (uintptr_t)object_error::OK &&
                 legacy.error == (uintptr_t)object_error::OK
             ? exported.value
             : 0xDEAD0000u | (uint32_t)(exported.error ? exported.error
                                                       : legacy.error);
}

// 作成権限の試験 (argument = 1 で呼ばれたとき): まず兄弟 (DESC_TEST) への作成を試みて
// 拒否されることを記録し、続けて自分自身の 1 番への作成が index 0 に入ることを記録する。
// 兄弟への拒否で枠が進んでいなければ、自己作成は (owner_recreated の後の) index 1 になる。
uintptr_t g_sibling_probe_error = (uintptr_t)object_error::UNKNOWN_API;
uintptr_t g_sibling_probe_index = UINTPTR_MAX;

uintptr_t desc_sibling_probe() {
  g_sibling_probe_error =
      api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1).error;
  // 自分自身の表に 1 番を export してから、自分自身への記述子を作る (自分自身は常に許可)。
  const api_result exported =
      api(object_api::EXPORT_METHOD, 1, (uintptr_t)&desc_target_fn);
  const api_result self =
      api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_OWNER, 1);
  g_sibling_probe_index = self.error == (uintptr_t)object_error::OK
                              ? self.value
                              : 0xDEAD0000u | (uint32_t)self.error;
  return exported.error;
}

// 階層と役の試験 (mode 2)。子 CH と孫 GC は下の関数で作る。GC の生成者は CH なので、
// OWNER から見て GC は孫になる。権限は親子関係で決まり、役は使わない。
uintptr_t g_role_flag_error = (uintptr_t)object_error::UNKNOWN_API;
uintptr_t g_child_create_error = (uintptr_t)object_error::UNKNOWN_API;
uintptr_t g_grandchild_create_error = (uintptr_t)object_error::UNKNOWN_API;
uintptr_t g_child_desc_error = (uintptr_t)object_error::UNKNOWN_API;
uintptr_t g_child_desc_index = UINTPTR_MAX;
uintptr_t g_grandchild_desc_error = (uintptr_t)object_error::UNKNOWN_API;
// kernel 役の K の結果。K は役付きなので、兄弟 (DESC_TEST) も孫 (GC) も通るはず。
uintptr_t g_kernel_sibling_error = (uintptr_t)object_error::UNKNOWN_API;
uintptr_t g_kernel_grandchild_error = (uintptr_t)object_error::UNKNOWN_API;

// 孫: 中身は使わない。method 0 だけを記述子の対象に使う。
uintptr_t desc_grandchild_fn(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return 0;
}

// 子: 自分の子として孫を作る (孫の生成者は子)。
uintptr_t desc_child_fn(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  g_grandchild_create_error =
      api(object_api::CREATE_OBJECT, OBJECT_DESC_GRANDCHILD, (uintptr_t)&desc_grandchild_fn).error;
  return 0;
}

// 役付きの K。作成権限の規則の外に置かれ、兄弟と孫のどちらへの作成も通る。
uintptr_t desc_kernel_fn(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  g_kernel_sibling_error =
      api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1).error;
  g_kernel_grandchild_error =
      api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_GRANDCHILD, 0).error;
  return 0;
}

uintptr_t desc_hierarchy_probe() {
  // 役の付与は一般オブジェクトには拒否される (枠も子も作られない)。
  g_role_flag_error =
      api(object_api::CREATE_OBJECT, OBJECT_DESC_CHILD, (uintptr_t)&desc_child_fn,
          KERNEL_OBJECT::INTERNAL_FLAG_KERNEL_ROLE).error;
  g_child_create_error =
      api(object_api::CREATE_OBJECT, OBJECT_DESC_CHILD, (uintptr_t)&desc_child_fn).error;
  call_method(OBJECT_DESC_CHILD, 0); // 子を走らせる。子が孫を作る。
  // 直接の子 (生成者は自分) への作成は通る。
  const api_result child = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_CHILD, 0);
  g_child_desc_error = child.error;
  g_child_desc_index = child.value;
  // 孫 (生成者の生成者) への作成は拒否される。
  g_grandchild_desc_error =
      api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_GRANDCHILD, 0).error;
  return child.error;
}

// 別オブジェクトの main。自分の表に、対象の 1 番を指す記述子を作って番号を返す。
uintptr_t desc_make_index(uintptr_t mode, uintptr_t, uintptr_t, uintptr_t) {
  if (mode == 1)
    return desc_sibling_probe();
  if (mode == 2)
    return desc_hierarchy_probe();
#if defined(__riscv)
  if (g_owner_probe_old_index) {
    g_owner_old_index_error = call_descriptor(0).error;
    g_owner_probe_old_index = false;
  }
#endif
  // 自分の 1 番を自分の表に export してから、自分自身への記述子を作る (自己は常に許可)。
  // 兄弟 (DESC_TEST) への作成は作成権限の規則で拒否されるため、ここでは使わない。
  const api_result exported = api(object_api::EXPORT_METHOD, 1, (uintptr_t)&desc_target_fn);
  if (exported.error != (uintptr_t)object_error::OK)
    return 0xDEAD0000u | (uint32_t)exported.error;
  const api_result made = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_OWNER, 1);
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
#if defined(__riscv)
uintptr_t g_relay_target_create_error = (uintptr_t)object_error::UNKNOWN_API;
uintptr_t g_relay_desc_error = (uintptr_t)object_error::UNKNOWN_API;

// 中継の子 C の main。走ると自分の表に 1 番 (desc_target_fn) を export する。
// B がこれを一度走らせてから、C への記述子を B の表に作る。
uintptr_t desc_relay_target_fn(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const api_result exported =
      api(object_api::EXPORT_METHOD, 1, (uintptr_t)&desc_target_fn);
  return exported.error == (uintptr_t)object_error::OK
             ? exported.value
             : 0xDEAD0000u | (uint32_t)exported.error;
}
#endif

uintptr_t desc_relay_fn(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  g_relay_self = api(object_api::GET_CURRENT_OBJECT).value;
  g_relay_caller = api(object_api::GET_CALLER_OBJECT).value;
  g_relay_depth_before = kernel_instance.current_depth();
  // ★呼出し前の値は「呼ばれていない」を表すように置いておく。riscv 以外ではこれが残る。
  g_relay_call_value = 0;
  g_relay_call_error = (uintptr_t)object_error::UNKNOWN_API;
#if defined(__riscv)
  // C は B が自分の子として作る (B の creator が C になる)。C の main を一度走らせて
  // C 自身に 1 番を export させ、その 1 番への記述子を B の表に作る。
  // 作成も記述子も親→直接の子の一般規則のまま通る (kernel 役は使わない)。
  g_relay_target_create_error =
      api(object_api::CREATE_OBJECT, OBJECT_DESC_RELAY_TARGET,
          (uintptr_t)&desc_relay_target_fn)
          .error;
  if (g_relay_target_create_error != (uintptr_t)object_error::OK) {
    g_relay_depth_after = kernel_instance.current_depth();
    return 0xDEAD0000u | (uint32_t)g_relay_target_create_error;
  }
  const api_result target_exported = call_method(OBJECT_DESC_RELAY_TARGET, 0);
  if (target_exported.error != (uintptr_t)object_error::OK) {
    g_relay_desc_error = target_exported.error;
    g_relay_depth_after = kernel_instance.current_depth();
    return 0xDEAD0000u | (uint32_t)target_exported.error;
  }
  // 記述子は B 自身の表に作られる (呼び出し元の表からだけ撃てる、の確認も兼ねる)。
  const api_result made =
      api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_RELAY_TARGET, 1);
  g_relay_desc_error = made.error;
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

// ---- 記述子経路の深さ上限 (MAX_DEPTH) --------------------------------------
// 自分の表の 0 番 (自分自身への記述子) を撃ち続ける。上限に達した呼出しだけが NO_STACK で
// 拒否され、その段がそれを観測する。記述子経路は a2..a7 = 1..6 を渡すので、設定の呼出しは
// それと区別できる鍵値で行う。
uint32_t g_recur_calls = 0;
uintptr_t g_recur_refused = (uintptr_t)object_error::UNKNOWN_API;
uintptr_t g_recur_index = UINTPTR_MAX;
constexpr uintptr_t RECUR_SETUP_KEY = 0x5E7;
// 段の入口に要る余白 (rv32 call_entry_has_return_room の 2T + 残り)。診断と試験の判断に使う。
constexpr uintptr_t RECUR_ENTRY_ROOM_BYTES = 1140;

uintptr_t desc_recur_fn(uintptr_t mode, uintptr_t, uintptr_t, uintptr_t) {
  if (mode == RECUR_SETUP_KEY) {
    const api_result made =
        api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_RECUR, 0);
    g_recur_index = made.value;
    return made.error;
  }
  ++g_recur_calls;
  // 診断専用 (挙動不変): この段の局所変数のアドレスを段番号ごとに残す (sp の代用)。
  volatile uintptr_t frame_mark = 0;
  shizuku_desc_recur_diag[0] = g_recur_calls;
  if (g_recur_calls <= 33)
    shizuku_desc_recur_diag[g_recur_calls] = (uintptr_t)&frame_mark;
#if defined(__riscv)
  const api_result next = call_descriptor(g_recur_index);
  if (next.error != (uintptr_t)object_error::OK)
    g_recur_refused = next.error;
  if (next.error == (uintptr_t)object_error::NO_STACK)
    shizuku_desc_recur_diag[35] = g_recur_calls;
#endif
  return 0;
}

} // namespace

void call_ladder() {
  reset_descriptor_test_record();
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
    // (h) 拒否の前後で ledger (depth / call_count) が変わらず、呼び先も走らないこと。
    // ルートの表は空なので、OWNER の番号はルートの表では必ず拒否される。
    const uint32_t foreign_depth = kernel_instance.current_depth();
    const uint32_t foreign_count = kernel_instance.current_call_count();
    const uint32_t foreign_calls = g_desc_calls;
    const api_result foreign = call_descriptor(owner.value);
    check("descriptor: another object's index rejected with BAD_DESCRIPTOR (22)",
          foreign.error == (uintptr_t)object_error::BAD_DESCRIPTOR,
          (unsigned long)foreign.error, (unsigned long)object_error::BAD_DESCRIPTOR);
    check("descriptor: rejected foreign index leaves ledger depth unchanged",
          kernel_instance.current_depth() == foreign_depth,
          (unsigned long)kernel_instance.current_depth(), (unsigned long)foreign_depth);
    check("descriptor: rejected foreign index leaves call count unchanged",
          kernel_instance.current_call_count() == foreign_count,
          (unsigned long)kernel_instance.current_call_count(), (unsigned long)foreign_count);
    check("descriptor: rejected foreign index does not run the callee",
          g_desc_calls == foreign_calls,
          (unsigned long)g_desc_calls, (unsigned long)foreign_calls);
#endif
    const api_result owner_replaced = api(
        object_api::CREATE_OBJECT, OBJECT_DESC_OWNER, (uintptr_t)&desc_make_index,
        OBJECT_REPLACE);
    check("descriptor: replace owner invalidates its descriptor table",
          owner_replaced.error == (uintptr_t)object_error::OK,
          (unsigned long)owner_replaced.error, 0);
    g_owner_probe_old_index = true;
    const api_result owner_recreated = call_method(OBJECT_DESC_OWNER, 0);
#if defined(__riscv)
    check("descriptor: old owner index rejected before new allocation",
          g_owner_old_index_error == (uintptr_t)object_error::BAD_DESCRIPTOR,
          (unsigned long)g_owner_old_index_error,
          (unsigned long)object_error::BAD_DESCRIPTOR);
#endif
    check("descriptor: replacement owner starts a new table at index zero",
          owner_recreated.error == (uintptr_t)object_error::OK &&
              owner_recreated.value == 0,
          (unsigned long)owner_recreated.value, 0);

    // 作成権限: 一般オブジェクト (OWNER) から兄弟 (DESC_TEST) への作成は拒否され、
    // 枠を消費しない。続く自己作成が index 0 に入ることで確かめる。
    const api_result sibling_probe = call_method(OBJECT_DESC_OWNER, 1);
    check("descriptor: sibling creation refused with NOT_PRIVILEGED",
          g_sibling_probe_error == (uintptr_t)object_error::NOT_PRIVILEGED,
          (unsigned long)g_sibling_probe_error,
          (unsigned long)object_error::NOT_PRIVILEGED);
    // index 0 は owner_recreated のもの。拒否で枠が進んでいなければ自己作成は index 1。
    check("descriptor: refused sibling creation consumes no slot (self at index 1)",
          sibling_probe.error == (uintptr_t)object_error::OK &&
              g_sibling_probe_index == 1,
          (unsigned long)g_sibling_probe_index, 1);

    // 階層 (mode 2): 一般オブジェクト OWNER が子 CH を作り、CH が孫 GC を作る。
    // 直接の子 CH への作成は通り、孫 GC への作成は拒否される (枠も進まない)。
    // 役の付与を一般オブジェクトが要求すると拒否される。
    const api_result hierarchy = call_method(OBJECT_DESC_OWNER, 2);
    check("descriptor: general object cannot grant kernel role (NOT_PRIVILEGED)",
          g_role_flag_error == (uintptr_t)object_error::NOT_PRIVILEGED,
          (unsigned long)g_role_flag_error,
          (unsigned long)object_error::NOT_PRIVILEGED);
    check("descriptor: general object creates a child object",
          g_child_create_error == (uintptr_t)object_error::OK,
          (unsigned long)g_child_create_error, 0);
    check("descriptor: child's child (grandchild) created by the child",
          g_grandchild_create_error == (uintptr_t)object_error::OK,
          (unsigned long)g_grandchild_create_error, 0);
    check("descriptor: direct child descriptor allowed (index 2)",
          hierarchy.error == (uintptr_t)object_error::OK &&
              g_child_desc_error == (uintptr_t)object_error::OK &&
              g_child_desc_index == 2,
          (unsigned long)g_child_desc_index, 2);
    check("descriptor: grandchild descriptor refused with NOT_PRIVILEGED",
          g_grandchild_desc_error == (uintptr_t)object_error::NOT_PRIVILEGED,
          (unsigned long)g_grandchild_desc_error,
          (unsigned long)object_error::NOT_PRIVILEGED);

    // kernel 役の object K: ROOT が役付きで作り、K から兄弟と孫の両方への作成が通る。
    const api_result kernel_made = api(
        object_api::CREATE_OBJECT, OBJECT_DESC_KERNEL, (uintptr_t)&desc_kernel_fn,
        KERNEL_OBJECT::INTERNAL_FLAG_KERNEL_ROLE);
    check("descriptor: root creates kernel-role object",
          kernel_made.error == (uintptr_t)object_error::OK,
          (unsigned long)kernel_made.error, 0);
    const api_result kernel_run = call_method(OBJECT_DESC_KERNEL, 0);
    check("descriptor: kernel-role object creates descriptor for sibling",
          kernel_run.error == (uintptr_t)object_error::OK &&
              g_kernel_sibling_error == (uintptr_t)object_error::OK,
          (unsigned long)g_kernel_sibling_error, (unsigned long)object_error::OK);
    check("descriptor: kernel-role object creates descriptor for grandchild",
          g_kernel_grandchild_error == (uintptr_t)object_error::OK,
          (unsigned long)g_kernel_grandchild_error, (unsigned long)object_error::OK);

    // K を OBJECT_REPLACE で差し替える (付与の要求なし)。役は引き継がれ、新しい K も同じ扱い。
    const api_result kernel_replaced = api(
        object_api::CREATE_OBJECT, OBJECT_DESC_KERNEL, (uintptr_t)&desc_kernel_fn,
        OBJECT_REPLACE);
    check("descriptor: replace kernel-role object",
          kernel_replaced.error == (uintptr_t)object_error::OK,
          (unsigned long)kernel_replaced.error, 0);
    g_kernel_sibling_error = (uintptr_t)object_error::UNKNOWN_API;
    g_kernel_grandchild_error = (uintptr_t)object_error::UNKNOWN_API;
    const api_result kernel_rerun = call_method(OBJECT_DESC_KERNEL, 0);
    check("descriptor: replaced kernel-role object keeps the role for sibling",
          kernel_rerun.error == (uintptr_t)object_error::OK &&
              g_kernel_sibling_error == (uintptr_t)object_error::OK,
          (unsigned long)g_kernel_sibling_error, (unsigned long)object_error::OK);
    check("descriptor: replaced kernel-role object keeps the role for grandchild",
          g_kernel_grandchild_error == (uintptr_t)object_error::OK,
          (unsigned long)g_kernel_grandchild_error, (unsigned long)object_error::OK);

    const api_result created = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
    check("descriptor: create valid descriptor",
          created.error == (uintptr_t)object_error::OK && created.value < 16,
          (unsigned long)created.value, 0);
    const uintptr_t desc_idx = created.value;
    uint32_t root_descriptors_created = 1;

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

    // Re-export at a different PC must stale the old index without recycling it.
    g_desc_export_v2 = true;
    const api_result reexported = call_method(OBJECT_DESC_TEST, 0);
    check("descriptor: target re-export succeeds",
          reexported.error == (uintptr_t)object_error::OK &&
              reexported.value == OBJECT_DESC_TEST,
          (unsigned long)reexported.error, 0);
#if defined(__riscv)
    {
      const uint32_t v2_before = g_desc_v2_calls;
      const api_result stale_reexport = call_descriptor(desc_idx);
      check("descriptor: re-export makes old index stale",
            stale_reexport.error == (uintptr_t)object_error::STALE_DESCRIPTOR,
            (unsigned long)stale_reexport.error,
            (unsigned long)object_error::STALE_DESCRIPTOR);
      check("descriptor: stale re-export does not enter changed PC",
            g_desc_v2_calls == v2_before,
            (unsigned long)g_desc_v2_calls, (unsigned long)v2_before);
    }
#endif
    const api_result fresh_reexport =
        api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
    check("descriptor: new index after re-export is allocated",
          fresh_reexport.error == (uintptr_t)object_error::OK &&
              fresh_reexport.value != desc_idx,
          (unsigned long)fresh_reexport.value, 1);
    if (fresh_reexport.error == (uintptr_t)object_error::OK)
      ++root_descriptors_created;
#if defined(__riscv)
    const api_result fresh_reexport_call = call_descriptor(fresh_reexport.value);
    check("descriptor: new index enters changed PC (sum 1021)",
          fresh_reexport_call.error == (uintptr_t)object_error::OK &&
              fresh_reexport_call.value == 1021,
          (unsigned long)fresh_reexport_call.value, 1021);
    check("descriptor: old index stays stale after a different slot is allocated",
          call_descriptor(desc_idx).error ==
              (uintptr_t)object_error::STALE_DESCRIPTOR,
          (unsigned long)call_descriptor(desc_idx).error,
          (unsigned long)object_error::STALE_DESCRIPTOR);
#endif

    // OBJECT_REPLACE clears methods 1+; the new object must export them again.
    g_desc_export_v2 = false;
    const api_result replaced =
        api(object_api::CREATE_OBJECT, OBJECT_DESC_TEST, (uintptr_t)&desc_export_fn, OBJECT_REPLACE);
    check("descriptor: replace target bumps generation",
          replaced.error == (uintptr_t)object_error::OK,
          (unsigned long)replaced.error, 0);
    const api_result reexport_after_replace = call_method(OBJECT_DESC_TEST, 0);
    check("descriptor: replacement method 0 re-exports method 1",
          reexport_after_replace.error == (uintptr_t)object_error::OK &&
              reexport_after_replace.value == OBJECT_DESC_TEST,
          (unsigned long)reexport_after_replace.error, 0);

#if defined(__riscv)
    {
      const uint32_t calls_before = g_desc_calls;
      const uint32_t stale_depth = kernel_instance.current_depth();
      const uint32_t stale_count = kernel_instance.current_call_count();
      const api_result stale = call_descriptor(fresh_reexport.value);
      check("descriptor: stale descriptor rejected with STALE_DESCRIPTOR (23)",
            stale.error == (uintptr_t)object_error::STALE_DESCRIPTOR,
            (unsigned long)stale.error, (unsigned long)object_error::STALE_DESCRIPTOR);
      // ★拒否されたら古い入口は走っていないこと。
      check("descriptor: stale rejection does not run the callee",
            g_desc_calls == calls_before,
            (unsigned long)g_desc_calls, (unsigned long)calls_before);
      // (h) 拒否は段を積む前に返るので、ledger は元のまま。
      check("descriptor: stale rejection leaves ledger depth unchanged",
            kernel_instance.current_depth() == stale_depth,
            (unsigned long)kernel_instance.current_depth(), (unsigned long)stale_depth);
      check("descriptor: stale rejection leaves call count unchanged",
            kernel_instance.current_call_count() == stale_count,
            (unsigned long)kernel_instance.current_call_count(), (unsigned long)stale_count);
    }
#endif

#if defined(__riscv)
    // (d) 記述子経路の深さ上限。RECUR は自分自身への記述子 (0 番) を撃ち続け、上限に達した
    // 最後の呼出しだけが NO_STACK で拒否される。拒否された呼出しは callee を走らせない。
    // 上限までに走った段の数は MAX_DEPTH と一致する (CALL 段 1 本ぶんは ROOT からの call_method)。
    {
      const api_result recur_made =
          api(object_api::CREATE_OBJECT, OBJECT_DESC_RECUR, (uintptr_t)&desc_recur_fn);
      check("descriptor: create recursion object",
            recur_made.error == (uintptr_t)object_error::OK,
            (unsigned long)recur_made.error, 0);
      const api_result setup = call_method(OBJECT_DESC_RECUR, RECUR_SETUP_KEY);
      check("descriptor: recursion object creates its self descriptor",
            setup.error == (uintptr_t)object_error::OK && g_recur_index < 16,
            (unsigned long)g_recur_index, 0);
      const uint32_t recur_depth = kernel_instance.current_depth();
      const uint32_t recur_count = kernel_instance.current_call_count();
      // 診断配列を空にし、走らせる前に thread のスタック下限を読んでおく。
      for (unsigned i = 0; i < 40; ++i)
        shizuku_desc_recur_diag[i] = 0;
      shizuku_desc_recur_diag[34] =
          (uintptr_t)ARCH::stack_limit(*kernel_instance.current_context());
      g_recur_calls = 0;
      g_recur_refused = (uintptr_t)object_error::UNKNOWN_API;
      const api_result top = call_method(OBJECT_DESC_RECUR, 0);
      const uintptr_t recur_n = g_recur_calls;
      // (a) 常に実行: 止まった理由が スタック上限 でも 深さ上限 でも、NO_STACK で拒否される。
      check("descriptor: recursion refused with NO_STACK (stack or depth limit)",
            g_recur_refused == (uintptr_t)object_error::NO_STACK,
            (unsigned long)g_recur_refused, (unsigned long)object_error::NO_STACK);
      check("descriptor: recursion ran between 1 and MAX_DEPTH callees",
            recur_n >= 1 && recur_n <= KERNEL_OBJECT::MAX_DEPTH,
            (unsigned long)recur_n, (unsigned long)KERNEL_OBJECT::MAX_DEPTH);
      check("descriptor: top-level recursive call returned OK",
            top.error == (uintptr_t)object_error::OK,
            (unsigned long)top.error, 0);
      check("descriptor: ledger depth restored after recursion",
            kernel_instance.current_depth() == recur_depth,
            (unsigned long)kernel_instance.current_depth(), (unsigned long)recur_depth);
      check("descriptor: call count restored after recursion",
            kernel_instance.current_call_count() == recur_count,
            (unsigned long)kernel_instance.current_call_count(), (unsigned long)recur_count);

      // (b) 深さ上限の厳密な試験。上の走行で測った 1 段あたり消費 s と、下限より上の余白
      // room から、MAX_DEPTH 段に足りると言える時だけ check を呼ぶ。足りない/測れないなら
      // check は呼ばず、status=4 (SKIPPED) と理由コードだけを残す (偽の PASS/FAIL を出さない)。
      const uintptr_t limit = shizuku_desc_recur_diag[34];
      const uintptr_t first = shizuku_desc_recur_diag[1];
      const uintptr_t measured = recur_n < 33 ? recur_n : 33;
      uintptr_t stride = 0;
      uintptr_t room = 0;
      uint32_t reason = 0;
      if (limit == 0) {
        reason = 1; // 下限が読めない
      } else if (measured < 2 || first < shizuku_desc_recur_diag[measured]) {
        reason = 2; // 段あたり消費を測れない
      } else {
        stride = (first - shizuku_desc_recur_diag[measured]) / (measured - 1);
        room = first > limit ? first - limit : 0;
        // 1140 = call_entry_has_return_room の 2T + 残り (rv32: 2*224 + 692)。
        if (room < stride * KERNEL_OBJECT::MAX_DEPTH + RECUR_ENTRY_ROOM_BYTES)
          reason = 3; // 余白が足りない
      }
      shizuku_desc_recur_diag[37] = reason;
      shizuku_desc_recur_diag[38] = stride;
      shizuku_desc_recur_diag[39] = room;
      if (reason == 0) {
        check("descriptor: recursion ran exactly MAX_DEPTH callees",
              recur_n == KERNEL_OBJECT::MAX_DEPTH,
              (unsigned long)recur_n, (unsigned long)KERNEL_OBJECT::MAX_DEPTH);
      } else {
        record_descriptor_skip("descriptor: recursion ran exactly MAX_DEPTH callees");
        BOARD::diag_printf("[SELFTEST] SKIP descriptor: recursion ran exactly MAX_DEPTH callees (reason=%lu)\n",
                           (unsigned long)reason);
      }
    }
#endif
    shizuku_desc_recur_diag[36] = 0xD1A60003u;

    const api_result fresh = api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
    check("descriptor: new descriptor after replace is created",
          fresh.error == (uintptr_t)object_error::OK && fresh.value < 16,
          (unsigned long)fresh.value, 2);
    if (fresh.error == (uintptr_t)object_error::OK)
      ++root_descriptors_created;
#if defined(__riscv)
    const api_result fresh_call = call_descriptor(fresh.value);
    check("descriptor: new descriptor after replace runs (sum 21)",
          fresh_call.error == (uintptr_t)object_error::OK && fresh_call.value == 21,
          (unsigned long)fresh_call.value, 21);
#endif

    // Region grant and revoke change the target generation, so cached rights
    // cannot survive either operation. New descriptors use the current window.
    const api_result grant_one =
        api(object_api::GRANT_REGION, OBJECT_DESC_TEST, 0x1000, 0x1fff);
    check("descriptor: initial region grant succeeds",
          grant_one.error == (uintptr_t)object_error::OK,
          (unsigned long)grant_one.error, 0);
    const api_result grant_desc =
        api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
    check("descriptor: descriptor after region grant is created",
          grant_desc.error == (uintptr_t)object_error::OK,
          (unsigned long)grant_desc.error, 0);
    if (grant_desc.error == (uintptr_t)object_error::OK)
      ++root_descriptors_created;
    const api_result grant_two =
        api(object_api::GRANT_REGION, OBJECT_DESC_TEST, 0x2000, 0x2fff);
    check("descriptor: region change succeeds",
          grant_two.error == (uintptr_t)object_error::OK,
          (unsigned long)grant_two.error, 0);
#if defined(__riscv)
    check("descriptor: region change stales old grant descriptor",
          call_descriptor(grant_desc.value).error ==
              (uintptr_t)object_error::STALE_DESCRIPTOR,
          (unsigned long)call_descriptor(grant_desc.value).error,
          (unsigned long)object_error::STALE_DESCRIPTOR);
#endif
    const api_result changed_window_desc =
        api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
    check("descriptor: descriptor after region change is created",
          changed_window_desc.error == (uintptr_t)object_error::OK,
          (unsigned long)changed_window_desc.error, 0);
    if (changed_window_desc.error == (uintptr_t)object_error::OK)
      ++root_descriptors_created;
    const api_result region_revoke =
        api(object_api::GRANT_REGION, OBJECT_DESC_TEST, 0, 0);
    check("descriptor: region revoke succeeds",
          region_revoke.error == (uintptr_t)object_error::OK,
          (unsigned long)region_revoke.error, 0);
#if defined(__riscv)
    check("descriptor: region revoke stales old grant descriptor",
          call_descriptor(changed_window_desc.value).error ==
              (uintptr_t)object_error::STALE_DESCRIPTOR,
          (unsigned long)call_descriptor(changed_window_desc.value).error,
          (unsigned long)object_error::STALE_DESCRIPTOR);
#endif
    const api_result revoked_window_desc =
        api(object_api::CREATE_METHOD_DESCRIPTOR, OBJECT_DESC_TEST, 1);
    check("descriptor: descriptor after revoke is created",
          revoked_window_desc.error == (uintptr_t)object_error::OK,
          (unsigned long)revoked_window_desc.error, 0);
    if (revoked_window_desc.error == (uintptr_t)object_error::OK)
      ++root_descriptors_created;
#if defined(__riscv)
    const api_result revoked_window_call = call_descriptor(revoked_window_desc.value);
    check("descriptor: current revoked window descriptor calls target",
          revoked_window_call.error == (uintptr_t)object_error::OK &&
              revoked_window_call.value == 21,
          (unsigned long)revoked_window_call.value, 21);
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
    const api_result legacy_abi = api(object_api::CALL_METHOD, OBJECT_DESC_TEST,
                                      2, 42);
    check("descriptor: legacy four-argument ABI probe returns 77",
          legacy_abi.error == (uintptr_t)object_error::OK && legacy_abi.value == 77,
          (unsigned long)legacy_abi.value, 77);
#if defined(__riscv)
    check("descriptor: legacy four-argument entry sees a4/a5 zero",
          g_desc_legacy_a4 == 0 && g_desc_legacy_a5 == 0,
          (unsigned long)(g_desc_legacy_a4 | g_desc_legacy_a5), 0);
    // 旧 4 引数 CALL_METHOD の経路 (CALL 段の積み込み = set_args) へ、呼出し側の a4/a5 に
    // ゴミを入れて撃つ。callee の a4/a5 は 0 のまま (呼出し側の値が届いてはならない)。
    g_desc_legacy_a4 = UINTPTR_MAX;
    g_desc_legacy_a5 = UINTPTR_MAX;
    const auto dirty = ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_DESC_TEST,
                                     2, 42, 0x5A5A5A5Au, 0xC3C3C3C3u, 0, 0);
    check("descriptor: legacy four-argument call with dirty caller a4/a5 returns 77",
          dirty.error == (uintptr_t)object_error::OK && dirty.value == 77,
          (unsigned long)dirty.value, 77);
    check("descriptor: dirty caller a4/a5 do not reach the four-argument callee",
          g_desc_legacy_a4 == 0 && g_desc_legacy_a5 == 0,
          (unsigned long)(g_desc_legacy_a4 | g_desc_legacy_a5), 0);
#endif

    // ---- 呼び出し元の識別: A (ルート) -> B (desc_relay) -> C (desc_relay_target, 記述子で直接呼出し) ----
    // 階層は A が B の親、B が C の親。各段は親→直接の子へ作る (kernel 役は使わない)。
    //   A→B: ルートが B を作る (B の creator は A)。 B→C: B が C を作る (C の creator は B)。
    //   B の表の C への記述子: 対象の creator が呼出し元なので一般規則で許可される。
    // ★C の g_desc_* に残る値が「B 経由で撃たれた」ものだと言えるように、先に空けておく。
    //   ルートは B しか呼ばず、B の表の記述子だけで C に届く。
    {
      // 診断専用 (挙動不変): 作成前後の値を shizuku_desc_relay_diag に残す。
      shizuku_desc_relay_diag[0] = 0xD1A60001u;
      const api_result relay_made = api(
          object_api::CREATE_OBJECT, OBJECT_DESC_RELAY, (uintptr_t)&desc_relay_fn);
      shizuku_desc_relay_diag[1] = (uint32_t)relay_made.error;
      shizuku_desc_relay_diag[2] = (uint32_t)relay_made.value;
      shizuku_desc_relay_diag[3] = (uint32_t)OBJECT_DESC_RELAY;
      shizuku_desc_relay_diag[4] = (uint32_t)kernel_instance.current_object(
          kernel_instance.current_thread_id());
      shizuku_desc_relay_diag[5] = kernel_instance.current_depth();
      shizuku_desc_relay_diag[6] = (uint32_t)KERNEL_OBJECT::OBJECT_COUNT;
      shizuku_desc_relay_diag[7] = 0xD1A60002u;
      check("descriptor A->B->C: create relay object",
            relay_made.error == (uintptr_t)object_error::OK,
            (unsigned long)relay_made.error, 0);

      g_desc_self = 0;
      g_desc_caller = 0;
#if defined(__riscv)
      g_relay_target_create_error = (uintptr_t)object_error::UNKNOWN_API;
      g_relay_desc_error = (uintptr_t)object_error::UNKNOWN_API;
#endif
      const auto root_depth_before = kernel_instance.current_depth();
      const api_result relay = call_method(OBJECT_DESC_RELAY, 0);
      check("descriptor A->B->C: root call into B returns OK",
            relay.error == (uintptr_t)object_error::OK,
            (unsigned long)relay.error, 0);
#if defined(__riscv)
      // B が C を作れたこと、B の表に C への記述子を作れたこと。
      check("descriptor A->B->C: B creates C (desc_relay_target)",
            g_relay_target_create_error == (uintptr_t)object_error::OK,
            (unsigned long)g_relay_target_create_error, 0);
      check("descriptor A->B->C: B creates descriptor for C",
            g_relay_desc_error == (uintptr_t)object_error::OK,
            (unsigned long)g_relay_desc_error, 0);
      // C の identity。B の中から撃たれたので、呼んだ側は B (ルートでも C 自身でもない)。
      check("descriptor A->B->C: C self is C (desc_relay_target)",
            g_desc_self == OBJECT_DESC_RELAY_TARGET,
            (unsigned long)g_desc_self, (unsigned long)OBJECT_DESC_RELAY_TARGET);
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
    uint32_t filled = root_descriptors_created;
    for (uint32_t i = filled; i < 16; ++i) {
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

  shizuku_descriptor_test_record.completed = 1;
  BOARD::diag_printf("[SELFTEST] call ladder done: %lu passed, %lu failed\n",
                     (unsigned long)passed, (unsigned long)failed);
}

} // namespace selftest
} // namespace shizuku
