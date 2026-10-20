#include "chips/esp32_c6/stage05_trace.h"

extern "C" {

volatile shizuku_c6_stage05_record_t shizuku_c6_stage05_record = {
    SHIZUKU_C6_STAGE05_MAGIC,
    SHIZUKU_C6_STAGE05_VERSION,
    SHIZUKU_C6_STAGE05_STATUS_UNINIT,
    SHIZUKU_C6_STAGE05_ERR_NONE,
    SHIZUKU_C6_STAGE05_ROUNDS,
    0, 0, 0,
    0, 0, 0, 0,
    0, 0, 7u, 0,
    0xFFFFFFFFu, 0, 0, 0,
    {}
};

volatile shizuku_c6_stage05_sample_t shizuku_c6_stage05_current_sample = {};
volatile shizuku_c6_stage05_calib_diag_t shizuku_c6_stage05_calib_diag = {};
volatile uint32_t shizuku_c6_stage05_active = 0;
volatile uint32_t shizuku_c6_stage05_t6 = 0;
volatile uint32_t shizuku_c6_stage05_is_return = 0;

static uint64_t s_accum_sum[SHIZUKU_C6_STAGE05_INTERVAL_COUNT] = {0};
static uint64_t s_total_sum = 0;

static const uint32_t kFromIds[SHIZUKU_C6_STAGE05_INTERVAL_COUNT] = {
    0,  // T0 caller pre
    1,  // T1 asm trap entry
    2,  // T2 C dispatch entry
    3,  // T3 kernel svc_dispatch entry
    4,  // T5 return prep (Trap 1)
    5,  // T6 asm restore begin (Trap 1)
    6,  // T4 handler entry (current_object)
    7,  // T1_ret asm trap entry
    8,  // T2_ret C dispatch entry
    9,  // T3_ret kernel svc_dispatch entry (RETURN)
    10, // T5_ret return prep (Trap 2)
    11  // T6_ret asm restore begin (Trap 2)
};

static const uint32_t kToIds[SHIZUKU_C6_STAGE05_INTERVAL_COUNT] = {
    1,  // T1 asm trap entry
    2,  // T2 C dispatch entry
    3,  // T3 kernel svc_dispatch entry
    4,  // T5 return prep (Trap 1)
    5,  // T6 asm restore begin (Trap 1)
    6,  // T4 handler entry (current_object)
    7,  // T1_ret asm trap entry
    8,  // T2_ret C dispatch entry
    9,  // T3_ret kernel svc_dispatch entry (RETURN)
    10, // T5_ret return prep (Trap 2)
    11, // T6_ret asm restore begin (Trap 2)
    12  // T7 caller post
};

void shizuku_c6_stage05_init(uint32_t placement_mode) {
    shizuku_c6_stage05_active = 0;
    shizuku_c6_stage05_t6 = 0;
    shizuku_c6_stage05_is_return = 0;
    shizuku_c6_stage05_record.magic = SHIZUKU_C6_STAGE05_MAGIC;
    shizuku_c6_stage05_record.version = SHIZUKU_C6_STAGE05_VERSION;
    shizuku_c6_stage05_record.status = SHIZUKU_C6_STAGE05_STATUS_UNINIT;
    shizuku_c6_stage05_record.error_code = SHIZUKU_C6_STAGE05_ERR_NONE;
    shizuku_c6_stage05_record.rounds_requested = SHIZUKU_C6_STAGE05_ROUNDS;
    shizuku_c6_stage05_record.rounds_completed = 0;
    shizuku_c6_stage05_record.placement_mode = placement_mode;
    shizuku_c6_stage05_record.reserved0 = 0;
    shizuku_c6_stage05_record.systimer_ticks = 0;
    shizuku_c6_stage05_record.cpu_cycle_delta = 0;
    shizuku_c6_stage05_record.cpu_mhz = 0;
    shizuku_c6_stage05_record.clock_verified = 0;
    shizuku_c6_stage05_record.hook_single_raw_min = 0xFFFFFFFFu;
    shizuku_c6_stage05_record.hook_single_raw_mean = 0;
    shizuku_c6_stage05_record.hooks_per_round = 7u;
    shizuku_c6_stage05_record.total_perturbation = 0;
    shizuku_c6_stage05_record.total_raw_min = 0xFFFFFFFFu;
    shizuku_c6_stage05_record.total_raw_mean = 0;
    shizuku_c6_stage05_record.total_net_min = 0;
    shizuku_c6_stage05_record.total_net_mean = 0;

    for (uint32_t i = 0; i < SHIZUKU_C6_STAGE05_INTERVAL_COUNT; ++i) {
        shizuku_c6_stage05_record.intervals[i].from_id = kFromIds[i];
        shizuku_c6_stage05_record.intervals[i].to_id = kToIds[i];
        shizuku_c6_stage05_record.intervals[i].raw_min = 0xFFFFFFFFu;
        shizuku_c6_stage05_record.intervals[i].raw_mean = 0;
        shizuku_c6_stage05_record.intervals[i].net_min = 0;
        shizuku_c6_stage05_record.intervals[i].net_mean = 0;
        s_accum_sum[i] = 0;
    }
    s_total_sum = 0;
}

void shizuku_c6_stage05_calibrate_hooks(void) {
    // 1. csrr 0x7E2 単体読み取りオーバヘッド (fence 挟み2回読み差分) の測定
    uint32_t csrr_min = 0xFFFFFFFFu;
    uint64_t csrr_total = 0;
    for (uint32_t r = 0; r < 128; ++r) {
        uint32_t c1 = 0, c2 = 0;
#if defined(__riscv)
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c1) :: "memory");
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c2) :: "memory");
#endif
        const uint32_t diff = c2 - c1;
        if (diff < csrr_min) csrr_min = diff;
        csrr_total += diff;
    }
    const uint32_t csrr_mean = (uint32_t)(csrr_total / 128u);

    // 2. 実フック経路の測定 (active=1 で実行し、関数呼出し・active分岐・csrr・store を含めた実費を計測)
    //    校正中は active を 1 に設定する (0 のままではフックが即 return してしまうため)
    shizuku_c6_stage05_active = 1u;

    // (A) hook_trap_entry
    uint32_t te_min = 0xFFFFFFFFu;
    uint64_t te_total = 0;
    for (uint32_t r = 0; r < 128; ++r) {
        uint32_t c1 = 0, c2 = 0;
#if defined(__riscv)
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c1) :: "memory");
        shizuku_c6_stage05_hook_trap_entry(0, 0);
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c2) :: "memory");
#else
        shizuku_c6_stage05_hook_trap_entry(0, 0);
