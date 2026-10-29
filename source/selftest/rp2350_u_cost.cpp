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
uintptr_t u_cost_child_worker(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
void u_cost_code_begin();
void u_cost_code_end_pad();
extern const char u_cost_code_end[];
void u_cost_probe_code_access();
void u_cost_probe_data_access();
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
  asm volatile(".syntax unified\n.thumb\nldr r0, =u_cost_denied_code\nbx r0\n");
}

SHIZUKU_U_COST_CODE __attribute__((naked)) void u_cost_probe_data_access() {
  asm volatile(
      ".syntax unified\n.thumb\n"
      "ldr r0, =u_cost_p_stack\n"
      ".global u_cost_probe_data_fault_pc\n"
      "u_cost_probe_data_fault_pc:\n"
      "ldr r0, [r0]\n"
      "bx lr\n");
}

// Registered only after timing so the measured child worker stays two instructions.
SHIZUKU_U_COST_CODE uintptr_t u_cost_denied_probe_worker(uintptr_t mode, uintptr_t,
                                                        uintptr_t, uintptr_t) {
  if (mode == 90) u_cost_probe_code_access();
  if (mode == 91) u_cost_probe_data_access();
  return 0;
}

// This function is intentionally in a separate SRAM input section and is never called
// by privileged code. The U probe attempts to execute it while only u_cost is enabled.
__attribute__((used, noinline, aligned(32), section(".time_critical.u_cost_denied"), no_reorder, naked))
void u_cost_denied_code() {
  asm volatile(".syntax unified\n.thumb\nbx lr\n");
}

SHIZUKU_U_COST_CODE uintptr_t u_cost_parent_worker(uintptr_t cmd, uintptr_t, uintptr_t, uintptr_t) {
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

// 最後のliteral poolを窓内にflushしてから32B境界へalignし、window終端を置く。
SHIZUKU_U_COST_CODE __attribute__((naked)) void u_cost_code_end_pad() {
  asm volatile(".ltorg\n.balign 32\n.global u_cost_code_end\nu_cost_code_end:\n");
}

#else
// ホスト環境用スタブ
extern "C" {
uintptr_t u_cost_parent_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
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
      w.before_core = A::core_slot();
      w.before = cost_probe::routes<true>::snapshot(w.before_core);
    }
  }
  template<class A> static void end(unsigned i) {
    exception_windows::end(i);
    if constexpr (Enabled) {
      auto &w = windows[i];
      w.after_core = A::core_slot();
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
    shizuku_rp2350_u_cost_stage = 0xC050FFu;
  }
#endif
}

} // namespace shizuku::selftest
