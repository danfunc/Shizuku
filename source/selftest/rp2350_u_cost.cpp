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
#include "shizuku/u_cost_invariants.hpp"
#include "shizuku/u_cost_sram_window.hpp"
#include "shizuku/xip_cost.hpp"
#include "shizuku/armv8m_abi.hpp"
#if defined(SHIZUKU_DIAG_FP)
#include "shizuku/diag_fp.hpp"
#endif

// --//configs:layout_pad_bytes=N の詰め物。flash のコードと .bss を N バイト動かして、
// 計測値が配置に依らないことを確かめる (0 なら何も足さない)。pad は pool より前に並べる。
__attribute__((used, no_reorder, section(".text.u_cost_layout_pad")))
const uint8_t u_cost_layout_pad_flash[shizuku::layout_pad_bytes] = {};
__attribute__((used, no_reorder, section(".bss.u_cost_data")))
volatile uint8_t u_cost_layout_pad_bss[shizuku::layout_pad_bytes];

// P/C の可変データと probe 用領域はコード実行窓から分けた専用 SRAM section に置く。
// 各 object が使う data は従来どおり個別の MPU 窓でだけ開示する。
alignas(32) __attribute__((used, no_reorder, section(".bss.u_cost_data")))
uint8_t u_cost_p_data_pool[256];
alignas(32) __attribute__((used, no_reorder, section(".bss.u_cost_data")))
uint8_t u_cost_p_stack[1024];

// 子 C 用データ領域・スタック (32B 整列)
alignas(32) __attribute__((used, no_reorder, section(".bss.u_cost_data")))
uint8_t u_cost_c_data_pool[256];
alignas(32) __attribute__((used, no_reorder, section(".bss.u_cost_data")))
uint8_t u_cost_c_stack[1024];

