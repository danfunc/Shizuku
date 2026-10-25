// ===========================================================================
//  RP2350 (ARMv8-M): 非特権 (U-mode) 同士の IPC コスト実測
// ===========================================================================
//  呼び手 P (親) も呼び先 C (子) も非特権 (CONTROL.nPRIV=1)。
//  P と C はそれぞれ専用の data 窓を持ち、MPU 張替えが入る別保護ドメインの組。
//  P は 2048 回の呼出しループを時間を読まずに回し、特権側で DWT CYCCNT を
//  用いて全体の経過サイクルを測定。svc なしの空ループ (baseline) を引いて
//  1 回あたりの net mean サイクル数を算出し [COST] 形式で出力する。

#include <cstdint>
#include <cstddef>
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"

// 親 P 用データ領域・スタック (32B 整列: ARMv8-M の MPU 境界 32B)
alignas(32) uint8_t u_cost_p_data_pool[256];
alignas(32) uint8_t u_cost_p_stack[1024];

// 子 C 用データ領域・スタック (32B 整列)
alignas(32) uint8_t u_cost_c_data_pool[256];
alignas(32) uint8_t u_cost_c_stack[1024];

// アセンブリシンボル
extern "C" {
uintptr_t u_cost_parent_worker(uintptr_t cmd, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
uintptr_t u_cost_child_worker(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
}

#if defined(__arm__) || defined(__thumb__)
__attribute__((naked)) uintptr_t u_cost_child_worker(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t) {
  asm volatile(
      ".syntax unified\n"
      ".thumb\n"
      "adds r0, r0, #1\n"
      "bx   lr\n"
  );
}

__attribute__((naked)) uintptr_t u_cost_parent_worker(uintptr_t cmd, uintptr_t, uintptr_t, uintptr_t) {
  asm volatile(
      ".syntax unified\n"
      ".thumb\n"
      "push {r4-r7, lr}\n"
      "ldr  r4, =u_cost_p_data_pool\n"
      "cmp  r0, #1\n"
      "beq  .Lp_cmd_create_child\n"
      "cmp  r0, #2\n"
      "beq  .Lp_cmd_baseline_method\n"
      "cmp  r0, #3\n"
      "beq  .Lp_cmd_measure_method\n"
      "cmp  r0, #4\n"
      "beq  .Lp_cmd_create_desc\n"
      "cmp  r0, #5\n"
      "beq  .Lp_cmd_baseline_desc\n"
      "cmp  r0, #6\n"
      "beq  .Lp_cmd_measure_desc\n"
      "movs r0, #0\n"
      "pop  {r4-r7, pc}\n"

      // cmd=1: 子 C (OBJECT_U_COST_C) を CREATE_OBJECT
      ".Lp_cmd_create_child:\n"
      "ldr  r1, [r4, #0]\n"   // child_id
      "ldr  r2, [r4, #4]\n"   // callee_entry
      "movs r0, #1\n"         // CREATE_OBJECT = 1
      "movs r3, #2\n"         // OBJECT_UNPRIVILEGED = 2 (1u << 1)
      "mov  r12, #0\n"
      "svc  0\n"
      "pop  {r4-r7, pc}\n"

      // cmd=2: method baseline 測定 (2048 回、svc なし空ループ)
      ".Lp_cmd_baseline_method:\n"
      "ldr  r7, [r4, #12]\n"  // rounds (2048)
      "ldr  r6, [r4, #0]\n"   // child_id
      // warm-up 32 回
      "movs r5, #32\n"
      ".Lbase_meth_warm:\n"
      "movs r0, #4\n"         // CALL_METHOD = 4
      "mov  r1, r6\n"
      "movs r2, #0\n"
      "movs r3, #0\n"
      "mov  r12, #0\n"
      "subs r5, r5, #1\n"
      "bne  .Lbase_meth_warm\n"
      // 本番 2048 回 (空ループ)
      "mov  r5, r7\n"
      ".Lbase_meth_loop:\n"
      "movs r0, #4\n"
      "mov  r1, r6\n"
      "movs r2, #0\n"
      "movs r3, #0\n"
      "mov  r12, #0\n"
      "subs r5, r5, #1\n"
      "bne  .Lbase_meth_loop\n"
      "str  r7, [r4, #16]\n"  // baseline_meth_rounds_executed
      "mov  r0, r7\n"         // return rounds (2048)
      "pop  {r4-r7, pc}\n"

      // cmd=3: method call 測定 (2048 回、svc あり、時間計測なし)
      ".Lp_cmd_measure_method:\n"
      "ldr  r7, [r4, #12]\n"  // rounds (2048)
      "ldr  r6, [r4, #0]\n"   // child_id
      // warm-up 32 回
      "movs r5, #32\n"
      ".Lmeth_warm:\n"
      "movs r0, #4\n"
      "mov  r1, r6\n"
      "movs r2, #0\n"
      "movs r3, #0\n"
      "mov  r12, #0\n"
      "svc  0\n"
      "subs r5, r5, #1\n"
      "bne  .Lmeth_warm\n"
      // 本番 2048 回 (svc あり)
      "mov  r5, r7\n"
      ".Lmeth_loop:\n"
      "movs r0, #4\n"
      "mov  r1, r6\n"
      "movs r2, #0\n"
      "movs r3, #0\n"
      "mov  r12, #0\n"
      "svc  0\n"
      "subs r5, r5, #1\n"
      "bne  .Lmeth_loop\n"
      "str  r7, [r4, #20]\n"  // meth_rounds_executed
      // 検算: arg=41 で呼んで 42 が返ることを確認
      "movs r0, #4\n"
      "mov  r1, r6\n"
      "movs r2, #0\n"
      "movs r3, #41\n"
      "mov  r12, #0\n"
      "svc  0\n"
      "str  r1, [r4, #40]\n"  // meth_check_val
      "str  r0, [r4, #44]\n"  // meth_check_err
      "mov  r0, r7\n"         // return rounds (2048)
      "pop  {r4-r7, pc}\n"

      // cmd=4: descriptor 作成 (CREATE_METHOD_DESCRIPTOR = 32)
      ".Lp_cmd_create_desc:\n"
      "ldr  r1, [r4, #0]\n"   // child_id
      "movs r0, #32\n"        // CREATE_METHOD_DESCRIPTOR = 32
      "movs r2, #0\n"
      "movs r3, #0\n"
      "mov  r12, #0\n"
      "svc  0\n"
      "str  r1, [r4, #8]\n"   // desc_index
      "pop  {r4-r7, pc}\n"

      // cmd=5: descriptor baseline 測定 (2048 回、svc なし空ループ)
      ".Lp_cmd_baseline_desc:\n"
      "ldr  r7, [r4, #12]\n"  // rounds (2048)
      "ldr  r6, [r4, #8]\n"   // desc_index
      "movs r5, #32\n"
      ".Lbase_desc_warm:\n"
      "movs r0, #33\n"        // CALL_METHOD_DESCRIPTOR = 33
      "mov  r1, r6\n"
      "movs r2, #0\n"
      "movs r3, #0\n"
      "mov  r12, #0\n"
      "subs r5, r5, #1\n"
      "bne  .Lbase_desc_warm\n"
      // 本番 2048 回
      "mov  r5, r7\n"
      ".Lbase_desc_loop:\n"
      "movs r0, #33\n"
      "mov  r1, r6\n"
      "movs r2, #0\n"
      "movs r3, #0\n"
      "mov  r12, #0\n"
      "subs r5, r5, #1\n"
      "bne  .Lbase_desc_loop\n"
      "str  r7, [r4, #24]\n"  // baseline_desc_rounds_executed
      "mov  r0, r7\n"
      "pop  {r4-r7, pc}\n"

      // cmd=6: descriptor call 測定 (2048 回、svc あり、時間計測なし)
      ".Lp_cmd_measure_desc:\n"
      "ldr  r7, [r4, #12]\n"  // rounds (2048)
      "ldr  r6, [r4, #8]\n"   // desc_index
      // warm-up 32 回
      "movs r5, #32\n"
      ".Ldesc_warm:\n"
      "movs r0, #33\n"
      "mov  r1, r6\n"
      "movs r2, #0\n"
      "movs r3, #0\n"
      "mov  r12, #0\n"
      "svc  0\n"
      "subs r5, r5, #1\n"
      "bne  .Ldesc_warm\n"
      // 本番 2048 回
      "mov  r5, r7\n"
      ".Ldesc_loop:\n"
      "movs r0, #33\n"
      "mov  r1, r6\n"
      "movs r2, #0\n"
      "movs r3, #0\n"
      "mov  r12, #0\n"
      "svc  0\n"
      "subs r5, r5, #1\n"
      "bne  .Ldesc_loop\n"
      "str  r7, [r4, #28]\n"  // desc_rounds_executed
      // 検算: arg=41 で呼んで 42 が返ることを確認
      "movs r0, #33\n"
      "mov  r1, r6\n"
      "movs r2, #41\n"
      "movs r3, #0\n"
      "mov  r12, #0\n"
      "svc  0\n"
      "str  r1, [r4, #60]\n"  // desc_check_val
      "str  r0, [r4, #64]\n"  // desc_check_err
      "mov  r0, r7\n"
      "pop  {r4-r7, pc}\n"
      ".ltorg\n"
  );
}
#else
// ホスト環境用スタブ
extern "C" {
uintptr_t u_cost_parent_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
uintptr_t u_cost_child_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
}
#endif

namespace shizuku::selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_U_COST_P = object_id::cost_u_parent;
constexpr uintptr_t OBJECT_U_COST_C = object_id::cost_u_child;
constexpr uint32_t ROUNDS = 2048;

template <typename A> inline uint32_t cycles_now() {
#if defined(__arm__) || defined(__thumb__)
  __asm__ volatile("dmb" ::: "memory");
  const uint32_t cycles = A::cycle_counter_read();
  __asm__ volatile("dmb" ::: "memory");
  return cycles;
#elif defined(__riscv)
  __asm__ volatile("fence" ::: "memory");
  const uint32_t cycles = A::cycle_counter_read();
  __asm__ volatile("fence" ::: "memory");
  return cycles;
#else
  return 0;
#endif
}

struct shizuku_rp2350_u_cost_record_t {
  uint32_t magic;                 // 0x55554353 ("UUCS")
  uint32_t version;               // 2
  uint32_t rounds;                // 2048
  uint32_t baseline_method_mean;  // baseline mean for method call
  uint32_t baseline_desc_mean;    // baseline mean for desc call
  uint32_t method_raw_mean;
  uint32_t method_net_mean;
  uint32_t desc_raw_mean;
  uint32_t desc_net_mean;
  uint32_t checked_ok;
};
static_assert(sizeof(shizuku_rp2350_u_cost_record_t) == 40, "shizuku_rp2350_u_cost_record_t size");

volatile shizuku_rp2350_u_cost_record_t shizuku_rp2350_u_cost_record = {};
volatile uint32_t shizuku_rp2350_u_cost_stage = 0;

void record_check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[COST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    record_fail(name, got, want);
    BOARD::diag_printf("[COST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

} // namespace

void rp2350_u_cost() {
#if defined(__ARM_ARCH_8M_MAIN__) || defined(SHIZUKU_RP2350)
  if constexpr (ARCH::HAS_CYCLE_COUNTER) {
    ARCH::cycle_counter_enable();

    // 1. 親 P と子 C の保護窓を登録
    if constexpr (requires { ARCH::set_object_bounds(0, 0, 0, 0, 0); }) {
      ARCH::set_object_bounds(
          OBJECT_U_COST_P,
          reinterpret_cast<uintptr_t>(u_cost_p_data_pool),
          reinterpret_cast<uintptr_t>(u_cost_p_data_pool) + sizeof(u_cost_p_data_pool),
          0, 0);
      ARCH::set_object_bounds(
          OBJECT_U_COST_C,
          reinterpret_cast<uintptr_t>(u_cost_c_data_pool),
          reinterpret_cast<uintptr_t>(u_cost_c_data_pool) + sizeof(u_cost_c_data_pool),
          0, 0);
    }

    // 2. スタックのカナリア初期化
    constexpr uint32_t U_STACK_CANARY = 0x5A5A5A5Au;
    auto *p_stack_words = reinterpret_cast<uint32_t *>(u_cost_p_stack);
    for (size_t i = 0; i < sizeof(u_cost_p_stack) / sizeof(uint32_t); ++i) {
      p_stack_words[i] = U_STACK_CANARY;
    }
    auto *c_stack_words = reinterpret_cast<uint32_t *>(u_cost_c_stack);
    for (size_t i = 0; i < sizeof(u_cost_c_stack) / sizeof(uint32_t); ++i) {
      c_stack_words[i] = U_STACK_CANARY;
    }

    // 3. 共有データ窓の初期化
    auto *shared = reinterpret_cast<volatile uint32_t *>(u_cost_p_data_pool);
    for (size_t i = 0; i < sizeof(u_cost_p_data_pool) / sizeof(uint32_t); ++i) {
      shared[i] = 0;
    }
    shared[0] = OBJECT_U_COST_C;
    shared[1] = reinterpret_cast<uintptr_t>(&u_cost_child_worker);
    shared[2] = 0; // desc_index
    shared[3] = ROUNDS; // 2048

    // 4. 親 P を CREATE_OBJECT (非特権フラグ OBJECT_UNPRIVILEGED=2 を指定)
    shizuku_rp2350_u_cost_stage = 0xC05010u;
    const auto create_p_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CREATE_OBJECT),
        OBJECT_U_COST_P,
        reinterpret_cast<uintptr_t>(&u_cost_parent_worker),
        OBJECT_UNPRIVILEGED);
    if (create_p_res.error != 0) {
      BOARD::panic("rp2350_u_cost: failed to create parent P");
    }

    // 5. 親 P に cmd=1 (子 C 作成) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05020u;
    const auto create_c_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P,
        0,
        1 /* cmd=1 */);
    if (create_c_res.error != 0) {
      BOARD::panic("rp2350_u_cost: failed to create child C via P");
    }

    // 6. 親 P に cmd=2 (baseline method) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05030u;
    const uint32_t t_base_meth_start = cycles_now<ARCH>();
    const auto base_meth_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P,
        0,
        2 /* cmd=2: baseline method */);
    const uint32_t t_base_meth_end = cycles_now<ARCH>();
    const uint32_t base_meth_cycles = t_base_meth_end - t_base_meth_start;

    // 7. 親 P に cmd=3 (measure method) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05040u;
    const uint32_t t_meth_start = cycles_now<ARCH>();
    const auto meth_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P,
        0,
        3 /* cmd=3: measure method */);
    const uint32_t t_meth_end = cycles_now<ARCH>();
    const uint32_t meth_cycles = t_meth_end - t_meth_start;

    // 8. 親 P に cmd=4 (create desc) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05050u;
    const auto desc_create_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P,
        0,
        4 /* cmd=4: create desc */);

    // 9. 親 P に cmd=5 (baseline desc) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05060u;
    const uint32_t t_base_desc_start = cycles_now<ARCH>();
    const auto base_desc_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P,
        0,
        5 /* cmd=5: baseline desc */);
    const uint32_t t_base_desc_end = cycles_now<ARCH>();
    const uint32_t base_desc_cycles = t_base_desc_end - t_base_desc_start;

    // 10. 親 P に cmd=6 (measure desc) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05070u;
    const uint32_t t_desc_start = cycles_now<ARCH>();
    const auto desc_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P,
        0,
        6 /* cmd=6: measure desc */);
    const uint32_t t_desc_end = cycles_now<ARCH>();
    const uint32_t desc_cycles = t_desc_end - t_desc_start;

    // ループ完走の検証
    if (base_meth_res.error != 0 || base_meth_res.value != ROUNDS || shared[4] != ROUNDS) {
      BOARD::diag_printf("[COST] WARN baseline method mismatch: err=%lu res=%lu shared=%lu want=%lu\n",
                         (unsigned long)base_meth_res.error,
                         (unsigned long)base_meth_res.value,
                         (unsigned long)shared[4],
                         (unsigned long)ROUNDS);
    }
    if (meth_res.error != 0 || meth_res.value != ROUNDS || shared[5] != ROUNDS) {
      BOARD::diag_printf("[COST] WARN measure method mismatch: err=%lu res=%lu shared=%lu want=%lu\n",
                         (unsigned long)meth_res.error,
                         (unsigned long)meth_res.value,
                         (unsigned long)shared[5],
                         (unsigned long)ROUNDS);
    }
    if (base_desc_res.error != 0 || base_desc_res.value != ROUNDS || shared[6] != ROUNDS) {
      BOARD::diag_printf("[COST] WARN baseline desc mismatch: err=%lu res=%lu shared=%lu want=%lu\n",
                         (unsigned long)base_desc_res.error,
                         (unsigned long)base_desc_res.value,
                         (unsigned long)shared[6],
                         (unsigned long)ROUNDS);
    }
    if (desc_res.error != 0 || desc_res.value != ROUNDS || shared[7] != ROUNDS) {
      BOARD::diag_printf("[COST] WARN measure desc mismatch: err=%lu res=%lu shared=%lu want=%lu\n",
                         (unsigned long)desc_res.error,
                         (unsigned long)desc_res.value,
                         (unsigned long)shared[7],
                         (unsigned long)ROUNDS);
    }

    // 結果の集計と表示
    const uint32_t meth_val = shared[10]; // offset 40
    const uint32_t meth_err = shared[11]; // offset 44

    const uint32_t desc_val = shared[15]; // offset 60
    const uint32_t desc_err = shared[16]; // offset 64

    const uint32_t base_meth_mean = base_meth_cycles / ROUNDS;
    const uint32_t meth_raw_mean = meth_cycles / ROUNDS;
    const uint32_t net_meth_mean = meth_cycles > base_meth_cycles
                                       ? (meth_cycles - base_meth_cycles) / ROUNDS
                                       : 0;

    const uint32_t base_desc_mean = base_desc_cycles / ROUNDS;
    const uint32_t desc_raw_mean = desc_cycles / ROUNDS;
    const uint32_t net_desc_mean = desc_cycles > base_desc_cycles
                                       ? (desc_cycles - base_desc_cycles) / ROUNDS
                                       : 0;

    BOARD::diag_printf(
        "[COST] %-24s net mean %5lu cyc  [raw mean %lu, baseline mean %lu]\n",
        "U->U method call (1 hop)",
        (unsigned long)net_meth_mean,
        (unsigned long)meth_raw_mean,
        (unsigned long)base_meth_mean);
    BOARD::diag_printf(
        "[COST] %-24s net mean %5lu cyc  [raw mean %lu, baseline mean %lu]\n",
        "U->U desc call (1 hop)",
        (unsigned long)net_desc_mean,
        (unsigned long)desc_raw_mean,
        (unsigned long)base_desc_mean);

    // RAM レコードへの記録
    shizuku_rp2350_u_cost_record.magic = 0x55554353u; // "UUCS"
    shizuku_rp2350_u_cost_record.version = 2;
    shizuku_rp2350_u_cost_record.rounds = ROUNDS;
    shizuku_rp2350_u_cost_record.baseline_method_mean = base_meth_mean;
    shizuku_rp2350_u_cost_record.baseline_desc_mean = base_desc_mean;
    shizuku_rp2350_u_cost_record.method_raw_mean = meth_raw_mean;
    shizuku_rp2350_u_cost_record.method_net_mean = net_meth_mean;
    shizuku_rp2350_u_cost_record.desc_raw_mean = desc_raw_mean;
    shizuku_rp2350_u_cost_record.desc_net_mean = net_desc_mean;

    // PASS/FAIL アサーション
    const bool meth_ok = (meth_err == 0 && meth_val == 42);
    record_check("u_cost: U->U method call returns 42", meth_ok, meth_val, 42);

    const bool desc_ok = (desc_err == 0 && desc_val == 42);
    record_check("u_cost: U->U descriptor call returns 42", desc_ok, desc_val, 42);

    shizuku_rp2350_u_cost_record.checked_ok = (meth_ok && desc_ok) ? 1u : 0u;
    shizuku_rp2350_u_cost_stage = 0xC050FFu;
  }
#endif
}

} // namespace shizuku::selftest
