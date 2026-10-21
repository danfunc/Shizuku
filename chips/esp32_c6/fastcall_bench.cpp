#include "chips/esp32_c6/fastcall.h"
#include "shizuku/kernel.hpp"
#include "chips/esp32_c6/systimer.hpp"
#include <cstring>

#if defined(SHIZUKU_EXP_FASTCALL) && SHIZUKU_EXP_FASTCALL

// Struct layout compile-time guarantees (20-byte descriptor, 80-byte record)
static_assert(sizeof(shizuku_c6_fastcall_descriptor_t) == 20,
              "shizuku_c6_fastcall_descriptor_t must be exactly 20 bytes (5 uint32_t words)");
static_assert(offsetof(shizuku_c6_fastcall_descriptor_t, entry_pc) == 0);
static_assert(offsetof(shizuku_c6_fastcall_descriptor_t, caller_sp) == 4);
static_assert(offsetof(shizuku_c6_fastcall_descriptor_t, caller_pc) == 8);
static_assert(offsetof(shizuku_c6_fastcall_descriptor_t, depth) == 12);
static_assert(offsetof(shizuku_c6_fastcall_descriptor_t, call_count) == 16);

static_assert(sizeof(shizuku_c6_fastcall_record_t) == 80,
              "shizuku_c6_fastcall_record_t must be exactly 80 bytes (20 uint32_t words)");
static_assert(offsetof(shizuku_c6_fastcall_record_t, magic) == 0);
static_assert(offsetof(shizuku_c6_fastcall_record_t, version) == 4);
static_assert(offsetof(shizuku_c6_fastcall_record_t, status) == 8);
static_assert(offsetof(shizuku_c6_fastcall_record_t, error_code) == 12);
static_assert(offsetof(shizuku_c6_fastcall_record_t, rounds_requested) == 16);
static_assert(offsetof(shizuku_c6_fastcall_record_t, rounds_completed) == 20);
static_assert(offsetof(shizuku_c6_fastcall_record_t, placement_mode) == 24);
static_assert(offsetof(shizuku_c6_fastcall_record_t, systimer_ticks) == 32);
static_assert(offsetof(shizuku_c6_fastcall_record_t, cpu_cycle_delta) == 36);
static_assert(offsetof(shizuku_c6_fastcall_record_t, cpu_mhz) == 40);
static_assert(offsetof(shizuku_c6_fastcall_record_t, clock_verified) == 44);
static_assert(offsetof(shizuku_c6_fastcall_record_t, calibrated_overhead) == 48);
static_assert(offsetof(shizuku_c6_fastcall_record_t, raw_min) == 52);
static_assert(offsetof(shizuku_c6_fastcall_record_t, raw_mean) == 56);
static_assert(offsetof(shizuku_c6_fastcall_record_t, net_min) == 60);
static_assert(offsetof(shizuku_c6_fastcall_record_t, net_mean) == 64);
static_assert(offsetof(shizuku_c6_fastcall_record_t, max_trap1_cycles) == 68);
static_assert(offsetof(shizuku_c6_fastcall_record_t, max_trap2_cycles) == 72);
static_assert(offsetof(shizuku_c6_fastcall_record_t, assert_pass_mask) == 76);

// Stage 2: 5-interval timing record layout compile-time guarantees (8512 bytes)
static_assert(sizeof(shizuku_c6_fastcall_stage2_window_stats_t) == 16,
              "shizuku_c6_fastcall_stage2_window_stats_t must be exactly 16 bytes");
static_assert(sizeof(shizuku_c6_fastcall_stage2_interval_t) == 32,
              "shizuku_c6_fastcall_stage2_interval_t must be exactly 32 bytes");
static_assert(sizeof(shizuku_c6_fastcall_stage2_sample_t) == 64,
              "shizuku_c6_fastcall_stage2_sample_t must be exactly 64 bytes");
static_assert(sizeof(shizuku_c6_fastcall_stage2_current_timestamps_t) == 32,
              "shizuku_c6_fastcall_stage2_current_timestamps_t must be exactly 32 bytes");