// U 側のコード (P / C 本体と converter・svc stub・計測ループ) は 1 本の SRAM section
// `.time_critical.u_cost` に置く。flash の XIP キャッシュでカーネルと衝突して振れるのを避けるため。
// begin / end は同じ section の中のマーカーで、window は 32B 境界の [begin, end)。
// no_reorder が section 内の並び (begin → C → P → end) を固定する。
// 非特権に実行を開ける SRAM はこの範囲だけ (rp2350_u_cost() が region 1 に RO+X で張る)。
extern "C" {
uintptr_t u_cost_parent_worker(uintptr_t cmd, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
uintptr_t u_cost_parent_worker_fp(uintptr_t cmd, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
uintptr_t u_cost_child_worker(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
void u_cost_code_begin();
void u_cost_code_end_pad();
extern const char u_cost_code_end[];
void u_cost_probe_code_access();
void u_cost_probe_data_access();
uintptr_t u_cost_probe_sp_spoof_child(uintptr_t target);
uintptr_t u_cost_probe_stream_caller_stack();
void u_cost_denied_code();
extern const char u_cost_probe_data_fault_pc[];
}

#if defined(__ARM_ARCH_8M_MAIN__)
#define SHIZUKU_U_COST_CODE \
  __attribute__((section(".time_critical.u_cost"), used, noinline, no_reorder))

SHIZUKU_U_COST_CODE __attribute__((naked, aligned(32))) void u_cost_code_begin() {
  asm volatile(".syntax unified\n.thumb\nnop\n");
}

SHIZUKU_U_COST_CODE __attribute__((naked)) uintptr_t u_cost_child_worker(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t) {
  asm volatile(
      ".syntax unified\n"
      ".thumb\n"
      "adds r0, r0, #1\n"
      "bx   lr\n"
  );
}

// These two probes stay inside the allowed code window, then deliberately cross it.
SHIZUKU_U_COST_CODE __attribute__((naked)) void u_cost_probe_code_access() {
  asm volatile(".syntax unified\n.thumb\nldr r0, =u_cost_denied_code\nbx r0\n.ltorg\n");
}

SHIZUKU_U_COST_CODE __attribute__((naked)) void u_cost_probe_data_access() {
  asm volatile(
      ".syntax unified\n.thumb\n"
      "ldr r0, =u_cost_p_stack\n"
      ".global u_cost_probe_data_fault_pc\n"
      "u_cost_probe_data_fault_pc:\n"
      "ldr r0, [r0]\n"
      "bx lr\n"
      ".ltorg\n");
}

// Registered only after timing so the measured child worker stays two instructions.
SHIZUKU_U_COST_CODE __attribute__((naked)) uintptr_t u_cost_probe_sp_spoof_child(uintptr_t target) {
  asm volatile(
      ".syntax unified\n"
      ".thumb\n"
      "push {r4, lr}\n"
      "mov  r1, r0\n"                  // target object in r1
      "add  sp, sp, #128\n"            // SP 偽装: caller 枠内へ
      "movs r0, #4\n"                  // CALL_METHOD (4)
      "movs r2, #0\n"                  // method 0
      "movs r3, #0\n"
      "movs r12, #0\n"
      "svc  #0\n"
      "sub  sp, sp, #128\n"            // SP 復元
      "cmp  r0, #6\n"                  // object_error::NO_STACK == 6
      "bne  1f\n"
      "movs r0, #0\n"
      "pop  {r4, pc}\n"
      "1:\n"
      "cmp  r0, #0\n"
      "bne  2f\n"
      "ldr  r0, =0xDEAD0038\n"
      "2:\n"
      "pop  {r4, pc}\n"
      ".ltorg\n"
  );
}

SHIZUKU_U_COST_CODE __attribute__((naked)) uintptr_t u_cost_probe_stream_caller_stack() {
  asm volatile(
      ".syntax unified\n"
      ".thumb\n"
      "push {r4, lr}\n"

      // 1. STREAM_PUSH_N (29) with request_ptr = sp + 64 (in caller frame)
      "movs r0, #29\n"                 // STREAM_PUSH_N
      "movs r1, #0\n"                  // stream id 0
      "add  r2, sp, #64\n"             // request_ptr in caller frame
      "movs r3, #0\n"
      "movs r12, #0\n"
      "svc  #0\n"
      "cmp  r0, #15\n"                 // object_error::BAD_STREAM == 15
      "bne  1f\n"

      // 2. STREAM_PUSH_N (29) with records = sp + 64 (in caller frame)
      "add  r1, sp, #64\n"
      "str  r1, [sp, #-16]!\n"          // push_request.records
      "movs r1, #1\n"
      "str  r1, [sp, #4]\n"            // push_request.count = 1
      "movs r0, #29\n"
      "movs r1, #0\n"
      "mov  r2, sp\n"                  // request_ptr on callee stack
      "movs r3, #0\n"
      "movs r12, #0\n"
      "svc  #0\n"
      "add  sp, sp, #16\n"
      "cmp  r0, #15\n"
      "bne  2f\n"

      // 3. STREAM_POP_N (30) with records = sp + 64 (in caller frame)
      "add  r1, sp, #64\n"
      "str  r1, [sp, #-16]!\n"          // pop_request.records
      "movs r1, #1\n"
      "str  r1, [sp, #4]\n"            // pop_request.count = 1
      "movs r1, #0\n"
      "str  r1, [sp, #8]\n"            // pop_request.lost = 0
      "movs r0, #30\n"                 // STREAM_POP_N
      "movs r1, #0\n"
      "mov  r2, sp\n"                  // request_ptr on callee stack
      "movs r3, #0\n"
      "movs r12, #0\n"
      "svc  #0\n"
      "add  sp, sp, #16\n"
      "cmp  r0, #15\n"
      "bne  3f\n"

      // 4. STREAM_POP_N (30) with request_ptr = sp + 64 (in caller frame)
      "movs r0, #30\n"                 // STREAM_POP_N
      "movs r1, #0\n"
      "add  r2, sp, #64\n"             // request_ptr in caller frame
      "movs r3, #0\n"
      "movs r12, #0\n"
      "svc  #0\n"
      "cmp  r0, #15\n"
      "bne  4f\n"

      "movs r0, #0\n"
      "pop  {r4, pc}\n"

      "1:\n"
      "ldr  r0, =0xDEAD003A\n"
      "pop  {r4, pc}\n"

      "2:\n"
      "ldr  r0, =0xDEAD003B\n"
      "pop  {r4, pc}\n"

      "3:\n"
      "ldr  r0, =0xDEAD003C\n"
      "pop  {r4, pc}\n"

      "4:\n"
      "ldr  r0, =0xDEAD003D\n"
      "pop  {r4, pc}\n"
      ".ltorg\n"
  );
}

SHIZUKU_U_COST_CODE uintptr_t u_cost_denied_probe_worker(uintptr_t mode, uintptr_t,
                                                        uintptr_t, uintptr_t) {
  if (mode == 90) u_cost_probe_code_access();
  if (mode == 91) u_cost_probe_data_access();
  if (mode == 92) return u_cost_probe_sp_spoof_child(shizuku::object_id::cost_u_parent);
  if (mode == 93) return u_cost_probe_stream_caller_stack();
  return 0;
}

// This function is intentionally in a separate SRAM input section and is never called
// by privileged code. The U probe attempts to execute it while only u_cost is enabled.
__attribute__((used, noinline, aligned(32), section(".time_critical.u_cost_denied"), no_reorder, naked))
void u_cost_denied_code() {
  asm volatile(".syntax unified\n.thumb\nbx lr\n");
}

SHIZUKU_U_COST_CODE uintptr_t u_cost_parent_worker_fp(uintptr_t cmd, uintptr_t, uintptr_t, uintptr_t);

SHIZUKU_U_COST_CODE uintptr_t u_cost_parent_worker(uintptr_t cmd, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {
  if (cmd >= 20) return u_cost_parent_worker_fp(cmd, arg1, arg2, arg3);
  using shizuku::armv8m_abi::syscall;
  auto *const data = reinterpret_cast<volatile uint32_t *>(u_cost_p_data_pool);
  const uintptr_t child = data[0];
  const uintptr_t rounds = data[3];
  if (cmd == 1) return syscall(1, child, data[1], 2).error;
  if (cmd == 4) {
    const auto result = syscall(32, child);
    data[2] = result.value;
    return result.error;
  }
  const bool descriptor = cmd == 5 || cmd == 6 || cmd == 11 || cmd >= 15;
  const uintptr_t target = descriptor ? data[2] : child;
  const uintptr_t number = descriptor ? 33 : 4;
  auto call = [&](uintptr_t value) __attribute__((always_inline)) {
    return descriptor ? syscall(number, target, value) : syscall(number, target, 0, value);
  };
  if (cmd == 14 || cmd == 17) {
    const auto result = call(41);
    data[descriptor ? 15 : 10] = result.value;
    data[descriptor ? 16 : 11] = result.error;
    return 1;
  }
  if (cmd == 10 || cmd == 11) {
    for (uintptr_t i = 0; i < 32; ++i) (void)call(0);
    return 32;
  }
  if (cmd < 2 || cmd > 16 || cmd == 7 || cmd == 8 || cmd == 9) {
    // This cold command is used only after the timing runs. It adds no work to the
    // normal measured parent command paths.
    if (cmd == 18)
      return syscall(static_cast<uintptr_t>(shizuku::object_api::EXPORT_METHOD), 1,
                     reinterpret_cast<uintptr_t>(&u_cost_denied_probe_worker)).error;
    if (cmd == 8) {
      volatile uint32_t sentinel = 0x5F01CAFE;
      const auto r = syscall(4, shizuku::object_id::cost_u_parent, 1, 92);
      if (r.error != 0 || r.value != 0) return r.value != 0 ? r.value : 0xDEAD0038;
      if (sentinel != 0x5F01CAFE) return 0xDEAD0037;
      return 0;
    }
    if (cmd == 9) {
      volatile uint32_t sentinel = 0x5F01CAFE;
      const auto r = syscall(4, shizuku::object_id::cost_u_parent, 1, 93);
      if (r.error != 0 || r.value != 0) return r.value != 0 ? r.value : 0xDEAD003A;
      if (sentinel != 0x5F01CAFE) return 0xDEAD0037;
      return 0;
    }
    return 0;
  }
  const bool baseline = cmd == 2 || cmd == 5 || cmd == 12 || cmd == 15;
  const bool corrected = cmd >= 12;
  if (!corrected) {
    for (uintptr_t i = 0; i < 32; ++i) {
      if (!baseline) (void)call(0);
      else asm volatile("" : : "r"(number), "r"(target) : "memory");
    }
  }
  uintptr_t value = 0, error = 0;
  for (uintptr_t i = 0; i < rounds; ++i) {
    if (baseline) {
      if (corrected) ++value;
      asm volatile("" : "+r"(value), "+r"(error) : "r"(number), "r"(target) : "memory");
    } else {
      const auto result = call(corrected ? value : 0);
      value = result.value;
      error |= result.error;
    }
  }
  if (corrected) {
    const unsigned index = descriptor ? (baseline ? 24 : 27) : (baseline ? 18 : 21);
    data[index] = value;
    data[index + 1] = error;
    data[index + 2] = rounds;
  } else {
    data[descriptor ? (baseline ? 6 : 7) : (baseline ? 4 : 5)] = rounds;
    if (!baseline) {
      const auto result = call(41);
      data[descriptor ? 15 : 10] = result.value;
      data[descriptor ? 16 : 11] = result.error;
    }
  }
  return rounds;
}

SHIZUKU_U_COST_CODE uintptr_t u_cost_parent_worker_fp(uintptr_t cmd, uintptr_t, uintptr_t, uintptr_t) {
  using shizuku::armv8m_abi::syscall;
  auto *const data = reinterpret_cast<volatile uint32_t *>(u_cost_p_data_pool);
  const uintptr_t child = data[0];
  const uintptr_t rounds = data[3];

  uint32_t ctrl_b = 0;
  asm volatile("mrs %0, control" : "=r"(ctrl_b));
  const uint32_t fpca_before = (ctrl_b >> 2) & 1u;

  const uint32_t sub_cmd = static_cast<uint32_t>(cmd % 10);
  const bool descriptor = sub_cmd == 1 || sub_cmd >= 5;
  const uintptr_t target = descriptor ? data[2] : child;
  const uintptr_t number = descriptor ? 33 : 4;
  auto call = [&](uintptr_t value) __attribute__((always_inline)) {
    return descriptor ? syscall(number, target, value) : syscall(number, target, 0, value);
  };

  if (sub_cmd == 4 || sub_cmd == 7) {
    const auto result = call(41);
    data[descriptor ? 52 : 50] = result.value;
    data[descriptor ? 53 : 51] = result.error;
    return 1;
  }
  if (sub_cmd == 0 || sub_cmd == 1) {
    for (uintptr_t i = 0; i < 32; ++i) (void)call(0);
    return 32;
  }

  const bool baseline = (sub_cmd == 2 || sub_cmd == 5);
  uintptr_t value = 0, error = 0;
  for (uintptr_t i = 0; i < rounds; ++i) {
    if (baseline) {
      ++value;
      asm volatile("" : "+r"(value), "+r"(error) : "r"(number), "r"(target) : "memory");
    } else {
      const auto result = call(value);
      value = result.value;
      error |= result.error;
    }
  }

  uint32_t ctrl_a = 0;
  asm volatile("mrs %0, control" : "=r"(ctrl_a));
  const uint32_t fpca_after = (ctrl_a >> 2) & 1u;

  const unsigned base_idx = descriptor ? (baseline ? 40 : 45) : (baseline ? 30 : 35);
  data[base_idx + 0] = fpca_before;
  data[base_idx + 1] = fpca_after;
  data[base_idx + 2] = value;
  data[base_idx + 3] = error;
  data[base_idx + 4] = rounds;

  return rounds;
}

// 最後のliteral poolを窓内にflushしてから32B境界へalignし、window終端を置く。
SHIZUKU_U_COST_CODE __attribute__((naked)) void u_cost_code_end_pad() {
  asm volatile(".ltorg\n.balign 32\n.global u_cost_code_end\nu_cost_code_end:\n");
}

#else
// ホスト環境用スタブ
extern "C" {
uintptr_t u_cost_parent_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
uintptr_t u_cost_parent_worker_fp(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
uintptr_t u_cost_child_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
void u_cost_code_begin() {}
void u_cost_code_end_pad() {}
const char u_cost_code_end[1] = {};
}
#endif

namespace shizuku::selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_U_COST_P = object_id::cost_u_parent;
constexpr uintptr_t OBJECT_U_COST_C = object_id::cost_u_child;
constexpr uint32_t ROUNDS = 2048;

// 例外の入口別カウンタ (fault 各種と SysTick) の前後差。DWT 窓の外で読むだけで、
// SVC/PendSV には何も足さない。常時有効 (配置を動かす probe 切替えにしない)。
struct exception_window {
  KERNEL::exception_counts before, after;
};
struct exception_windows {
  static inline exception_window windows[8] = {};
  static void begin(unsigned i) { windows[i].before = kernel_instance.exception_snapshot(); }
  static void end(unsigned i) { windows[i].after = kernel_instance.exception_snapshot(); }
  // 飽和 (UINT32_MAX) や巻き戻りは差を信用しない。
  static void report(unsigned i, const char *name) {
    const auto &w = windows[i];
    auto delta = [](uint32_t b, uint32_t a, bool &valid) -> unsigned long {
      if (b == UINT32_MAX || a == UINT32_MAX || a < b) { valid = false; return 0; }
      return a - b;
    };
    bool valid = true;
    const unsigned long mm = delta(w.before.memmanage, w.after.memmanage, valid);
    const unsigned long bf = delta(w.before.busfault, w.after.busfault, valid);
    const unsigned long uf = delta(w.before.usagefault, w.after.usagefault, valid);
    const unsigned long hf = delta(w.before.hardfault, w.after.hardfault, valid);
    const unsigned long st = delta(w.before.systick, w.after.systick, valid);
    const unsigned long fc = delta(w.before.faults, w.after.faults, valid);
    BOARD::diag_printf("[FAULTS] %s memmanage=%lu busfault=%lu usagefault=%lu "
                       "hardfault=%lu systick=%lu faults_total=%lu valid=%u\n",
                       name, mm, bf, uf, hf, st, fc, (unsigned)valid);
    []<class B = BOARD>() { if constexpr (requires { B::diag_drain(); }) B::diag_drain(); }();
  }
};


// Snapshots sit outside the DWT interval. Storage is only referenced when the
// opt-in route probe is enabled; individual bucket reads can be interrupted.
struct route_window {
  cost_probe::route_snapshot before, after;
  unsigned before_core, after_core;
};
template<bool Enabled> struct route_windows {
  static inline route_window windows[8] = {};
  template<class A> static void begin(unsigned i) {
    exception_windows::begin(i); // 例外カウンタは probe 設定と無関係に常時
    if constexpr (Enabled) {
      auto &w = windows[i];
      w.before_core = typename A::core_num{}.index();
      w.before = cost_probe::routes<true>::snapshot(w.before_core);
    }
  }
  template<class A> static void end(unsigned i) {
    exception_windows::end(i);
    if constexpr (Enabled) {
      auto &w = windows[i];
      w.after_core = typename A::core_num{}.index();
      w.after = cost_probe::routes<true>::snapshot(w.after_core);
    }
  }
  template<class B> static void report(unsigned i, const char *name) {
    exception_windows::report(i, name);
    if constexpr (Enabled) {
      const auto &w = windows[i];
      const bool same_core = w.before_core == w.after_core;
      B::diag_printf("[ROUTE] %s core_before=%u core_after=%u same_core=%u\n",
                     name, w.before_core, w.after_core, (unsigned)same_core);
      if constexpr (requires { B::diag_drain(); }) B::diag_drain();
      for (unsigned api = 0; api < cost_probe::bucket_count; ++api) {
        const auto d = cost_probe::difference(w.before.direct[api], w.after.direct[api]);
        const auto g = cost_probe::difference(w.before.generic[api], w.after.generic[api]);
        if (d.value || g.value || !d.valid || !g.valid || !same_core) {
          B::diag_printf("[ROUTE] %s api=%u direct=%lu dvalid=%u generic=%lu gvalid=%u\n",
                         name, api, (unsigned long)d.value, (unsigned)(same_core && d.valid),
                         (unsigned long)g.value, (unsigned)(same_core && g.valid));
          if constexpr (requires { B::diag_drain(); }) B::diag_drain();
        }
      }
      const auto p = cost_probe::difference(w.before.pendsv_unknown, w.after.pendsv_unknown);
      B::diag_printf("[ROUTE] %s pendsv_unknown=%lu valid=%u\n", name,
                     (unsigned long)p.value, (unsigned)(same_core && p.valid));
      if constexpr (requires { B::diag_drain(); }) B::diag_drain();
    }
  }
};
using route_measure = route_windows<route_cost_probe_enabled>;

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
  uint32_t version;               // 3
  uint32_t rounds;                // 2048
  uint32_t baseline_method_mean;  // baseline mean for method call (legacy)
  uint32_t baseline_desc_mean;    // baseline mean for desc call (legacy)
  uint32_t method_raw_mean;       // (legacy)
  uint32_t method_net_mean;       // (legacy)
  uint32_t desc_raw_mean;         // (legacy)
  uint32_t desc_net_mean;         // (legacy)
  uint32_t checked_ok;            // (legacy)
  // 6A Corrected series:
  uint32_t corr_baseline_method_mean;
  uint32_t corr_baseline_desc_mean;
  uint32_t corr_method_raw_mean;
  uint32_t corr_method_net_mean;
  uint32_t corr_desc_raw_mean;
  uint32_t corr_desc_net_mean;
  uint32_t corr_checked_ok;
};
static_assert(sizeof(shizuku_rp2350_u_cost_record_t) == 68, "shizuku_rp2350_u_cost_record_t size");

volatile shizuku_rp2350_u_cost_record_t shizuku_rp2350_u_cost_record = {};
volatile uint32_t shizuku_rp2350_u_cost_stage = 0;

template <typename BOARD_TYPE>
void drain_diag_if_supported() {
  if constexpr (requires { BOARD_TYPE::diag_drain(); }) BOARD_TYPE::diag_drain();
}

void record_check(const char *name, bool ok, unsigned long got, unsigned long want) {
  account_selftest_check(passed, failed, ok);
  if (ok) {
    BOARD::diag_printf("[COST] PASS %s (=%lu)\n", name, got);
  } else {
    record_fail(name, got, want);
    BOARD::diag_printf("[COST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
  drain_diag_if_supported<BOARD>();
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

    // 1b. U 側コードの SRAM window を検査して region 1 (REGION_UNUSED) に RO+X で張る。
    //     非特権に実行を開ける SRAM はこの 1 本だけで、計測が終わったら閉じる。
    //     MPU は core ごとなので、P / C が走る (= この) core にだけ張る。
    const u_cost_sram::range code_window{
        u_cost_sram::code_address(reinterpret_cast<uintptr_t>(&u_cost_code_begin)),
        reinterpret_cast<uintptr_t>(u_cost_code_end)};
    {
      const uintptr_t entries[] = {reinterpret_cast<uintptr_t>(&u_cost_parent_worker),
                                   reinterpret_cast<uintptr_t>(&u_cost_child_worker)};
      const u_cost_sram::range protected_ranges[] = {
          {reinterpret_cast<uintptr_t>(u_cost_p_data_pool),
           reinterpret_cast<uintptr_t>(u_cost_p_data_pool) + sizeof(u_cost_p_data_pool)},
          {reinterpret_cast<uintptr_t>(u_cost_p_stack),
           reinterpret_cast<uintptr_t>(u_cost_p_stack) + sizeof(u_cost_p_stack)},
          {reinterpret_cast<uintptr_t>(u_cost_c_data_pool),
           reinterpret_cast<uintptr_t>(u_cost_c_data_pool) + sizeof(u_cost_c_data_pool)},
          {reinterpret_cast<uintptr_t>(u_cost_c_stack),
           reinterpret_cast<uintptr_t>(u_cost_c_stack) + sizeof(u_cost_c_stack)}};
      const auto verdict = u_cost_sram::check_code_window(
          code_window, {CHIP::kSramBase, CHIP::kSramEnd}, entries, protected_ranges);
      // pad のアドレスを出すのは --gc-sections に pad を捨てさせないため (配置確認にも使う)。
      BOARD::diag_printf("[UCODE] window=%08lx..%08lx bytes=%lu entries=%08lx,%08lx pad=%lu "
                         "pad_flash=%08lx pad_bss=%08lx verdict=%u\n",
                         (unsigned long)code_window.begin, (unsigned long)code_window.end,
                         (unsigned long)(code_window.end - code_window.begin),
                         (unsigned long)entries[0], (unsigned long)entries[1],
                         (unsigned long)shizuku::layout_pad_bytes,
                         (unsigned long)u_cost_layout_pad_flash,
                         (unsigned long)u_cost_layout_pad_bss, (unsigned)verdict);
      drain_diag_if_supported<BOARD>();
      if (verdict != u_cost_sram::verdict::ok) {
        BOARD::panic("rp2350_u_cost: U code window is not a valid SRAM window");
      }
      if (!u_cost_sram::denied_code_entry(
              code_window, {CHIP::kSramBase, CHIP::kSramEnd},
              reinterpret_cast<uintptr_t>(&u_cost_denied_code))) {
        BOARD::panic("rp2350_u_cost: denied-code probe is not outside the U code window");
      }
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

    // REGION_UNUSED is core-global, so keep it open only for this U-cost test interval.
    // Normal completion closes it by RAII; the only failure exits below are noreturn panic.
    const auto sync_mpu = [] { asm volatile("dsb\nisb" ::: "memory"); };
    u_cost_sram::scoped_code_window<ARCH, decltype(sync_mpu)> code_window_scope(
        code_window, sync_mpu);

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
    route_measure::begin<ARCH>(0);
    const auto base_meth_xip_before = xip_cost_start();
    const uint32_t t_base_meth_start = cycles_now<ARCH>();
    const auto base_meth_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P,
        0,
        2 /* cmd=2: baseline method */);
    const uint32_t t_base_meth_end = cycles_now<ARCH>();
    const auto base_meth_xip_after = xip_cost_snapshot();
    route_measure::end<ARCH>(0);
    const uint32_t base_meth_cycles = t_base_meth_end - t_base_meth_start;

    // 7. 親 P に cmd=3 (measure method) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05040u;
    route_measure::begin<ARCH>(1);
    const auto meth_xip_before = xip_cost_start();
    const uint32_t t_meth_start = cycles_now<ARCH>();
    const auto meth_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P,
        0,
        3 /* cmd=3: measure method */);
    const uint32_t t_meth_end = cycles_now<ARCH>();
    const auto meth_xip_after = xip_cost_snapshot();
    route_measure::end<ARCH>(1);
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
    route_measure::begin<ARCH>(2);
    const auto base_desc_xip_before = xip_cost_start();
    const uint32_t t_base_desc_start = cycles_now<ARCH>();
    const auto base_desc_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P,
        0,
        5 /* cmd=5: baseline desc */);
    const uint32_t t_base_desc_end = cycles_now<ARCH>();
    const auto base_desc_xip_after = xip_cost_snapshot();
    route_measure::end<ARCH>(2);
    const uint32_t base_desc_cycles = t_base_desc_end - t_base_desc_start;

    // 10. 親 P に cmd=6 (measure desc) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05070u;
    route_measure::begin<ARCH>(3);
    const auto desc_xip_before = xip_cost_start();
    const uint32_t t_desc_start = cycles_now<ARCH>();
    const auto desc_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P,
        0,
        6 /* cmd=6: measure desc */);
    const uint32_t t_desc_end = cycles_now<ARCH>();
    const auto desc_xip_after = xip_cost_snapshot();
    route_measure::end<ARCH>(3);
    const uint32_t desc_cycles = t_desc_end - t_desc_start;
    xip_cost_report<BOARD>("U-U method baseline", base_meth_xip_before, base_meth_xip_after);
    xip_cost_report<BOARD>("U-U desc baseline", base_desc_xip_before, base_desc_xip_after);
    xip_cost_report<BOARD>("U-U method legacy", meth_xip_before, meth_xip_after);
    xip_cost_report<BOARD>("U-U desc legacy", desc_xip_before, desc_xip_after);
    route_measure::report<BOARD>(0, "U-U method baseline legacy");
    route_measure::report<BOARD>(1, "U-U method legacy");
    route_measure::report<BOARD>(2, "U-U desc baseline legacy");
    route_measure::report<BOARD>(3, "U-U desc legacy");

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
        "[COST-LEGACY] %-24s net mean %5lu cyc  [raw mean %lu, baseline mean %lu]\n",
        "U->U method call (1 hop)",
        (unsigned long)net_meth_mean,
        (unsigned long)meth_raw_mean,
        (unsigned long)base_meth_mean);
    BOARD::diag_printf(
        "[COST-LEGACY] %-24s net mean %5lu cyc  [raw mean %lu, baseline mean %lu]\n",
        "U->U desc call (1 hop)",
        (unsigned long)net_desc_mean,
        (unsigned long)desc_raw_mean,
        (unsigned long)base_desc_mean);

    // =======================================================================
    // 6A Corrected series:
    // =======================================================================
    shizuku_rp2350_u_cost_stage = 0xC05100u;
    const bool counter_enabled = ARCH::cycle_counter_enable();

    uint32_t default_control = 0;
#if defined(__ARM_ARCH_8M_MAIN__)
    asm volatile("mrs %0, control" : "=r"(default_control));
#endif
    const uint32_t default_fpca = (default_control >> 2) & 1u;
    BOARD::diag_printf(
        "[COST-FP] default fpca=%lu (existing [COST-CORRECTED] measurement ran with this caller state)\n",
        (unsigned long)default_fpca);
    drain_diag_if_supported<BOARD>();

    // 11. 親 P に cmd=10 (warm-up method: 計測区間外) を実行させる
    const auto corr_meth_warmup_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, 10 /* cmd=10 */);

    // 12. 親 P に cmd=12 (baseline method corrected) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05110u;
    route_measure::begin<ARCH>(4);
    const auto corr_base_meth_xip_before = xip_cost_start();
    const uint32_t t_corr_base_meth_start = cycles_now<ARCH>();
    const auto corr_base_meth_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P, 0, 12 /* cmd=12 */);
    const uint32_t t_corr_base_meth_end = cycles_now<ARCH>();
    const auto corr_base_meth_xip_after = xip_cost_snapshot();
    route_measure::end<ARCH>(4);
    const uint32_t corr_base_meth_cycles = t_corr_base_meth_end - t_corr_base_meth_start;

    // 13. 親 P に cmd=13 (measure method corrected) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05120u;
    route_measure::begin<ARCH>(5);
    const auto corr_meth_xip_before = xip_cost_start();
    const uint32_t t_corr_meth_start = cycles_now<ARCH>();
    const auto corr_meth_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P, 0, 13 /* cmd=13 */);
    const uint32_t t_corr_meth_end = cycles_now<ARCH>();
    const auto corr_meth_xip_after = xip_cost_snapshot();
    route_measure::end<ARCH>(5);
    const uint32_t corr_meth_cycles = t_corr_meth_end - t_corr_meth_start;

    // 14. 親 P に cmd=14 (verify method check 41->42: 計測区間外) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05130u;
    const auto corr_meth_check_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P, 0, 14 /* cmd=14 */);

    // 15. 親 P に cmd=11 (warm-up desc: 計測区間外) を実行させる
    const auto corr_desc_warmup_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, 11 /* cmd=11 */);

    // 16. 親 P に cmd=15 (baseline desc corrected) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05140u;
    route_measure::begin<ARCH>(6);
    const auto corr_base_desc_xip_before = xip_cost_start();
    const uint32_t t_corr_base_desc_start = cycles_now<ARCH>();
    const auto corr_base_desc_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P, 0, 15 /* cmd=15 */);
    const uint32_t t_corr_base_desc_end = cycles_now<ARCH>();
    const auto corr_base_desc_xip_after = xip_cost_snapshot();
    route_measure::end<ARCH>(6);
    const uint32_t corr_base_desc_cycles = t_corr_base_desc_end - t_corr_base_desc_start;

    // 17. 親 P に cmd=16 (measure desc corrected) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05150u;
    route_measure::begin<ARCH>(7);
    const auto corr_desc_xip_before = xip_cost_start();
    const uint32_t t_corr_desc_start = cycles_now<ARCH>();
    const auto corr_desc_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P, 0, 16 /* cmd=16 */);
    const uint32_t t_corr_desc_end = cycles_now<ARCH>();
    const auto corr_desc_xip_after = xip_cost_snapshot();
    route_measure::end<ARCH>(7);
    const uint32_t corr_desc_cycles = t_corr_desc_end - t_corr_desc_start;
    xip_cost_report<BOARD>("U-U method baseline corrected", corr_base_meth_xip_before, corr_base_meth_xip_after);
    xip_cost_report<BOARD>("U-U desc baseline corrected", corr_base_desc_xip_before, corr_base_desc_xip_after);
    xip_cost_report<BOARD>("U-U method corrected", corr_meth_xip_before, corr_meth_xip_after);
    xip_cost_report<BOARD>("U-U desc corrected", corr_desc_xip_before, corr_desc_xip_after);
    route_measure::report<BOARD>(4, "U-U method baseline corrected");
    route_measure::report<BOARD>(5, "U-U method corrected");
    route_measure::report<BOARD>(6, "U-U desc baseline corrected");
    route_measure::report<BOARD>(7, "U-U desc corrected");

    // 18. 親 P に cmd=17 (verify desc check 41->42: 計測区間外) を実行させる
    shizuku_rp2350_u_cost_stage = 0xC05160u;
    const auto corr_desc_check_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD),
        OBJECT_U_COST_P, 0, 17 /* cmd=17 */);

    // Probe both SRAM execute denial and data denial while the bounded code window is open.
    // Use a separate thread because faults stop only the offending thread.
    const auto export_probe_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, 18);
    record_check("u_cost denied-probe method registered",
                 export_probe_res.error == 0 && export_probe_res.value == 0,
                 export_probe_res.value, 0);
    auto run_denied_probe = [&](uintptr_t mode, const char *name, bool code_probe) {
      const uint32_t faults_before = kernel_instance.faults().count;
      const auto spawned = ARCH::syscall(static_cast<uintptr_t>(object_api::SPAWN),
                                         OBJECT_U_COST_P, 1, mode);
      const bool spawn_ok = spawned.error == 0 && spawned.value != 0;
      if (spawn_ok) {
        for (uint32_t guard = 0;
             guard < 200 && kernel_instance.faults().count == faults_before; ++guard)
          ARCH::syscall(static_cast<uintptr_t>(object_api::YIELD));
      }
      const auto &fault = kernel_instance.faults();
      const bool new_fault = spawn_ok && fault.count == faults_before + 1;
      bool expected_fault = new_fault && fault.thread == spawned.value;
      if (code_probe) {
        expected_fault = expected_fault && (fault.status & 0x1u) != 0 &&
                         (fault.pc & ~uintptr_t(1)) ==
                             u_cost_sram::code_address(
                                 reinterpret_cast<uintptr_t>(&u_cost_denied_code));
      } else {
        expected_fault = expected_fault && (fault.status & 0x82u) == 0x82u &&
                         fault.address == reinterpret_cast<uintptr_t>(u_cost_p_stack) &&
                         (fault.pc & ~uintptr_t(1)) ==
                             u_cost_sram::code_address(
                                 reinterpret_cast<uintptr_t>(u_cost_probe_data_fault_pc));
      }
      record_check(name, expected_fault, fault.status,
                   code_probe ? 0x1u : 0x82u);
      record_check("u_cost SRAM deny probe stopped only its thread",
                   new_fault && fault.thread == spawned.value,
                   fault.thread, spawned.value);
      BOARD::diag_printf("[UCODE-DENY] mode=%lu spawn=%lu faults=%lu thread=%lu "
                         "pc=%08lx address=%08lx cfsr=%08lx\n",
                         (unsigned long)mode, (unsigned long)spawned.value,
                         (unsigned long)fault.count, (unsigned long)fault.thread,
                         (unsigned long)fault.pc, (unsigned long)fault.address,
                         (unsigned long)fault.status);
      drain_diag_if_supported<BOARD>();
    };
    run_denied_probe(90, "u_cost SRAM code outside executable window faults", true);
    run_denied_probe(91, "u_cost SRAM data outside object window faults", false);

    // (f) callee SP 偽装防止: callee が SP を stack_ceiling 以上に偽装して再呼出し → NO_STACK で拒絶され caller 枠センチネル無傷
    const auto spoof_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, 8 /* cmd=8 */);
    record_check("u_cost: callee SP spoofed above ceiling rejected with NO_STACK and caller sentinel intact",
                 spoof_res.error == 0 && spoof_res.value == 0,
                 spoof_res.value, 0);

    // (g) stream stack 範囲隔離: callee が STREAM_PUSH_N / POP_N で caller 枠を指定 → BAD_STREAM で拒絶
    const auto stream_res = ARCH::syscall(
        static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, 9 /* cmd=9 */);
    record_check("u_cost: STREAM_PUSH_N / POP_N to caller stack rejected with BAD_STREAM",
                 stream_res.error == 0 && stream_res.value == 0,
                 stream_res.value, 0);

    // 結果抽出
    const uint32_t corr_meth_final = shared[21];       // offset 84 (want ROUNDS)
    const uint32_t corr_meth_accum_err = shared[22];   // offset 88 (want 0)
    const uint32_t corr_desc_final = shared[27];       // offset 108 (want ROUNDS)
    const uint32_t corr_desc_accum_err = shared[28];   // offset 112 (want 0)

    const uint32_t corr_meth_check_val = shared[10];   // offset 40 (want 42)
    const uint32_t corr_meth_check_err = shared[11];   // offset 44 (want 0)
    const uint32_t corr_desc_check_val = shared[15];   // offset 60 (want 42)
    const uint32_t corr_desc_check_err = shared[16];   // offset 64 (want 0)

    const uint32_t corr_base_meth_mean = corr_base_meth_cycles / ROUNDS;
    const uint32_t corr_meth_raw_mean = corr_meth_cycles / ROUNDS;
    const uint32_t corr_net_meth_mean = corr_meth_cycles > corr_base_meth_cycles
                                           ? (corr_meth_cycles - corr_base_meth_cycles) / ROUNDS
                                           : 0;

    const uint32_t corr_base_desc_mean = corr_base_desc_cycles / ROUNDS;
    const uint32_t corr_desc_raw_mean = corr_desc_cycles / ROUNDS;
    const uint32_t corr_net_desc_mean = corr_desc_cycles > corr_base_desc_cycles
                                           ? (corr_desc_cycles - corr_base_desc_cycles) / ROUNDS
                                           : 0;

    BOARD::diag_printf(
        "[COST-CORRECTED] %-24s net mean %5lu cyc  [raw mean %lu, baseline mean %lu]\n",
        "U->U method call (1 hop)",
        (unsigned long)corr_net_meth_mean,
        (unsigned long)corr_meth_raw_mean,
        (unsigned long)corr_base_meth_mean);
    drain_diag_if_supported<BOARD>();
    BOARD::diag_printf(
        "[COST-CORRECTED] %-24s net mean %5lu cyc  [raw mean %lu, baseline mean %lu]\n",
        "U->U desc call (1 hop)",
        (unsigned long)corr_net_desc_mean,
        (unsigned long)corr_desc_raw_mean,
        (unsigned long)corr_base_desc_mean);
    drain_diag_if_supported<BOARD>();

    // RAM レコードへの記録
    shizuku_rp2350_u_cost_record.magic = 0x55554353u; // "UUCS"
    shizuku_rp2350_u_cost_record.version = 3;
    shizuku_rp2350_u_cost_record.rounds = ROUNDS;
    shizuku_rp2350_u_cost_record.baseline_method_mean = base_meth_mean;
    shizuku_rp2350_u_cost_record.baseline_desc_mean = base_desc_mean;
    shizuku_rp2350_u_cost_record.method_raw_mean = meth_raw_mean;
    shizuku_rp2350_u_cost_record.method_net_mean = net_meth_mean;
    shizuku_rp2350_u_cost_record.desc_raw_mean = desc_raw_mean;
    shizuku_rp2350_u_cost_record.desc_net_mean = net_desc_mean;

    shizuku_rp2350_u_cost_record.corr_baseline_method_mean = corr_base_meth_mean;
    shizuku_rp2350_u_cost_record.corr_baseline_desc_mean = corr_base_desc_mean;
    shizuku_rp2350_u_cost_record.corr_method_raw_mean = corr_meth_raw_mean;
    shizuku_rp2350_u_cost_record.corr_method_net_mean = corr_net_meth_mean;
    shizuku_rp2350_u_cost_record.corr_desc_raw_mean = corr_desc_raw_mean;
    shizuku_rp2350_u_cost_record.corr_desc_net_mean = corr_net_desc_mean;

    // PASS/FAIL アサーション (D7 Legacy)
    const bool meth_ok = (meth_err == 0 && meth_val == 42);
    record_check("u_cost [LEGACY]: U->U method call returns 42", meth_ok, meth_val, 42);

    const bool desc_ok = (desc_err == 0 && desc_val == 42);
    record_check("u_cost [LEGACY]: U->U descriptor call returns 42", desc_ok, desc_val, 42);

    shizuku_rp2350_u_cost_record.checked_ok = (meth_ok && desc_ok) ? 1u : 0u;

    // PASS/FAIL アサーション (6A Corrected)
    const bool counter_ok = counter_enabled &&
                            (corr_base_meth_cycles > 0) &&
                            (corr_meth_cycles > corr_base_meth_cycles) &&
                            (corr_base_desc_cycles > 0) &&
                            (corr_desc_cycles > corr_base_desc_cycles);
    record_check("u_cost [CORRECTED]: DWT cycle counter progressed and valid", counter_ok, counter_ok ? 1 : 0, 1);

    const uint32_t desc_create_inner_error = static_cast<uint32_t>(desc_create_res.value);
    const bool desc_create_ok = (desc_create_res.error == 0 && desc_create_inner_error == 0);
    record_check("u_cost [CORRECTED]: CREATE_METHOD_DESCRIPTOR succeeded", desc_create_ok,
                 desc_create_ok ? 0 : 1, 0);

    u_cost_eval_input_t meth_eval{
        .counter_enabled = counter_enabled,
        .base_cycles = corr_base_meth_cycles,
        .measure_cycles = corr_meth_cycles,
        .final_chain_value = corr_meth_final,
        .accumulated_error = corr_meth_accum_err,
        .verify_value = corr_meth_check_val,
        .verify_error = corr_meth_check_err,
        .verify_outer_error = static_cast<uint32_t>(corr_meth_check_res.error),
        .verify_outer_value = static_cast<uint32_t>(corr_meth_check_res.value),
        .desc_create_error = desc_create_inner_error,
        .desc_create_outer_error = static_cast<uint32_t>(desc_create_res.error),
        .expected_rounds = ROUNDS,
        .outer_error = static_cast<uint32_t>(corr_meth_res.error),
        .outer_value = static_cast<uint32_t>(corr_meth_res.value),
        .executed_rounds = shared[23],
        .baseline_outer_error = static_cast<uint32_t>(corr_base_meth_res.error),
        .baseline_outer_value = static_cast<uint32_t>(corr_base_meth_res.value),
        .baseline_executed_rounds = shared[20],
        .warmup_outer_error = static_cast<uint32_t>(corr_meth_warmup_res.error),
        .warmup_rounds = static_cast<uint32_t>(corr_meth_warmup_res.value),
    };
    const bool corr_meth_ok = evaluate_u_cost_invariants(meth_eval);
    record_check("u_cost [CORRECTED]: method call chain & error accumulation", corr_meth_ok, corr_meth_final, ROUNDS);

    u_cost_eval_input_t desc_eval{
        .counter_enabled = counter_enabled,
        .base_cycles = corr_base_desc_cycles,
        .measure_cycles = corr_desc_cycles,
        .final_chain_value = corr_desc_final,
        .accumulated_error = corr_desc_accum_err,
        .verify_value = corr_desc_check_val,
        .verify_error = corr_desc_check_err,
        .verify_outer_error = static_cast<uint32_t>(corr_desc_check_res.error),
        .verify_outer_value = static_cast<uint32_t>(corr_desc_check_res.value),
        .desc_create_error = desc_create_inner_error,
        .desc_create_outer_error = static_cast<uint32_t>(desc_create_res.error),
        .expected_rounds = ROUNDS,
        .outer_error = static_cast<uint32_t>(corr_desc_res.error),
        .outer_value = static_cast<uint32_t>(corr_desc_res.value),
        .executed_rounds = shared[29],
        .baseline_outer_error = static_cast<uint32_t>(corr_base_desc_res.error),
        .baseline_outer_value = static_cast<uint32_t>(corr_base_desc_res.value),
        .baseline_executed_rounds = shared[26],
        .warmup_outer_error = static_cast<uint32_t>(corr_desc_warmup_res.error),
        .warmup_rounds = static_cast<uint32_t>(corr_desc_warmup_res.value),
    };
    const bool corr_desc_ok = evaluate_u_cost_invariants(desc_eval);
    record_check("u_cost [CORRECTED]: descriptor call chain & error accumulation", corr_desc_ok, corr_desc_final, ROUNDS);

    shizuku_rp2350_u_cost_record.corr_checked_ok = (counter_ok && desc_create_ok && corr_meth_ok && corr_desc_ok) ? 1u : 0u;

    // =======================================================================
    // FPCA series (fpca=0 and fpca=1):
    // =======================================================================
    auto set_boot_fpca = [](uint32_t want_fpca) -> uint32_t {
#if defined(__ARM_ARCH_8M_MAIN__)
      if (want_fpca == 0) {
        asm volatile(
            "mrs r0, control\n"
            "bic r0, r0, #4\n"
            "msr control, r0\n"
            "isb\n"
            ::: "r0", "memory");
      } else {
        asm volatile(
            "movs r0, #0\n"
            "vmov s0, r0\n"
            "isb\n"
            ::: "r0", "s0", "memory");
      }
      uint32_t ctrl = 0;
      asm volatile("mrs %0, control" : "=r"(ctrl));
      return (ctrl >> 2) & 1u;
#else
      return want_fpca;
#endif
    };

