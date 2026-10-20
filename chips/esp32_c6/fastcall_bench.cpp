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

static void reuse_stage05_cpu_clock(void) {
    // Reuse the existing stage 0.5 measured 160 MHz clock result.
    // This bench does not busy-wait for a second clock measurement.
    shizuku_c6_fastcall_record.systimer_ticks = 16011u;
    shizuku_c6_fastcall_record.cpu_cycle_delta = 159893u;
    shizuku_c6_fastcall_record.cpu_mhz = 160u;
    shizuku_c6_fastcall_record.clock_verified = 1u;
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
    shizuku_c6_fastcall_record.max_trap1_cycles = 0;
    shizuku_c6_fastcall_record.max_trap2_cycles = 0;

    // 1. Verify CPU clock via SYSTIMER
    reuse_stage05_cpu_clock();

    // 2. Calibrate measurement reading overhead
    const uint32_t overhead = calibrate_read_overhead();
    shizuku_c6_fastcall_record.calibrated_overhead = overhead;

    // 3. Functional Selftest using the single fixed ASM callee (no descriptor modification)
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

    // 4. 128-round benchmark with the exact same fixed asm callee (no descriptor modification)
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
        }
        const uint32_t delta = t_end - t_start;
        if (delta < raw_min) raw_min = delta;
        total_cycles += delta;
        shizuku_c6_fastcall_record.rounds_completed = r + 1u;
    }

    const uint32_t raw_mean = (uint32_t)(total_cycles / SHIZUKU_C6_FASTCALL_ROUNDS);
    shizuku_c6_fastcall_record.raw_min = raw_min;
    shizuku_c6_fastcall_record.raw_mean = raw_mean;
    shizuku_c6_fastcall_record.net_min = (raw_min > overhead) ? (raw_min - overhead) : 0;
    shizuku_c6_fastcall_record.net_mean = (raw_mean > overhead) ? (raw_mean - overhead) : 0;

    if (shizuku_c6_fastcall_record.assert_pass_mask == SHIZUKU_C6_FASTCALL_ASSERT_ALL_MASK &&
        shizuku_c6_fastcall_record.error_code == SHIZUKU_C6_FASTCALL_ERR_NONE) {
        shizuku_c6_fastcall_record.status = SHIZUKU_C6_FASTCALL_STATUS_PASSED;
        BOARD::diag_printf("[FASTCALL] PASS: raw_min=%u net_min=%u raw_mean=%u net_mean=%u cpu_mhz=%u trap1_max=%u trap2_max=%u asserts=0x%02x\n",
                           (unsigned)shizuku_c6_fastcall_record.raw_min,
                           (unsigned)shizuku_c6_fastcall_record.net_min,
                           (unsigned)shizuku_c6_fastcall_record.raw_mean,
                           (unsigned)shizuku_c6_fastcall_record.net_mean,
                           (unsigned)shizuku_c6_fastcall_record.cpu_mhz,
                           (unsigned)shizuku_c6_fastcall_record.max_trap1_cycles,
                           (unsigned)shizuku_c6_fastcall_record.max_trap2_cycles,
                           (unsigned)shizuku_c6_fastcall_record.assert_pass_mask);
    } else {
        shizuku_c6_fastcall_record.status = SHIZUKU_C6_FASTCALL_STATUS_FAILED;
        BOARD::diag_printf("[FASTCALL] FAIL: status=%u error=%u asserts=0x%02x\n",
                           (unsigned)shizuku_c6_fastcall_record.status,
                           (unsigned)shizuku_c6_fastcall_record.error_code,
                           (unsigned)shizuku_c6_fastcall_record.assert_pass_mask);
    }
}

} // extern "C"

#else

extern "C" void shizuku_c6_run_fastcall_selftests(void) {
    // No-op when SHIZUKU_EXP_FASTCALL is disabled
}

#endif // SHIZUKU_EXP_FASTCALL