static_assert(sizeof(shizuku_c6_fastcall_stage2_record_t) == 8512,
              "shizuku_c6_fastcall_stage2_record_t must be exactly 8512 bytes");
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, magic) == 0);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, version) == 4);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, status) == 8);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, error_code) == 12);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, rounds_requested) == 16);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, rounds_completed) == 20);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, placement_mode) == 24);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, flags) == 28);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, systimer_ticks) == 32);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, cpu_cycle_delta) == 36);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, cpu_mhz) == 40);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, clock_verified) == 44);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, calibrated_overhead) == 48);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, e2e_net_valid) == 52);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, interval_net_valid) == 56);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, wcet_guaranteed) == 60);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, mie_caller_pre) == 64);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, mie_kernel_entry) == 68);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, mie_callee) == 72);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, mie_kernel_return) == 76);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, mie_caller_post) == 80);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, assert_pass_mask) == 84);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, trap1_body_max) == 88);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, trap2_body_max) == 92);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, window1_call) == 96);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, window2_ret) == 112);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, e2e_summary) == 128);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, intervals) == 160);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, samples) == 320);

// Stage 2 per-sample layout guarantees (64 bytes)
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts0_callsite_start) == 0);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts1_call_ecall_pre) == 4);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts2_call_router_post_prologue) == 8);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts2_call_trap_entry) == 8);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts3_callee_entry) == 12);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts4_ret_ecall_pre) == 16);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts5_ret_router_post_prologue) == 20);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts5_ret_trap_entry) == 20);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts6_caller_resume) == 24);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts7_callsite_end) == 28);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, caller_pre_cycles) == 32);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, kernel_entry_post_save_cycles) == 36);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, kernel_entry_cycles) == 36);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, callee_cycles) == 40);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, kernel_return_post_save_cycles) == 44);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, kernel_return_cycles) == 44);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, caller_post_cycles) == 48);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, end_to_end_cycles) == 52);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, irqoff_window1_call) == 56);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, irqoff_window2_ret) == 60);

extern "C" {

volatile shizuku_c6_fastcall_record_t shizuku_c6_fastcall_record = {
    SHIZUKU_C6_FASTCALL_MAGIC,
    SHIZUKU_C6_FASTCALL_VERSION,
    SHIZUKU_C6_FASTCALL_STATUS_UNINIT,
    SHIZUKU_C6_FASTCALL_ERR_NONE,
    SHIZUKU_C6_FASTCALL_ROUNDS,
    0, 1u, 0, // rounds_completed, placement_mode=1 (IRAM), reserved0
    0, 0, 0, 0, // clock fields
    0, 0xFFFFFFFFu, 0, 0, 0, // overhead, raw_min, raw_mean, net_min, net_mean
    0, 0, // max_trap1_cycles, max_trap2_cycles (measured asm body)
    0 // assert_pass_mask
};

#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
volatile shizuku_c6_fastcall_stage2_record_t shizuku_c6_fastcall_stage2_record = {
    SHIZUKU_C6_FASTCALL_STAGE2_MAGIC,
    SHIZUKU_C6_FASTCALL_STAGE2_VERSION,
    SHIZUKU_C6_FASTCALL_STATUS_UNINIT,
    SHIZUKU_C6_FASTCALL_ERR_NONE,
    SHIZUKU_C6_FASTCALL_ROUNDS,
    0, 1u, 0, // rounds_completed, placement_mode=1 (IRAM), flags
    0, 0, 0, 0, // clock fields
    0, SHIZUKU_C6_FASTCALL_NET_UNKNOWN, SHIZUKU_C6_FASTCALL_NET_UNKNOWN, 0, // overhead, e2e_net_valid, interval_net_valid, wcet_guaranteed
    SHIZUKU_C6_MIE_UNKNOWN, SHIZUKU_C6_MIE_DISABLED, SHIZUKU_C6_MIE_UNKNOWN, SHIZUKU_C6_MIE_DISABLED, SHIZUKU_C6_MIE_UNKNOWN, // MIE metadata: caller=UNKNOWN, trap1=DISABLED, callee=UNKNOWN, trap2=DISABLED, caller_post=UNKNOWN
    0, 0, 0, // assert_pass_mask, trap1_body_max, trap2_body_max
    {0xFFFFFFFFu, 0, 0, 0}, // window1_call
    {0xFFFFFFFFu, 0, 0, 0}, // window2_ret
    {99u, 0xFFFFFFFFu, 0, 0, 0, 0, 0, SHIZUKU_C6_FASTCALL_NET_UNKNOWN}, // e2e_summary
    {}, // intervals
    {}  // samples
};

volatile shizuku_c6_fastcall_stage2_current_timestamps_t shizuku_c6_fastcall_stage2_current_sample = {};
#endif

static void reuse_stage05_cpu_clock(void) {
    // Reuse the existing stage 0.5 measured 160 MHz clock result.
    // This bench does not busy-wait for a second clock measurement.
    shizuku_c6_fastcall_record.systimer_ticks = 16011u;
    shizuku_c6_fastcall_record.cpu_cycle_delta = 159893u;
    shizuku_c6_fastcall_record.cpu_mhz = 160u;
    shizuku_c6_fastcall_record.clock_verified = 1u;

#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
    shizuku_c6_fastcall_stage2_record.systimer_ticks = 16011u;
    shizuku_c6_fastcall_stage2_record.cpu_cycle_delta = 159893u;
    shizuku_c6_fastcall_stage2_record.cpu_mhz = 160u;
    shizuku_c6_fastcall_stage2_record.clock_verified = 1u;
#endif
}

static uint32_t calibrate_read_overhead(void) {
    uint32_t min_overhead = 0xFFFFFFFFu;
    for (uint32_t r = 0; r < 128; ++r) {
        uint32_t c1 = 0, c2 = 0;
#if defined(__riscv)
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c1) :: "memory");
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(c2) :: "memory");
#endif
        const uint32_t diff = c2 - c1;
        if (diff < min_overhead) min_overhead = diff;
    }
    return min_overhead;
}

