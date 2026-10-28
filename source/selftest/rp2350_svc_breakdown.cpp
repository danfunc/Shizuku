// ===========================================================================
//  RP2350 (ARMv8-M): S0 SVC 往復の内訳実測 (DWT CYCCNT による各区間計測)
// ===========================================================================
//  C++ SVC handler の probe (0..6) の各区間を記録・出力する。
//  旧 asm S0 の同名フィールドとは区間が異なり、数値を混ぜて比較しない。
//
//  ★残差 (Residual) の定義:
//    呼び手 roundtrip net からハンドラ内測定区間 (probe 0..6) を引いた値。
//    ここには HW 例外突入スタッキング (PSP への 8 語退避)、HW 例外復帰アンスタッキング、
//    および各 probe の呼出し・計測オーバーヘッドが含まれる。
//    負の残差が生じても 0 に丸めず、そのまま記録・報告する。
//  no-switch getter は probe 3..6 を連続して実行し、復帰準備を行わない。

#include <cstdint>
#include <cstddef>
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/svc_breakdown_probe.hpp"

namespace shizuku::selftest {

static constexpr size_t NUM_POINTS = SVC_BREAKDOWN_POINT_COUNT;
static constexpr uint32_t ROUNDS = 128;

static svc_breakdown_probe_state_t g_probe_states[2] = {};

template <typename ARCH>
static uint32_t read_current_psp() {
  if constexpr (ARCH::IS_ARMV8M) {
    uint32_t psp = 0;
    asm volatile("mrs %0, psp" : "=r"(psp));
    return psp;
  }
  return 0;
}

template <typename ARCH>
static constexpr bool svc_breakdown_build_enabled() {
  if constexpr (requires { ARCH::SVC_BREAKDOWN_ENABLED; })
    return ARCH::SVC_BREAKDOWN_ENABLED;
  return false;
}

extern "C" void shizuku_svc_breakdown_probe(uint32_t point, uint32_t exc_return,
                                              uint32_t api_number) {
  const uint32_t core = shizuku::KERNEL::BOARD::core_num();
  if (core >= 2) return;
  auto &state = g_probe_states[core];
  if (point == 0 && !state.armed) return;
  if (point != 0 && !state.active) return;
  const uintptr_t owner = shizuku::kernel_instance.current_thread().identity_object();
  const uintptr_t context = reinterpret_cast<uintptr_t>(shizuku::kernel_instance.current_context());
  const uint32_t psp = read_current_psp<typename shizuku::KERNEL::ARCH>();
  const uint32_t now = *reinterpret_cast<volatile uint32_t *>(0xe0001004);
  observe_svc_breakdown_probe(state, point, api_number, owner, context,
                              exc_return, psp, now);
}

struct api_result_t {
  uintptr_t error;
  uintptr_t value;
};

template <typename ARCH>
[[gnu::always_inline]] static inline api_result_t issue_api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
                                                            uintptr_t a3 = 0) {
  const auto result = ARCH::syscall(static_cast<uintptr_t>(number), a1, a2, a3);
  if (result.error & KERNEL_ERROR_MARK)
    return {static_cast<uintptr_t>(object_error::KERNEL_REFUSED), result.error};
  return {result.error, result.value};
}

extern "C" {
struct shizuku_rp2350_svc_breakdown_record_t {
  uint32_t magic;          // 0x53564342 ("SVCB")
  uint32_t version;        // 2: record layout; see diagnostic banner for boundaries
  uint32_t rounds;         // 128
  uint32_t total_mean;
  uint32_t total_min;
  uint32_t handler_mean;
  uint32_t handler_min;
  int32_t  residual_mean;  // signed residual
  int32_t  residual_min;
  uint32_t ctx_save_mean;  // legacy field name: probe 0->1 setup
  uint32_t ctx_save_min;
  uint32_t dispatch_mean;
  uint32_t dispatch_min;
  uint32_t pending_step_mean;
  uint32_t pending_step_min;
  uint32_t restore_region_mean;
  uint32_t restore_region_min;
  uint32_t current_context_mean; // legacy field name: probe 4->5
  uint32_t current_context_min;
  uint32_t reg_restore_mean;
  uint32_t reg_restore_min;
  uint32_t all_probes_hit_ok;
  uint32_t sequence_monotonic_ok;
  uint32_t result_ok;
};

__attribute__((used)) volatile shizuku_rp2350_svc_breakdown_record_t shizuku_rp2350_svc_breakdown_record = {
    .magic = 0x53564342u,
    .version = 2u,
    .rounds = ROUNDS,
    .total_mean = 0,
    .total_min = 0,
    .handler_mean = 0,
    .handler_min = 0,
    .residual_mean = 0,
    .residual_min = 0,
    .ctx_save_mean = 0,
    .ctx_save_min = 0,
    .dispatch_mean = 0,
    .dispatch_min = 0,
    .pending_step_mean = 0,
    .pending_step_min = 0,
    .restore_region_mean = 0,
    .restore_region_min = 0,
    .current_context_mean = 0,
    .current_context_min = 0,
    .reg_restore_mean = 0,
    .reg_restore_min = 0,
    .all_probes_hit_ok = 0,
    .sequence_monotonic_ok = 0,
    .result_ok = 0,
};
}