#endif
        const uint32_t diff = c2 - c1;
        if (diff < te_min) te_min = diff;
        te_total += diff;
    }
    const uint32_t te_mean = (uint32_t)(te_total / 128u);
    const uint32_t te_net_min = (te_min > csrr_min) ? (te_min - csrr_min) : 0;
    const uint32_t te_net_mean = (te_mean > csrr_mean) ? (te_mean - csrr_mean) : 0;

    // (B) hook_svc_dispatch
    uint32_t sd_min = 0xFFFFFFFFu;
    uint64_t sd_total = 0;
    for (uint32_t r = 0; r < 128; ++r) {
        uint32_t c1 = 0, c2 = 0;
#if defined(__riscv)
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c1) :: "memory");
        shizuku_c6_stage05_hook_svc_dispatch(0);
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c2) :: "memory");
#else
        shizuku_c6_stage05_hook_svc_dispatch(0);
#endif
        const uint32_t diff = c2 - c1;
        if (diff < sd_min) sd_min = diff;
        sd_total += diff;
    }
    const uint32_t sd_mean = (uint32_t)(sd_total / 128u);
    const uint32_t sd_net_min = (sd_min > csrr_min) ? (sd_min - csrr_min) : 0;
    const uint32_t sd_net_mean = (sd_mean > csrr_mean) ? (sd_mean - csrr_mean) : 0;

    // (C) hook_handler
    uint32_t hd_min = 0xFFFFFFFFu;
    uint64_t hd_total = 0;
    for (uint32_t r = 0; r < 128; ++r) {
        uint32_t c1 = 0, c2 = 0;
#if defined(__riscv)
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c1) :: "memory");
        shizuku_c6_stage05_hook_handler();
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c2) :: "memory");
#else
        shizuku_c6_stage05_hook_handler();