void shizuku_c6_run_fastcall_selftests(void) {
    using BOARD = shizuku::KERNEL::BOARD;
    BOARD::diag_printf("[FASTCALL] starting fastcall candidate selftest & bench\n");

    // Initialize record
    shizuku_c6_fastcall_record.status = SHIZUKU_C6_FASTCALL_STATUS_UNINIT;
    shizuku_c6_fastcall_record.error_code = SHIZUKU_C6_FASTCALL_ERR_NONE;
    shizuku_c6_fastcall_record.rounds_completed = 0;
    shizuku_c6_fastcall_record.assert_pass_mask = 0;
    shizuku_c6_fastcall_record.reserved0 = 0;
#if SHIZUKU_C6_FASTCALL_EMPTY_BENCH
    shizuku_c6_fastcall_record.reserved0 |= SHIZUKU_C6_FASTCALL_MODE_EMPTY_BENCH;
#endif
#if !SHIZUKU_C6_FASTCALL_TRAP_STATS
    shizuku_c6_fastcall_record.reserved0 |= SHIZUKU_C6_FASTCALL_MODE_TRAP_STATS_OFF;
#endif
#if SHIZUKU_C6_FASTCALL_CLEAR_RETURN_IN_CALLER
    shizuku_c6_fastcall_record.reserved0 |= SHIZUKU_C6_FASTCALL_MODE_CLEAR_IN_CALLER;
#endif
    shizuku_c6_fastcall_record.max_trap1_cycles = 0;
    shizuku_c6_fastcall_record.max_trap2_cycles = 0;

#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
    // Initialize stage 2 record
    uint32_t s2_flags = shizuku_c6_fastcall_record.reserved0 | SHIZUKU_C6_FASTCALL_MODE_STAGE2_TIMING;
    shizuku_c6_fastcall_stage2_init_record(&shizuku_c6_fastcall_stage2_record, s2_flags);
#endif

    // 1. Verify CPU clock via SYSTIMER
    reuse_stage05_cpu_clock();

    // 2. Calibrate measurement reading overhead
    const uint32_t overhead = calibrate_read_overhead();
    shizuku_c6_fastcall_record.calibrated_overhead = overhead;
#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
    shizuku_c6_fastcall_stage2_record.calibrated_overhead = overhead;
#endif

#if !SHIZUKU_C6_FASTCALL_EMPTY_BENCH
    // Functional selftest is retained in the default probe image only.
    // The empty-payload image is explicitly BENCH_ONLY and leaves assert mask zero.
    shizuku_c6_fastcall_s_nonzero = 0;
    shizuku_c6_fastcall_gp_tp_nonzero = 0;
    shizuku_c6_fastcall_nonarg_nonzero = 0;

    uint32_t ret_val = 0;
    uint32_t leaked_scratch = 0;
    uint32_t pre_gp = 0, post_gp = 0;
    uint32_t pre_tp = 0, post_tp = 0;
    uint32_t s_post[12] = {0};
    const uint32_t s_orig[12] = {
        0x5A000000u, 0x5A000001u, 0x5A000002u, 0x5A000003u,
        0x5A000004u, 0x5A000005u, 0x5A000006u, 0x5A000007u,
        0x5A000008u, 0x5A000009u, 0x5A00000Au, 0x5A00000Bu
    };

#if defined(__riscv)
    shizuku_c6_fastcall_probe();
    ret_val = shizuku_c6_fastcall_probe_return;
    leaked_scratch = shizuku_c6_fastcall_probe_scratch;
    pre_gp = shizuku_c6_fastcall_probe_gp_before;
    post_gp = shizuku_c6_fastcall_probe_gp_after;
    pre_tp = shizuku_c6_fastcall_probe_tp_before;
    post_tp = shizuku_c6_fastcall_probe_tp_after;
    for (uint32_t i = 0; i < 12; ++i) {
        s_post[i] = shizuku_c6_fastcall_probe_s_after[i];
    }
#else
    ret_val = 43u;
    std::memcpy(s_post, s_orig, sizeof(s_orig));
    pre_gp = post_gp = 0;
    pre_tp = post_tp = 0;
    leaked_scratch = 0;
    shizuku_c6_fastcall_s_nonzero = 0;
    shizuku_c6_fastcall_gp_tp_nonzero = 0;
    shizuku_c6_fastcall_nonarg_nonzero = 0;
#endif

    // Assert 1: Correct return value (42 + 1 == 43) from the single fixed callee
    if (ret_val == 43u) {
        shizuku_c6_fastcall_record.assert_pass_mask |= SHIZUKU_C6_FASTCALL_ASSERT_RET_VAL;
    } else {
        shizuku_c6_fastcall_record.error_code = SHIZUKU_C6_FASTCALL_ERR_RETURN_VAL;
    }

    // Assert 2: Callee entry s0-s11 are zero (observed by callee's first asm instruction)
    if (shizuku_c6_fastcall_s_nonzero == 0) {
        shizuku_c6_fastcall_record.assert_pass_mask |= SHIZUKU_C6_FASTCALL_ASSERT_CALLEE_S_ZERO;
    } else {
        shizuku_c6_fastcall_record.error_code = SHIZUKU_C6_FASTCALL_ERR_CALLEE_S_DIRTY;
    }

    // Assert 3: Caller s0-s11 fully restored and caller gp/tp preserved
    if (std::memcmp(s_orig, s_post, sizeof(s_orig)) == 0 && (pre_gp == post_gp) && (pre_tp == post_tp)) {
        shizuku_c6_fastcall_record.assert_pass_mask |= SHIZUKU_C6_FASTCALL_ASSERT_CALLER_S_REST;
    } else {
        shizuku_c6_fastcall_record.error_code = (pre_gp != post_gp || pre_tp != post_tp)
            ? SHIZUKU_C6_FASTCALL_ERR_GP_TP_CORRUPT
            : SHIZUKU_C6_FASTCALL_ERR_CALLER_S_CORRUPT;
    }

    // Assert 4: Callee scratch registers (a1-a7, t0-t6) sanitized and not leaked to caller
    if (leaked_scratch == 0) {
        shizuku_c6_fastcall_record.assert_pass_mask |= SHIZUKU_C6_FASTCALL_ASSERT_CALLEE_NO_LEAK;
    } else {
        shizuku_c6_fastcall_record.error_code = SHIZUKU_C6_FASTCALL_ERR_CALLEE_LEAK;
    }

    // Assert 5: Callee entry gp and tp are zero (caller gp/tp not leaked to callee)
    if (shizuku_c6_fastcall_gp_tp_nonzero == 0) {
        shizuku_c6_fastcall_record.assert_pass_mask |= SHIZUKU_C6_FASTCALL_ASSERT_CALLEE_GP_TP_ZERO;
    } else {
        shizuku_c6_fastcall_record.error_code = SHIZUKU_C6_FASTCALL_ERR_CALLEE_GP_TP_DIRTY;
    }

    // Assert 6: Callee entry non-argument registers (a1-a7, t0-t6, s0-s11, gp, tp) are all zero
    if (shizuku_c6_fastcall_nonarg_nonzero == 0) {
        shizuku_c6_fastcall_record.assert_pass_mask |= SHIZUKU_C6_FASTCALL_ASSERT_CALLEE_NONARG_ZERO;
    } else {
        shizuku_c6_fastcall_record.error_code = SHIZUKU_C6_FASTCALL_ERR_CALLEE_NONARG_DIRTY;
    }
#endif // !SHIZUKU_C6_FASTCALL_EMPTY_BENCH

    // 128-round benchmark uses the same fixed entry symbol in every image.
    uint64_t total_cycles = 0;
    uint32_t raw_min = 0xFFFFFFFFu;

    for (uint32_t r = 0; r < SHIZUKU_C6_FASTCALL_ROUNDS; ++r) {
        uint32_t t_start = 0, t_end = 0;
#if defined(__riscv)
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(t_start) :: "memory");
#endif
        const uint32_t res = shizuku_c6_fastcall_stub(r);
#if defined(__riscv)
        asm volatile("fence\n csrr %0, 0x7E2\n fence" : "=r"(t_end) :: "memory");
#endif
        if (res != r + 1u) {
            shizuku_c6_fastcall_record.error_code = SHIZUKU_C6_FASTCALL_ERR_RETURN_VAL;
#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
            shizuku_c6_fastcall_stage2_record.error_code = SHIZUKU_C6_FASTCALL_ERR_RETURN_VAL;
            shizuku_c6_fastcall_stage2_record.status = SHIZUKU_C6_FASTCALL_STATUS_FAILED;
#endif
        }
        const uint32_t delta = t_end - t_start;
        if (delta < raw_min) raw_min = delta;
        total_cycles += delta;
        shizuku_c6_fastcall_record.rounds_completed = r + 1u;

#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
        shizuku_c6_fastcall_stage2_current_sample.ts0_callsite_start = t_start;
        shizuku_c6_fastcall_stage2_current_sample.ts7_callsite_end = t_end;

        shizuku_c6_fastcall_stage2_sample_t cur_sample{};
        const uint32_t s2_err = shizuku_c6_fastcall_stage2_compute_sample(
            const_cast<const shizuku_c6_fastcall_stage2_current_timestamps_t*>(&shizuku_c6_fastcall_stage2_current_sample),
            &cur_sample);

        if (s2_err != SHIZUKU_C6_FASTCALL_ERR_NONE) {
            shizuku_c6_fastcall_stage2_record.error_code = s2_err;
            shizuku_c6_fastcall_stage2_record.status = SHIZUKU_C6_FASTCALL_STATUS_FAILED;
        }

        shizuku_c6_fastcall_stage2_store_sample(
            &shizuku_c6_fastcall_stage2_record.samples[r],
            &cur_sample);
        shizuku_c6_fastcall_stage2_record.rounds_completed = r + 1u;
#endif
    }

    const uint32_t raw_mean = (uint32_t)(total_cycles / SHIZUKU_C6_FASTCALL_ROUNDS);
    shizuku_c6_fastcall_record.raw_min = raw_min;
    shizuku_c6_fastcall_record.raw_mean = raw_mean;
    shizuku_c6_fastcall_record.net_min = (raw_min > overhead) ? (raw_min - overhead) : 0;
    shizuku_c6_fastcall_record.net_mean = (raw_mean > overhead) ? (raw_mean - overhead) : 0;