template <typename BOARD_TYPE>
static void record_check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    passed++;
  } else {
    failed++;
    record_fail(name, got, want);
  }
  BOARD_TYPE::diag_printf("[SVC-BREAKDOWN] %s %s (got=%lu want=%lu)\n",
                     ok ? "PASS" : "FAIL", name, got, want);
  if constexpr (requires { BOARD_TYPE::diag_drain(); }) BOARD_TYPE::diag_drain();
}

template <typename KERNEL_TYPE = shizuku::KERNEL>
void run_rp2350_svc_breakdown() {
  using BOARD = typename KERNEL_TYPE::BOARD;
  using ARCH = typename KERNEL_TYPE::ARCH;

  if constexpr (ARCH::IS_ARMV8M && ARCH::HAS_CYCLE_COUNTER &&
                svc_breakdown_build_enabled<ARCH>()) {
    // コア0のみを対象とする
    if (BOARD::core_num() != 0) {
      BOARD::diag_printf("[SVC-BREAKDOWN] Skipped on secondary core\n");
      return;
    }

    const bool counter_enabled = ARCH::cycle_counter_enable();
    if (!counter_enabled) {
      BOARD::diag_printf("[SVC-BREAKDOWN] FAIL: DWT cycle counter could not be enabled\n");
      record_check<BOARD>("rp2350_svc_breakdown: DWT cycle counter enabled", false, 0, 1);
      return;
    }

    // 測定枠の baseline (read_cycle_counter 読み取りオーバーヘッド)
    uint32_t base_overhead = 0;
    {
      uint32_t b_sum = 0;
      for (uint32_t i = 0; i < 32; ++i) {
        uint32_t t0 = ARCH::cycle_counter_read();
        __asm__ volatile("" ::: "memory");
        uint32_t t1 = ARCH::cycle_counter_read();
        b_sum += (t1 - t0);
      }
      base_overhead = b_sum / 32;
    }

    // ウォームアップ (16回)
    for (uint32_t i = 0; i < 16; ++i) {
      auto res = issue_api<ARCH>(object_api::GET_CURRENT_OBJECT);
      (void)res;
    }

    // 測定セッション全体を初期化 (core0 専用 state のみを触る)
    g_probe_states[0] = {};

    uint64_t sum_total = 0;
    uint32_t min_total = 0xFFFFFFFFu;
    uint64_t sum_handler = 0;
    uint32_t min_handler = 0xFFFFFFFFu;
    int64_t  sum_residual = 0;
    int32_t  min_residual = 0x7FFFFFFF;
    uint64_t sum_ctx_save = 0;
    uint32_t min_ctx_save = 0xFFFFFFFFu;
    uint64_t sum_dispatch = 0;
    uint32_t min_dispatch = 0xFFFFFFFFu;
    uint64_t sum_pending_step = 0;
    uint32_t min_pending_step = 0xFFFFFFFFu;
    uint64_t sum_restore_region = 0;
    uint32_t min_restore_region = 0xFFFFFFFFu;
    uint64_t sum_current_context = 0;
    uint32_t min_current_context = 0xFFFFFFFFu;
    uint64_t sum_reg_restore = 0;
    uint32_t min_reg_restore = 0xFFFFFFFFu;

    bool sequence_ok = true;
    bool return_result_ok = true;

    for (uint32_t round = 0; round < ROUNDS; ++round) {
      const uintptr_t expected_owner = kernel_instance.current_thread().identity_object();
      const uintptr_t expected_context = reinterpret_cast<uintptr_t>(kernel_instance.current_context());
      arm_svc_breakdown_probe(g_probe_states[0], expected_owner, expected_context);
      const uint32_t t_start = ARCH::cycle_counter_read();
      auto res = issue_api<ARCH>(object_api::GET_CURRENT_OBJECT);
      const uint32_t t_end = ARCH::cycle_counter_read();

      if (res.error != static_cast<uintptr_t>(object_error::OK)) {
        return_result_ok = false;
      }
      const bool sample_ok = svc_breakdown_sample_ok(g_probe_states[0], round + 1);
      if (res.value != expected_owner || g_probe_states[0].owner != expected_owner || !sample_ok)
        return_result_ok = false;
      disarm_svc_breakdown_probe(g_probe_states[0]);

      const auto &timestamps = g_probe_states[0].timestamps;
      for (size_t p = 0; p + 1 < NUM_POINTS; ++p)
        if (!timestamp_delta_valid(timestamps[p], timestamps[p + 1])) sequence_ok = false;

      const uint32_t raw_roundtrip = t_end - t_start;
      const uint32_t net_roundtrip = (raw_roundtrip >= base_overhead) ? (raw_roundtrip - base_overhead) : raw_roundtrip;

      const uint32_t d_ctx_save = timestamps[1] - timestamps[0];
      const uint32_t d_dispatch = timestamps[2] - timestamps[1];
      const uint32_t d_pending_step = timestamps[3] - timestamps[2];
      const uint32_t d_restore_region = timestamps[4] - timestamps[3];
      const uint32_t d_current_context = timestamps[5] - timestamps[4];
      const uint32_t d_reg_restore = timestamps[6] - timestamps[5];
      const uint32_t d_handler = timestamps[6] - timestamps[0];
      const int32_t  d_residual = static_cast<int32_t>(net_roundtrip) - static_cast<int32_t>(d_handler);

      sum_total += net_roundtrip;
      if (net_roundtrip < min_total) min_total = net_roundtrip;

      sum_handler += d_handler;
      if (d_handler < min_handler) min_handler = d_handler;

      sum_residual += d_residual;
      if (d_residual < min_residual) min_residual = d_residual;

      sum_ctx_save += d_ctx_save;
      if (d_ctx_save < min_ctx_save) min_ctx_save = d_ctx_save;

      sum_dispatch += d_dispatch;
      if (d_dispatch < min_dispatch) min_dispatch = d_dispatch;

      sum_pending_step += d_pending_step;
      if (d_pending_step < min_pending_step) min_pending_step = d_pending_step;

      sum_restore_region += d_restore_region;
      if (d_restore_region < min_restore_region) min_restore_region = d_restore_region;

      sum_current_context += d_current_context;
      if (d_current_context < min_current_context) min_current_context = d_current_context;

      sum_reg_restore += d_reg_restore;
      if (d_reg_restore < min_reg_restore) min_reg_restore = d_reg_restore;
    }

    bool all_hits_ok = true;
    for (size_t i = 0; i < NUM_POINTS; ++i) {
      if (g_probe_states[0].hits[i] != ROUNDS) {
        all_hits_ok = false;
      }
    }
    all_hits_ok = all_hits_ok && svc_breakdown_sample_ok(g_probe_states[0], ROUNDS);

    shizuku_rp2350_svc_breakdown_record.total_mean = static_cast<uint32_t>(sum_total / ROUNDS);
    shizuku_rp2350_svc_breakdown_record.total_min = min_total;
    shizuku_rp2350_svc_breakdown_record.handler_mean = static_cast<uint32_t>(sum_handler / ROUNDS);
    shizuku_rp2350_svc_breakdown_record.handler_min = min_handler;
    shizuku_rp2350_svc_breakdown_record.residual_mean = static_cast<int32_t>(sum_residual / static_cast<int64_t>(ROUNDS));
    shizuku_rp2350_svc_breakdown_record.residual_min = min_residual;
    shizuku_rp2350_svc_breakdown_record.ctx_save_mean = static_cast<uint32_t>(sum_ctx_save / ROUNDS);
    shizuku_rp2350_svc_breakdown_record.ctx_save_min = min_ctx_save;
    shizuku_rp2350_svc_breakdown_record.dispatch_mean = static_cast<uint32_t>(sum_dispatch / ROUNDS);
    shizuku_rp2350_svc_breakdown_record.dispatch_min = min_dispatch;
    shizuku_rp2350_svc_breakdown_record.pending_step_mean = static_cast<uint32_t>(sum_pending_step / ROUNDS);
    shizuku_rp2350_svc_breakdown_record.pending_step_min = min_pending_step;
    shizuku_rp2350_svc_breakdown_record.restore_region_mean = static_cast<uint32_t>(sum_restore_region / ROUNDS);
    shizuku_rp2350_svc_breakdown_record.restore_region_min = min_restore_region;
    shizuku_rp2350_svc_breakdown_record.current_context_mean = static_cast<uint32_t>(sum_current_context / ROUNDS);
    shizuku_rp2350_svc_breakdown_record.current_context_min = min_current_context;
    shizuku_rp2350_svc_breakdown_record.reg_restore_mean = static_cast<uint32_t>(sum_reg_restore / ROUNDS);
    shizuku_rp2350_svc_breakdown_record.reg_restore_min = min_reg_restore;
    shizuku_rp2350_svc_breakdown_record.all_probes_hit_ok = all_hits_ok ? 1u : 0u;
    shizuku_rp2350_svc_breakdown_record.sequence_monotonic_ok = sequence_ok ? 1u : 0u;
    shizuku_rp2350_svc_breakdown_record.result_ok = return_result_ok ? 1u : 0u;

    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN] ========================================\n");
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN] C++ SVC probes; old asm S0 intervals differ (N=%u)\n", (unsigned)ROUNDS);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN] Target: GET_CURRENT_OBJECT (SVC 0)\n");
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN] ----------------------------------------\n");
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN] Total Caller Roundtrip: mean=%lu cyc, min=%lu cyc\n",
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.total_mean,
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.total_min);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN]   In-Handler Total    : mean=%lu cyc, min=%lu cyc\n",
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.handler_mean,
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.handler_min);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN]   Residual (HW entry/exit+probe overhead): mean=%ld cyc, min=%ld cyc\n",
                       (long)shizuku_rp2350_svc_breakdown_record.residual_mean,
                       (long)shizuku_rp2350_svc_breakdown_record.residual_min);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN] Handler Breakdown:\n");
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN]   1. probe 0->1 setup                  : mean=%lu cyc, min=%lu cyc\n",
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.ctx_save_mean,
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.ctx_save_min);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN]   2. Dispatcher (svc_dispatch)        : mean=%lu cyc, min=%lu cyc\n",
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.dispatch_mean,
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.dispatch_min);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN]   3. probe 2->3 decision               : mean=%lu cyc, min=%lu cyc\n",
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.pending_step_mean,
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.pending_step_min);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN]   4. probe 3->4 prepare if switched    : mean=%lu cyc, min=%lu cyc\n",
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.restore_region_mean,
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.restore_region_min);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN]   5. probe 4->5 control if switched    : mean=%lu cyc, min=%lu cyc\n",
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.current_context_mean,
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.current_context_min);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN]   6. probe 5->6 state if switched      : mean=%lu cyc, min=%lu cyc\n",
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.reg_restore_mean,
                       (unsigned long)shizuku_rp2350_svc_breakdown_record.reg_restore_min);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN] Sample checks: hits=%s, monotonic=%s, result=%s (hits: 0:%lu, 1:%lu, 2:%lu, 3:%lu, 4:%lu, 5:%lu, 6:%lu)\n",
                       all_hits_ok ? "PASS" : "FAIL",
                       sequence_ok ? "PASS" : "FAIL",
                       return_result_ok ? "PASS" : "FAIL",
                       (unsigned long)g_probe_states[0].hits[0], (unsigned long)g_probe_states[0].hits[1],
                       (unsigned long)g_probe_states[0].hits[2], (unsigned long)g_probe_states[0].hits[3],
                       (unsigned long)g_probe_states[0].hits[4], (unsigned long)g_probe_states[0].hits[5],
                       (unsigned long)g_probe_states[0].hits[6]);
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();
    BOARD::diag_printf("[SVC-BREAKDOWN] ========================================\n");
    if constexpr (requires { BOARD::diag_drain(); }) BOARD::diag_drain();

    record_check<BOARD>("rp2350_svc_breakdown: all 7 probes hit exactly", all_hits_ok, all_hits_ok ? 1 : 0, 1);
    record_check<BOARD>("rp2350_svc_breakdown: probe timestamps monotonic", sequence_ok, sequence_ok ? 1 : 0, 1);
    record_check<BOARD>("rp2350_svc_breakdown: target SVC returned and every sample complete",
                 return_result_ok, return_result_ok ? ROUNDS : 0, ROUNDS);
  }
}

void rp2350_svc_breakdown() {
  run_rp2350_svc_breakdown<>();
}

} // namespace shizuku::selftest