#if defined(SHIZUKU_DIAG_FP)
    static shizuku::diag_fp::state_t s_fpca0_meth_diag{};
    static shizuku::diag_fp::state_t s_fpca0_desc_diag{};
#endif

    auto run_fp_condition = [&](uint32_t want_fpca, uint32_t cmd_base) {
      u_cost_fp_invariants_input_t fp_inv{};
      fp_inv.expected_fpca = want_fpca;

      // 1. warmup method
      set_boot_fpca(want_fpca);
      const auto fp_warm_meth_res = ARCH::syscall(
          static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, cmd_base + 0);

      // 2. baseline method
      fp_inv.priv_meth_base = set_boot_fpca(want_fpca);
      const uint32_t t_base_meth_start = cycles_now<ARCH>();
      const auto fp_base_meth_res = ARCH::syscall(
          static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, cmd_base + 2);
      const uint32_t t_base_meth_end = cycles_now<ARCH>();
      const uint32_t fp_base_meth_cycles = t_base_meth_end - t_base_meth_start;

      // 3. measure method
#if defined(SHIZUKU_DIAG_FP)
      shizuku::diag_fp::reset();
#endif
      fp_inv.priv_meth_meas = set_boot_fpca(want_fpca);
      const uint32_t t_meth_start = cycles_now<ARCH>();
      const auto fp_meth_res = ARCH::syscall(
          static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, cmd_base + 3);
      const uint32_t t_meth_end = cycles_now<ARCH>();
      const uint32_t fp_meth_cycles = t_meth_end - t_meth_start;
#if defined(SHIZUKU_DIAG_FP)
      shizuku::diag_fp::stop();
      const auto meth_diag = shizuku::diag_fp::g_state;
#endif

      // 4. verify method check (41 -> 42)
      set_boot_fpca(want_fpca);
      const auto fp_meth_check_res = ARCH::syscall(
          static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, cmd_base + 4);

      // 5. warmup desc
      set_boot_fpca(want_fpca);
      const auto fp_warm_desc_res = ARCH::syscall(
          static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, cmd_base + 1);

      // 6. baseline desc
      fp_inv.priv_desc_base = set_boot_fpca(want_fpca);
      const uint32_t t_base_desc_start = cycles_now<ARCH>();
      const auto fp_base_desc_res = ARCH::syscall(
          static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, cmd_base + 5);
      const uint32_t t_base_desc_end = cycles_now<ARCH>();
      const uint32_t fp_base_desc_cycles = t_base_desc_end - t_base_desc_start;

      // 7. measure desc
#if defined(SHIZUKU_DIAG_FP)
      shizuku::diag_fp::reset();
#endif
      fp_inv.priv_desc_meas = set_boot_fpca(want_fpca);
      const uint32_t t_desc_start = cycles_now<ARCH>();
      const auto fp_desc_res = ARCH::syscall(
          static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, cmd_base + 6);
      const uint32_t t_desc_end = cycles_now<ARCH>();
      const uint32_t fp_desc_cycles = t_desc_end - t_desc_start;
#if defined(SHIZUKU_DIAG_FP)
      shizuku::diag_fp::stop();
      const auto desc_diag = shizuku::diag_fp::g_state;
#endif

      // 8. verify desc check (41 -> 42)
      set_boot_fpca(want_fpca);
      const auto fp_desc_check_res = ARCH::syscall(
          static_cast<uintptr_t>(object_api::CALL_METHOD), OBJECT_U_COST_P, 0, cmd_base + 7);

      fp_inv.u_meth_base_before = shared[30];
      fp_inv.u_meth_base_after = shared[31];
      const uint32_t meth_base_rounds = shared[34];

      fp_inv.u_meth_meas_before = shared[35];
      fp_inv.u_meth_meas_after = shared[36];
      const uint32_t meth_final = shared[37];
      const uint32_t meth_accum_err = shared[38];
      const uint32_t meth_rounds = shared[39];

      fp_inv.u_desc_base_before = shared[40];
      fp_inv.u_desc_base_after = shared[41];
      const uint32_t desc_base_rounds = shared[44];

      fp_inv.u_desc_meas_before = shared[45];
      fp_inv.u_desc_meas_after = shared[46];
      const uint32_t desc_final = shared[47];
      const uint32_t desc_accum_err = shared[48];
      const uint32_t desc_rounds = shared[49];

      const uint32_t meth_check_val = shared[50];
      const uint32_t meth_check_err = shared[51];
      const uint32_t desc_check_val = shared[52];
      const uint32_t desc_check_err = shared[53];

      const uint32_t base_meth_mean = fp_base_meth_cycles / ROUNDS;
      const uint32_t meth_raw_mean = fp_meth_cycles / ROUNDS;
      const uint32_t net_meth_mean = fp_meth_cycles > fp_base_meth_cycles
                                         ? (fp_meth_cycles - fp_base_meth_cycles) / ROUNDS
                                         : 0;

      const uint32_t base_desc_mean = fp_base_desc_cycles / ROUNDS;
      const uint32_t desc_raw_mean = fp_desc_cycles / ROUNDS;
      const uint32_t net_desc_mean = fp_desc_cycles > fp_base_desc_cycles
                                         ? (fp_desc_cycles - fp_base_desc_cycles) / ROUNDS
                                         : 0;

      BOARD::diag_printf(
          "[COST-FP] %-24s fpca=%lu net mean %5lu cyc  [raw mean %lu, baseline mean %lu]\n",
          "U->U method call (1 hop)",
          (unsigned long)want_fpca,
          (unsigned long)net_meth_mean,
          (unsigned long)meth_raw_mean,
          (unsigned long)base_meth_mean);
      drain_diag_if_supported<BOARD>();
      BOARD::diag_printf(
          "[COST-FP] %-24s fpca=%lu net mean %5lu cyc  [raw mean %lu, baseline mean %lu]\n",
          "U->U desc call (1 hop)",
          (unsigned long)want_fpca,
          (unsigned long)net_desc_mean,
          (unsigned long)desc_raw_mean,
          (unsigned long)base_desc_mean);
      drain_diag_if_supported<BOARD>();

#if defined(SHIZUKU_DIAG_FP)
      BOARD::diag_printf(
          "[DIAG-FP] Note: Diagnostic build incurs probe overhead (few cycles per point); values differ from normal [COST-FP].\n");
      drain_diag_if_supported<BOARD>();

      auto print_diag_row = [](const char *label, uint32_t fpca_val,
                               const shizuku::diag_fp::state_t &st) {
        const uint32_t n = st.sample_count > 0 ? st.sample_count : 1;
        const uint32_t m_k2_k1 = static_cast<uint32_t>(st.accum_k2_k1 / n);
        const uint32_t m_k3_k2 = static_cast<uint32_t>(st.accum_k3_k2 / n);
        const uint32_t m_k4_k3 = static_cast<uint32_t>(st.accum_k4_k3 / n);
        const uint32_t m_mat   = static_cast<uint32_t>(st.accum_k3b_mat / n);
        const uint32_t m_move  = static_cast<uint32_t>(st.accum_k3c_memmove / n);
        const uint32_t m_ret_other = (m_k4_k3 >= m_mat + m_move) ? (m_k4_k3 - m_mat - m_move) : 0;
        const uint32_t n_caller = st.caller_u_count > 0 ? st.caller_u_count : 1;
        const uint32_t m_caller = static_cast<uint32_t>(st.accum_caller_u / n_caller);
        const uint32_t m_total = m_k2_k1 + m_k3_k2 + m_k4_k3 + (st.caller_u_count > 0 ? m_caller : 0);

        BOARD::diag_printf(
            "[DIAG-FP] %s fpca=%lu K2-K1=%lu K3-K2=%lu K4-K3=%lu (mat=%lu memmove=%lu other=%lu) K1next-K4=%lu total=%lu (mean, N=%lu)\n",
            label,
            (unsigned long)fpca_val,
            (unsigned long)m_k2_k1,
            (unsigned long)m_k3_k2,
            (unsigned long)m_k4_k3,
            (unsigned long)m_mat,
            (unsigned long)m_move,
            (unsigned long)m_ret_other,
            (unsigned long)m_caller,
            (unsigned long)m_total,
            (unsigned long)st.sample_count);
        drain_diag_if_supported<BOARD>();
      };

      auto print_diag_diff = [](const char *label,
                                const shizuku::diag_fp::state_t &st0,
                                const shizuku::diag_fp::state_t &st1) {
        const uint32_t n0 = st0.sample_count > 0 ? st0.sample_count : 1;
        const uint32_t n1 = st1.sample_count > 0 ? st1.sample_count : 1;
        const int32_t d_k2_k1 = static_cast<int32_t>(st1.accum_k2_k1 / n1) - static_cast<int32_t>(st0.accum_k2_k1 / n0);
        const int32_t d_k3_k2 = static_cast<int32_t>(st1.accum_k3_k2 / n1) - static_cast<int32_t>(st0.accum_k3_k2 / n0);
        const int32_t d_k4_k3 = static_cast<int32_t>(st1.accum_k4_k3 / n1) - static_cast<int32_t>(st0.accum_k4_k3 / n0);
        const int32_t d_mat   = static_cast<int32_t>(st1.accum_k3b_mat / n1) - static_cast<int32_t>(st0.accum_k3b_mat / n0);
        const int32_t d_move  = static_cast<int32_t>(st1.accum_k3c_memmove / n1) - static_cast<int32_t>(st0.accum_k3c_memmove / n0);
        const uint32_t nc0 = st0.caller_u_count > 0 ? st0.caller_u_count : 1;
        const uint32_t nc1 = st1.caller_u_count > 0 ? st1.caller_u_count : 1;
        const int32_t d_caller = static_cast<int32_t>(st1.accum_caller_u / nc1) - static_cast<int32_t>(st0.accum_caller_u / nc0);
        const int32_t d_total = d_k2_k1 + d_k3_k2 + d_k4_k3 + d_caller;

        BOARD::diag_printf(
            "[DIAG-FP] %s diff(fpca=1 - fpca=0) K2-K1=%+ld K3-K2=%+ld K4-K3=%+ld (mat=%+ld memmove=%+ld) K1next-K4=%+ld total=%+ld\n",
            label,
            (long)d_k2_k1,
            (long)d_k3_k2,
            (long)d_k4_k3,
            (long)d_mat,
            (long)d_move,
            (long)d_caller,
            (long)d_total);
        drain_diag_if_supported<BOARD>();
      };

      auto print_fpccr_status = [](const shizuku::diag_fp::state_t &st) {
        using shizuku::diag_fp::point;
        BOARD::diag_printf(
            "[DIAG-FP] FPCCR=0x%08lX LSPEN=%u ASPEN=%u LSPACT@K1=%u @K2=%u @K3=%u @K3b-before=%u @K3b-after=%u @K3c-before=%u @K3c-after=%u @K4=%u\n",
            (unsigned long)st.last_fpccr,
            (unsigned)((st.last_fpccr >> 30) & 1u),
            (unsigned)((st.last_fpccr >> 31) & 1u),
            (unsigned)st.last_lspact[static_cast<size_t>(point::K1)],
            (unsigned)st.last_lspact[static_cast<size_t>(point::K2)],
            (unsigned)st.last_lspact[static_cast<size_t>(point::K3)],
            (unsigned)st.last_lspact[static_cast<size_t>(point::K3B_BEFORE)],
            (unsigned)st.last_lspact[static_cast<size_t>(point::K3B_AFTER)],
            (unsigned)st.last_lspact[static_cast<size_t>(point::K3C_BEFORE)],
            (unsigned)st.last_lspact[static_cast<size_t>(point::K3C_AFTER)],
            (unsigned)st.last_lspact[static_cast<size_t>(point::K4)]);
        drain_diag_if_supported<BOARD>();
      };

      print_diag_row("method", want_fpca, meth_diag);
      print_diag_row("desc", want_fpca, desc_diag);
      print_fpccr_status(meth_diag);

      if (want_fpca == 0) {
        s_fpca0_meth_diag = meth_diag;
        s_fpca0_desc_diag = desc_diag;
      } else {
        print_diag_diff("method", s_fpca0_meth_diag, meth_diag);
        print_diag_diff("desc", s_fpca0_desc_diag, desc_diag);
      }

      // PASS/FAIL record_check (要件 5)
      if (want_fpca == 1) {
        // 親ワーカー呼び出し (1回) + 子ワーカー呼出し (ROUNDS=2048回) により sample_count は ROUNDS+1 (2049) となる
        const bool diag_monotonic_ok = meth_diag.order_valid && (meth_diag.sample_count >= ROUNDS) &&
                                       desc_diag.order_valid && (desc_diag.sample_count >= ROUNDS);
        const bool diag_fpccr_ok = ((meth_diag.last_fpccr >> 30) & 3u) == 3u; // ASPEN=1, LSPEN=1

        record_check("u_cost [DIAG-FP]: timestamps monotonic K1<K2<K3<K4",
                     diag_monotonic_ok, diag_monotonic_ok ? 1 : 0, 1);
        record_check("u_cost [DIAG-FP]: FPCCR valid (ASPEN=1, LSPEN=1)",
                     diag_fpccr_ok, diag_fpccr_ok ? 1 : 0, 1);
      }
#endif

      const bool counter_ok = counter_enabled &&
                              (fp_base_meth_cycles > 0) &&
                              (fp_meth_cycles > fp_base_meth_cycles) &&
                              (fp_base_desc_cycles > 0) &&
                              (fp_desc_cycles > fp_base_desc_cycles);
      if (want_fpca == 0) {
        record_check("u_cost [FP]: DWT cycle counter progressed and valid (fpca=0)",
                     counter_ok, counter_ok ? 1 : 0, 1);
      } else {
        record_check("u_cost [FP]: DWT cycle counter progressed and valid (fpca=1)",
                     counter_ok, counter_ok ? 1 : 0, 1);
      }

      const bool fp_readback_ok = evaluate_u_cost_fp_invariants(fp_inv);
      if (want_fpca == 0) {
        record_check("u_cost [FP]: fpca=0 readback matched 0 (priv & user before/after)",
                     fp_readback_ok, fp_readback_ok ? 0 : 1, 0);
      } else {
        record_check("u_cost [FP]: fpca=1 readback matched 1 (priv & user before/after)",
                     fp_readback_ok, fp_readback_ok ? 1 : 0, 1);
      }

      u_cost_eval_input_t meth_eval{
          .counter_enabled = counter_enabled,
          .base_cycles = fp_base_meth_cycles,
          .measure_cycles = fp_meth_cycles,
          .final_chain_value = meth_final,
          .accumulated_error = meth_accum_err,
          .verify_value = meth_check_val,
          .verify_error = meth_check_err,
          .verify_outer_error = static_cast<uint32_t>(fp_meth_check_res.error),
          .verify_outer_value = static_cast<uint32_t>(fp_meth_check_res.value),
          .desc_create_error = desc_create_inner_error,
          .desc_create_outer_error = static_cast<uint32_t>(desc_create_res.error),
          .expected_rounds = ROUNDS,
          .outer_error = static_cast<uint32_t>(fp_meth_res.error),
          .outer_value = static_cast<uint32_t>(fp_meth_res.value),
          .executed_rounds = meth_rounds,
          .baseline_outer_error = static_cast<uint32_t>(fp_base_meth_res.error),
          .baseline_outer_value = static_cast<uint32_t>(fp_base_meth_res.value),
          .baseline_executed_rounds = meth_base_rounds,
          .warmup_outer_error = static_cast<uint32_t>(fp_warm_meth_res.error),
          .warmup_rounds = static_cast<uint32_t>(fp_warm_meth_res.value),
      };
      const bool meth_ok = evaluate_u_cost_invariants(meth_eval);
      if (want_fpca == 0) {
        record_check("u_cost [FP]: method call chain & error accumulation (fpca=0)",
                     meth_ok, meth_final, ROUNDS);
      } else {
        record_check("u_cost [FP]: method call chain & error accumulation (fpca=1)",
                     meth_ok, meth_final, ROUNDS);
      }

      u_cost_eval_input_t desc_eval{
          .counter_enabled = counter_enabled,
          .base_cycles = fp_base_desc_cycles,
          .measure_cycles = fp_desc_cycles,
          .final_chain_value = desc_final,
          .accumulated_error = desc_accum_err,
          .verify_value = desc_check_val,
          .verify_error = desc_check_err,
          .verify_outer_error = static_cast<uint32_t>(fp_desc_check_res.error),
          .verify_outer_value = static_cast<uint32_t>(fp_desc_check_res.value),
          .desc_create_error = desc_create_inner_error,
          .desc_create_outer_error = static_cast<uint32_t>(desc_create_res.error),
          .expected_rounds = ROUNDS,
          .outer_error = static_cast<uint32_t>(fp_desc_res.error),
          .outer_value = static_cast<uint32_t>(fp_desc_res.value),
          .executed_rounds = desc_rounds,
          .baseline_outer_error = static_cast<uint32_t>(fp_base_desc_res.error),
          .baseline_outer_value = static_cast<uint32_t>(fp_base_desc_res.value),
          .baseline_executed_rounds = desc_base_rounds,
          .warmup_outer_error = static_cast<uint32_t>(fp_warm_desc_res.error),
          .warmup_rounds = static_cast<uint32_t>(fp_warm_desc_res.value),
      };
      const bool desc_ok = evaluate_u_cost_invariants(desc_eval);
      if (want_fpca == 0) {
        record_check("u_cost [FP]: descriptor call chain & error accumulation (fpca=0)",
                     desc_ok, desc_final, ROUNDS);
      } else {
        record_check("u_cost [FP]: descriptor call chain & error accumulation (fpca=1)",
                     desc_ok, desc_final, ROUNDS);
      }
    };

    shizuku_rp2350_u_cost_stage = 0xC05200u;
    run_fp_condition(0, 20);

    shizuku_rp2350_u_cost_stage = 0xC05210u;
    run_fp_condition(1, 30);

    set_boot_fpca(default_fpca);

    shizuku_rp2350_u_cost_stage = 0xC050FFu;
  }
#endif
}

} // namespace shizuku::selftest