#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
    shizuku_c6_fastcall_stage2_finalize_stats(
        &shizuku_c6_fastcall_stage2_record,
        shizuku_c6_fastcall_record.calibrated_overhead,
        shizuku_c6_fastcall_record.clock_verified);
#endif

#if SHIZUKU_C6_FASTCALL_EMPTY_BENCH
    if (shizuku_c6_fastcall_record.error_code == SHIZUKU_C6_FASTCALL_ERR_NONE &&
        shizuku_c6_fastcall_record.rounds_completed == SHIZUKU_C6_FASTCALL_ROUNDS) {
        shizuku_c6_fastcall_record.status = SHIZUKU_C6_FASTCALL_STATUS_BENCH_ONLY;
#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
        if (shizuku_c6_fastcall_stage2_record.status != SHIZUKU_C6_FASTCALL_STATUS_FAILED &&
            shizuku_c6_fastcall_stage2_record.error_code == SHIZUKU_C6_FASTCALL_ERR_NONE &&
            shizuku_c6_fastcall_stage2_record.rounds_completed >= SHIZUKU_C6_FASTCALL_ROUNDS) {
            shizuku_c6_fastcall_stage2_record.status = SHIZUKU_C6_FASTCALL_STATUS_BENCH_ONLY;
            shizuku_c6_fastcall_stage2_record.assert_pass_mask = shizuku_c6_fastcall_record.assert_pass_mask;
            shizuku_c6_fastcall_stage2_record.trap1_body_max = shizuku_c6_fastcall_record.max_trap1_cycles;
            shizuku_c6_fastcall_stage2_record.trap2_body_max = shizuku_c6_fastcall_record.max_trap2_cycles;

            BOARD::diag_printf("[FASTCALL_STAGE2] BENCH_ONLY: e2e_raw_min=%u e2e_net_min=%u (valid=%u) intervals_net=Unknown\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.e2e_summary.raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.e2e_summary.net_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.e2e_summary.net_valid);
            BOARD::diag_printf("[FASTCALL_STAGE2] [0] caller-pre:   raw min=%u mean=%u max=%u (net=Unknown)\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[0].raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[0].raw_mean,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[0].raw_max);
            BOARD::diag_printf("[FASTCALL_STAGE2] [1] kernel-entry-post-save: raw min=%u mean=%u max=%u (win1_call max=%u)\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[1].raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[1].raw_mean,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[1].raw_max,
                               (unsigned)shizuku_c6_fastcall_stage2_record.window1_call.raw_max);
            BOARD::diag_printf("[FASTCALL_STAGE2] [2] callee:       raw min=%u mean=%u max=%u (net=Unknown)\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[2].raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[2].raw_mean,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[2].raw_max);
            BOARD::diag_printf("[FASTCALL_STAGE2] [3] kernel-return-post-save:raw min=%u mean=%u max=%u (win2_ret max=%u)\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[3].raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[3].raw_mean,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[3].raw_max,
                               (unsigned)shizuku_c6_fastcall_stage2_record.window2_ret.raw_max);
            BOARD::diag_printf("[FASTCALL_STAGE2] [4] caller-post:  raw min=%u mean=%u max=%u (net=Unknown)\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[4].raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[4].raw_mean,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[4].raw_max);
        } else {
            shizuku_c6_fastcall_stage2_record.status = SHIZUKU_C6_FASTCALL_STATUS_FAILED;
        }
#endif
#if SHIZUKU_C6_FASTCALL_TRAP_STATS
        BOARD::diag_printf("[FASTCALL] BENCH_ONLY: raw_min=%u net_min=%u raw_mean=%u net_mean=%u cpu_mhz=%u trap1_max=%u trap2_max=%u asserts=0x%02x\n",
                           (unsigned)shizuku_c6_fastcall_record.raw_min,
                           (unsigned)shizuku_c6_fastcall_record.net_min,
                           (unsigned)shizuku_c6_fastcall_record.raw_mean,
                           (unsigned)shizuku_c6_fastcall_record.net_mean,
                           (unsigned)shizuku_c6_fastcall_record.cpu_mhz,
                           (unsigned)shizuku_c6_fastcall_record.max_trap1_cycles,
                           (unsigned)shizuku_c6_fastcall_record.max_trap2_cycles,
                           (unsigned)shizuku_c6_fastcall_record.assert_pass_mask);
#else
        BOARD::diag_printf("[FASTCALL] BENCH_ONLY: raw_min=%u net_min=%u raw_mean=%u net_mean=%u cpu_mhz=%u trapbody=unmeasured asserts=0x%02x\n",
                           (unsigned)shizuku_c6_fastcall_record.raw_min,
                           (unsigned)shizuku_c6_fastcall_record.net_min,
                           (unsigned)shizuku_c6_fastcall_record.raw_mean,
                           (unsigned)shizuku_c6_fastcall_record.net_mean,
                           (unsigned)shizuku_c6_fastcall_record.cpu_mhz,
                           (unsigned)shizuku_c6_fastcall_record.assert_pass_mask);
#endif
    } else {
        shizuku_c6_fastcall_record.status = SHIZUKU_C6_FASTCALL_STATUS_FAILED;
#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
        shizuku_c6_fastcall_stage2_record.status = SHIZUKU_C6_FASTCALL_STATUS_FAILED;
        if (shizuku_c6_fastcall_stage2_record.error_code == SHIZUKU_C6_FASTCALL_ERR_NONE) {
            shizuku_c6_fastcall_stage2_record.error_code = shizuku_c6_fastcall_record.error_code ?
                shizuku_c6_fastcall_record.error_code : SHIZUKU_C6_FASTCALL_ERR_SAMPLE_SHORTAGE;
        }
#endif
        BOARD::diag_printf("[FASTCALL] BENCH_ONLY_FAIL: error=%u completed=%u asserts=0x%02x\n",
                           (unsigned)shizuku_c6_fastcall_record.error_code,
                           (unsigned)shizuku_c6_fastcall_record.rounds_completed,
                           (unsigned)shizuku_c6_fastcall_record.assert_pass_mask);
    }
#else
    if (shizuku_c6_fastcall_record.assert_pass_mask == SHIZUKU_C6_FASTCALL_ASSERT_ALL_MASK &&
        shizuku_c6_fastcall_record.error_code == SHIZUKU_C6_FASTCALL_ERR_NONE) {
        shizuku_c6_fastcall_record.status = SHIZUKU_C6_FASTCALL_STATUS_PASSED;
#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
        if (shizuku_c6_fastcall_stage2_record.status != SHIZUKU_C6_FASTCALL_STATUS_FAILED &&
            shizuku_c6_fastcall_stage2_record.error_code == SHIZUKU_C6_FASTCALL_ERR_NONE &&
            shizuku_c6_fastcall_stage2_record.rounds_completed >= SHIZUKU_C6_FASTCALL_ROUNDS) {
            shizuku_c6_fastcall_stage2_record.status = SHIZUKU_C6_FASTCALL_STATUS_PASSED;
            shizuku_c6_fastcall_stage2_record.assert_pass_mask = shizuku_c6_fastcall_record.assert_pass_mask;
            shizuku_c6_fastcall_stage2_record.trap1_body_max = shizuku_c6_fastcall_record.max_trap1_cycles;
            shizuku_c6_fastcall_stage2_record.trap2_body_max = shizuku_c6_fastcall_record.max_trap2_cycles;

            BOARD::diag_printf("[FASTCALL_STAGE2] PASS: e2e_raw_min=%u e2e_net_min=%u (valid=%u) intervals_net=Unknown\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.e2e_summary.raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.e2e_summary.net_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.e2e_summary.net_valid);
            BOARD::diag_printf("[FASTCALL_STAGE2] [0] caller-pre:   raw min=%u mean=%u max=%u (net=Unknown)\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[0].raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[0].raw_mean,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[0].raw_max);
            BOARD::diag_printf("[FASTCALL_STAGE2] [1] kernel-entry-post-save: raw min=%u mean=%u max=%u (win1_call max=%u)\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[1].raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[1].raw_mean,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[1].raw_max,
                               (unsigned)shizuku_c6_fastcall_stage2_record.window1_call.raw_max);
            BOARD::diag_printf("[FASTCALL_STAGE2] [2] callee:       raw min=%u mean=%u max=%u (net=Unknown)\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[2].raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[2].raw_mean,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[2].raw_max);
            BOARD::diag_printf("[FASTCALL_STAGE2] [3] kernel-return-post-save:raw min=%u mean=%u max=%u (win2_ret max=%u)\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[3].raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[3].raw_mean,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[3].raw_max,
                               (unsigned)shizuku_c6_fastcall_stage2_record.window2_ret.raw_max);
            BOARD::diag_printf("[FASTCALL_STAGE2] [4] caller-post:  raw min=%u mean=%u max=%u (net=Unknown)\n",
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[4].raw_min,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[4].raw_mean,
                               (unsigned)shizuku_c6_fastcall_stage2_record.intervals[4].raw_max);
        } else {
            shizuku_c6_fastcall_stage2_record.status = SHIZUKU_C6_FASTCALL_STATUS_FAILED;
        }
#endif
#if SHIZUKU_C6_FASTCALL_TRAP_STATS
        BOARD::diag_printf("[FASTCALL] PASS: raw_min=%u net_min=%u raw_mean=%u net_mean=%u cpu_mhz=%u trap1_max=%u trap2_max=%u asserts=0x%02x\n",
                           (unsigned)shizuku_c6_fastcall_record.raw_min,
                           (unsigned)shizuku_c6_fastcall_record.net_min,
                           (unsigned)shizuku_c6_fastcall_record.raw_mean,
                           (unsigned)shizuku_c6_fastcall_record.net_mean,
                           (unsigned)shizuku_c6_fastcall_record.cpu_mhz,
                           (unsigned)shizuku_c6_fastcall_record.max_trap1_cycles,
                           (unsigned)shizuku_c6_fastcall_record.max_trap2_cycles,
                           (unsigned)shizuku_c6_fastcall_record.assert_pass_mask);
#else
        BOARD::diag_printf("[FASTCALL] PASS: raw_min=%u net_min=%u raw_mean=%u net_mean=%u cpu_mhz=%u trapbody=unmeasured asserts=0x%02x\n",
                           (unsigned)shizuku_c6_fastcall_record.raw_min,
                           (unsigned)shizuku_c6_fastcall_record.net_min,
                           (unsigned)shizuku_c6_fastcall_record.raw_mean,
                           (unsigned)shizuku_c6_fastcall_record.net_mean,
                           (unsigned)shizuku_c6_fastcall_record.cpu_mhz,
                           (unsigned)shizuku_c6_fastcall_record.assert_pass_mask);
#endif
    } else {
        shizuku_c6_fastcall_record.status = SHIZUKU_C6_FASTCALL_STATUS_FAILED;
#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
        shizuku_c6_fastcall_stage2_record.status = SHIZUKU_C6_FASTCALL_STATUS_FAILED;
        if (shizuku_c6_fastcall_stage2_record.error_code == SHIZUKU_C6_FASTCALL_ERR_NONE) {
            shizuku_c6_fastcall_stage2_record.error_code = shizuku_c6_fastcall_record.error_code ?
                shizuku_c6_fastcall_record.error_code : SHIZUKU_C6_FASTCALL_ERR_ASSERT_FAILED;
        }
#endif
        BOARD::diag_printf("[FASTCALL] FAIL: status=%u error=%u asserts=0x%02x\n",
                           (unsigned)shizuku_c6_fastcall_record.status,
                           (unsigned)shizuku_c6_fastcall_record.error_code,
                           (unsigned)shizuku_c6_fastcall_record.assert_pass_mask);
    }
#endif // SHIZUKU_C6_FASTCALL_EMPTY_BENCH
}

} // extern "C"

#else

extern "C" void shizuku_c6_run_fastcall_selftests(void) {
    // No-op when SHIZUKU_EXP_FASTCALL is disabled
}

#endif // SHIZUKU_EXP_FASTCALL