#endif
        const uint32_t diff = c2 - c1;
        if (diff < hd_min) hd_min = diff;
        hd_total += diff;
    }
    const uint32_t hd_mean = (uint32_t)(hd_total / 128u);
    const uint32_t hd_net_min = (hd_min > csrr_min) ? (hd_min - csrr_min) : 0;
    const uint32_t hd_net_mean = (hd_mean > csrr_mean) ? (hd_mean - csrr_mean) : 0;

    // (D) hook_return_prep
    uint32_t rp_min = 0xFFFFFFFFu;
    uint64_t rp_total = 0;
    for (uint32_t r = 0; r < 128; ++r) {
        uint32_t c1 = 0, c2 = 0;
#if defined(__riscv)
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c1) :: "memory");
        shizuku_c6_stage05_hook_return_prep(0);
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c2) :: "memory");
#else
        shizuku_c6_stage05_hook_return_prep(0);
#endif
        const uint32_t diff = c2 - c1;
        if (diff < rp_min) rp_min = diff;
        rp_total += diff;
    }
    const uint32_t rp_mean = (uint32_t)(rp_total / 128u);
    const uint32_t rp_net_min = (rp_min > csrr_min) ? (rp_min - csrr_min) : 0;
    const uint32_t rp_net_mean = (rp_mean > csrr_mean) ? (rp_mean - csrr_mean) : 0;

    // レコードに入らない中間値を診断用構造体に退避 (BOARD::diag_printf で出力)
    shizuku_c6_stage05_calib_diag.csrr_min = csrr_min;
    shizuku_c6_stage05_calib_diag.csrr_mean = csrr_mean;
    shizuku_c6_stage05_calib_diag.hook_te_raw_min = te_min;
    shizuku_c6_stage05_calib_diag.hook_te_raw_mean = te_mean;
    shizuku_c6_stage05_calib_diag.hook_te_net_min = te_net_min;
    shizuku_c6_stage05_calib_diag.hook_te_net_mean = te_net_mean;
    shizuku_c6_stage05_calib_diag.hook_sd_raw_min = sd_min;
    shizuku_c6_stage05_calib_diag.hook_sd_raw_mean = sd_mean;
    shizuku_c6_stage05_calib_diag.hook_sd_net_min = sd_net_min;
    shizuku_c6_stage05_calib_diag.hook_sd_net_mean = sd_net_mean;
    shizuku_c6_stage05_calib_diag.hook_hd_raw_min = hd_min;
    shizuku_c6_stage05_calib_diag.hook_hd_raw_mean = hd_mean;
    shizuku_c6_stage05_calib_diag.hook_hd_net_min = hd_net_min;
    shizuku_c6_stage05_calib_diag.hook_hd_net_mean = hd_net_mean;
    shizuku_c6_stage05_calib_diag.hook_rp_raw_min = rp_min;
    shizuku_c6_stage05_calib_diag.hook_rp_raw_mean = rp_mean;
    shizuku_c6_stage05_calib_diag.hook_rp_net_min = rp_net_min;
    shizuku_c6_stage05_calib_diag.hook_rp_net_mean = rp_net_mean;

    // 4種の中で最大の net min / mean を代表単一フック費用とする
    uint32_t max_hook_min = te_net_min;
    if (sd_net_min > max_hook_min) max_hook_min = sd_net_min;
    if (hd_net_min > max_hook_min) max_hook_min = hd_net_min;
    if (rp_net_min > max_hook_min) max_hook_min = rp_net_min;

    uint32_t max_hook_mean = te_net_mean;
    if (sd_net_mean > max_hook_mean) max_hook_mean = sd_net_mean;
    if (hd_net_mean > max_hook_mean) max_hook_mean = hd_net_mean;
    if (rp_net_mean > max_hook_mean) max_hook_mean = rp_net_mean;

    shizuku_c6_stage05_record.hook_single_raw_min = max_hook_min;
    shizuku_c6_stage05_record.hook_single_raw_mean = max_hook_mean;
    shizuku_c6_stage05_record.hooks_per_round = 7u;

    // 摂動合計 (total_perturbation) の算出:
    // そのラウンドで実際に実行されるフック回数 × 該当フックの min:
    //   Trap 1: hook_trap_entry 1回 + hook_svc_dispatch 1回 + hook_return_prep 1回 + asm csrr 2回 (T1, T6)
    //   Handler: hook_handler 1回
    //   Trap 2: hook_trap_entry 1回 + hook_svc_dispatch 1回 + hook_return_prep 1回 + asm csrr 2回 (T1, T6)
    // 合計 7 Cフック + 4 asm csrr:
    //   total_perturbation = (te_net_min * 2) + (sd_net_min * 2) + (rp_net_min * 2) + (hd_net_min * 1) + (csrr_min * 4)
    shizuku_c6_stage05_record.total_perturbation =
        (te_net_min * 2u) + (sd_net_min * 2u) + (rp_net_min * 2u) + (hd_net_min * 1u) + (csrr_min * 4u);

    // 校正完了後は active を 0 に戻し、current_sample をゼロ初期化して本番計測への影響を残さない
    shizuku_c6_stage05_active = 0u;
    shizuku_c6_stage05_current_sample.t0_caller_pre = 0;
    shizuku_c6_stage05_current_sample.t1_trap_entry = 0;
    shizuku_c6_stage05_current_sample.t2_c_dispatch = 0;
    shizuku_c6_stage05_current_sample.t3_svc_dispatch = 0;
    shizuku_c6_stage05_current_sample.t5_return_prep = 0;
    shizuku_c6_stage05_current_sample.t6_restore_begin = 0;
    shizuku_c6_stage05_current_sample.t4_handler_entry = 0;
    shizuku_c6_stage05_current_sample.t1_ret_trap_entry = 0;
    shizuku_c6_stage05_current_sample.t2_ret_c_dispatch = 0;
    shizuku_c6_stage05_current_sample.t3_ret_svc_dispatch = 0;
    shizuku_c6_stage05_current_sample.t5_ret_return_prep = 0;
    shizuku_c6_stage05_current_sample.t6_ret_restore_begin = 0;
    shizuku_c6_stage05_current_sample.t7_caller_post = 0;
    shizuku_c6_stage05_t6 = 0u;
    shizuku_c6_stage05_is_return = 0u;
}

bool shizuku_c6_stage05_record_sample(const shizuku_c6_stage05_sample_t *s) {
    // タイムスタンプ順序の検証:
    // T0 <= T1 <= T2 <= T3 <= T5 <= T6 <= T4 <= T1_ret <= T2_ret <= T3_ret <= T5_ret <= T6_ret <= T7
    const uint32_t ts[13] = {
        s->t0_caller_pre,
        s->t1_trap_entry,
        s->t2_c_dispatch,
        s->t3_svc_dispatch,
        s->t5_return_prep,
        s->t6_restore_begin,
        s->t4_handler_entry,
        s->t1_ret_trap_entry,
        s->t2_ret_c_dispatch,
        s->t3_ret_svc_dispatch,
        s->t5_ret_return_prep,
        s->t6_ret_restore_begin,
        s->t7_caller_post
    };

    uint32_t deltas[SHIZUKU_C6_STAGE05_INTERVAL_COUNT];
    for (uint32_t i = 0; i < SHIZUKU_C6_STAGE05_INTERVAL_COUNT; ++i) {
        const uint32_t diff = ts[i + 1] - ts[i];
        // タイムスタンプ逆行 (wrap/overflow) または異常値 (> 200,000 cyc) はフェイルクローズ
        if (diff > 200000u) {
            shizuku_c6_stage05_record.error_code = SHIZUKU_C6_STAGE05_ERR_SEQUENCE_ORDER;
            shizuku_c6_stage05_record.status = SHIZUKU_C6_STAGE05_STATUS_FAILED;
            return false;
        }
        deltas[i] = diff;
    }

    const uint32_t roundtrip = ts[12] - ts[0];
    if (roundtrip == 0 || roundtrip > 500000u) {
        shizuku_c6_stage05_record.error_code = SHIZUKU_C6_STAGE05_ERR_COUNTER_FROZEN;
        shizuku_c6_stage05_record.status = SHIZUKU_C6_STAGE05_STATUS_FAILED;
        return false;
    }

    for (uint32_t i = 0; i < SHIZUKU_C6_STAGE05_INTERVAL_COUNT; ++i) {
        if (deltas[i] < shizuku_c6_stage05_record.intervals[i].raw_min) {
            shizuku_c6_stage05_record.intervals[i].raw_min = deltas[i];
        }
        s_accum_sum[i] += deltas[i];
    }
    if (roundtrip < shizuku_c6_stage05_record.total_raw_min) {
        shizuku_c6_stage05_record.total_raw_min = roundtrip;
    }
    s_total_sum += roundtrip;

    shizuku_c6_stage05_record.rounds_completed = shizuku_c6_stage05_record.rounds_completed + 1;
    return true;
}

void shizuku_c6_stage05_finish(void) {
    const uint32_t rounds = shizuku_c6_stage05_record.rounds_completed;
    if (rounds < SHIZUKU_C6_STAGE05_ROUNDS) {
        shizuku_c6_stage05_record.status = SHIZUKU_C6_STAGE05_STATUS_FAILED;
        shizuku_c6_stage05_record.error_code = SHIZUKU_C6_STAGE05_ERR_SAMPLE_SHORTAGE;
        return;
    }

    const uint32_t hook_cost = shizuku_c6_stage05_record.hook_single_raw_min;
    const uint32_t hook_mean = shizuku_c6_stage05_record.hook_single_raw_mean;

    for (uint32_t i = 0; i < SHIZUKU_C6_STAGE05_INTERVAL_COUNT; ++i) {
        const uint32_t raw_min = shizuku_c6_stage05_record.intervals[i].raw_min;
        const uint32_t raw_mean = (uint32_t)(s_accum_sum[i] / rounds);
        shizuku_c6_stage05_record.intervals[i].raw_mean = raw_mean;
        shizuku_c6_stage05_record.intervals[i].net_min = (raw_min > hook_cost) ? (raw_min - hook_cost) : 0;
        shizuku_c6_stage05_record.intervals[i].net_mean = (raw_mean > hook_mean) ? (raw_mean - hook_mean) : 0;
    }

    const uint32_t total_min = shizuku_c6_stage05_record.total_raw_min;
    const uint32_t total_mean = (uint32_t)(s_total_sum / rounds);
    const uint32_t total_hook = shizuku_c6_stage05_record.total_perturbation;
    shizuku_c6_stage05_record.total_raw_mean = total_mean;
    shizuku_c6_stage05_record.total_net_min = (total_min > total_hook) ? (total_min - total_hook) : 0;
    shizuku_c6_stage05_record.total_net_mean = (total_mean > total_hook) ? (total_mean - total_hook) : 0;

    if (shizuku_c6_stage05_record.error_code == SHIZUKU_C6_STAGE05_ERR_NONE) {
        shizuku_c6_stage05_record.status = SHIZUKU_C6_STAGE05_STATUS_PASSED;
    } else {
        shizuku_c6_stage05_record.status = SHIZUKU_C6_STAGE05_STATUS_FAILED;
    }
}

// ---------------------------------------------------------------------------
// 測定フック本体 (SHIZUKU_STAGE05_ATTR 配置)
// ---------------------------------------------------------------------------

SHIZUKU_STAGE05_ATTR void shizuku_c6_stage05_hook_trap_entry(uint32_t t1, uint32_t is_return) {
    if (!shizuku_c6_stage05_active) return;
    uint32_t t2 = 0;
#if defined(__riscv)
    asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(t2) :: "memory");
#endif
    if (is_return) {
        shizuku_c6_stage05_current_sample.t1_ret_trap_entry = t1;
        shizuku_c6_stage05_current_sample.t2_ret_c_dispatch = t2;
    } else {
        shizuku_c6_stage05_current_sample.t1_trap_entry = t1;
        shizuku_c6_stage05_current_sample.t2_c_dispatch = t2;
    }
}

SHIZUKU_STAGE05_ATTR void shizuku_c6_stage05_hook_svc_dispatch(uint32_t is_return) {
    if (!shizuku_c6_stage05_active) return;
    uint32_t t3 = 0;
#if defined(__riscv)
    asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(t3) :: "memory");
#endif
    if (is_return) {
        shizuku_c6_stage05_current_sample.t3_ret_svc_dispatch = t3;
    } else {
        shizuku_c6_stage05_current_sample.t3_svc_dispatch = t3;
    }
}

SHIZUKU_STAGE05_ATTR void shizuku_c6_stage05_hook_handler(void) {
    if (!shizuku_c6_stage05_active) return;
    uint32_t t4 = 0;
#if defined(__riscv)
    asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(t4) :: "memory");
#endif
    shizuku_c6_stage05_current_sample.t4_handler_entry = t4;
    shizuku_c6_stage05_current_sample.t6_restore_begin = shizuku_c6_stage05_t6;
}

SHIZUKU_STAGE05_ATTR void shizuku_c6_stage05_hook_return_prep(uint32_t is_return) {
    if (!shizuku_c6_stage05_active) return;
    uint32_t t5 = 0;
#if defined(__riscv)
    asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(t5) :: "memory");
#endif
    if (is_return) {
        shizuku_c6_stage05_current_sample.t5_ret_return_prep = t5;
    } else {
        shizuku_c6_stage05_current_sample.t5_return_prep = t5;
    }
}

} // extern "C"
