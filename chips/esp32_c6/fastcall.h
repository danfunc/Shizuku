#ifndef SHIZUKU_C6_FASTCALL_H
#define SHIZUKU_C6_FASTCALL_H

// Asm-safe constants (no 'u' suffix for assembler compatibility)
#define SHIZUKU_FASTCALL_TRAP_CALL   0x53464301 // "SFC\x01" - Fastcall CALL ecall trap
#define SHIZUKU_FASTCALL_TRAP_RETURN 0x53464302 // "SFC\x02" - Fastcall RETURN ecall trap

#define SHIZUKU_C6_FASTCALL_MAGIC   0x46433152  // "FC1R" (Fastcall 1 Record)
#define SHIZUKU_C6_FASTCALL_VERSION 1

#define SHIZUKU_C6_FASTCALL_STATUS_UNINIT 0
#define SHIZUKU_C6_FASTCALL_STATUS_PASSED 1
#define SHIZUKU_C6_FASTCALL_STATUS_FAILED 2
#define SHIZUKU_C6_FASTCALL_STATUS_BENCH_ONLY 3

// Compile-time comparison modes. Defaults preserve the probe, trap statistics,
// and kernel-side return sanitization behavior.
#ifndef SHIZUKU_C6_FASTCALL_EMPTY_BENCH
#define SHIZUKU_C6_FASTCALL_EMPTY_BENCH 0
#endif
#ifndef SHIZUKU_C6_FASTCALL_TRAP_STATS
#define SHIZUKU_C6_FASTCALL_TRAP_STATS 1
#endif
#ifndef SHIZUKU_C6_FASTCALL_CLEAR_RETURN_IN_CALLER
#define SHIZUKU_C6_FASTCALL_CLEAR_RETURN_IN_CALLER 0
#endif
#ifndef SHIZUKU_C6_FASTCALL_STAGE2_TIMING
#define SHIZUKU_C6_FASTCALL_STAGE2_TIMING 0
#endif

#define SHIZUKU_C6_FASTCALL_MODE_EMPTY_BENCH       (1u << 0)
#define SHIZUKU_C6_FASTCALL_MODE_TRAP_STATS_OFF    (1u << 1)
#define SHIZUKU_C6_FASTCALL_MODE_CLEAR_IN_CALLER   (1u << 2)
#define SHIZUKU_C6_FASTCALL_MODE_STAGE2_TIMING     (1u << 3)

#define SHIZUKU_C6_FASTCALL_ERR_NONE             0
#define SHIZUKU_C6_FASTCALL_ERR_RETURN_VAL       1
#define SHIZUKU_C6_FASTCALL_ERR_CALLEE_S_DIRTY   2
#define SHIZUKU_C6_FASTCALL_ERR_CALLER_S_CORRUPT 3
#define SHIZUKU_C6_FASTCALL_ERR_CALLEE_LEAK      4
#define SHIZUKU_C6_FASTCALL_ERR_SAMPLE_SHORTAGE  5
#define SHIZUKU_C6_FASTCALL_ERR_GP_TP_CORRUPT    6
#define SHIZUKU_C6_FASTCALL_ERR_CALLEE_GP_TP_DIRTY 7
#define SHIZUKU_C6_FASTCALL_ERR_CALLEE_NONARG_DIRTY 8
#define SHIZUKU_C6_FASTCALL_ERR_ASSERT_FAILED    9

#define SHIZUKU_C6_FASTCALL_ROUNDS 128

#define SHIZUKU_C6_FASTCALL_ASSERT_RET_VAL           (1u << 0)
#define SHIZUKU_C6_FASTCALL_ASSERT_CALLEE_S_ZERO     (1u << 1)
#define SHIZUKU_C6_FASTCALL_ASSERT_CALLER_S_REST     (1u << 2)
#define SHIZUKU_C6_FASTCALL_ASSERT_CALLEE_NO_LEAK    (1u << 3)
#define SHIZUKU_C6_FASTCALL_ASSERT_CALLEE_GP_TP_ZERO (1u << 4)
#define SHIZUKU_C6_FASTCALL_ASSERT_CALLEE_NONARG_ZERO (1u << 5)
#define SHIZUKU_C6_FASTCALL_ASSERT_ALL_MASK          (0x3Fu)

// Descriptor layout offsets (bytes, shared by asm and C/C++)
#define SHIZUKU_C6_FASTCALL_DESC_ENTRY_PC    0
#define SHIZUKU_C6_FASTCALL_DESC_CALLER_SP   4
#define SHIZUKU_C6_FASTCALL_DESC_CALLER_PC   8
#define SHIZUKU_C6_FASTCALL_DESC_DEPTH      12
#define SHIZUKU_C6_FASTCALL_DESC_CALL_COUNT 16
#define SHIZUKU_C6_FASTCALL_DESC_SIZE       20

// Record layout offsets (bytes, shared by asm and C/C++)
#define SHIZUKU_C6_FASTCALL_REC_MAGIC       0
#define SHIZUKU_C6_FASTCALL_REC_VERSION     4
#define SHIZUKU_C6_FASTCALL_REC_STATUS      8
#define SHIZUKU_C6_FASTCALL_REC_ERROR_CODE 12
#define SHIZUKU_C6_FASTCALL_REC_MAX_TRAP1  68
#define SHIZUKU_C6_FASTCALL_REC_MAX_TRAP2  72
#define SHIZUKU_C6_FASTCALL_REC_SIZE       80

// Stage 2: 5-interval timing constants & offsets
#define SHIZUKU_C6_FASTCALL_STAGE2_MAGIC    0x46433252  // "FC2R" (Fastcall 2 Record)
#define SHIZUKU_C6_FASTCALL_STAGE2_VERSION  1

#define SHIZUKU_C6_FASTCALL_NET_UNKNOWN     0
#define SHIZUKU_C6_FASTCALL_NET_VALID       1

#define SHIZUKU_C6_MIE_DISABLED             0  // Explicitly disabled by hardware/software (MIE=0)
#define SHIZUKU_C6_MIE_ENABLED              1  // Explicitly enabled (MIE=1)
#define SHIZUKU_C6_MIE_UNKNOWN              2  // Unknown / unverified caller context

#define SHIZUKU_C6_FASTCALL_STAGE2_INTERVAL_COUNT 5
#define SHIZUKU_C6_FASTCALL_STAGE2_INT_CALLER_PRE             0
#define SHIZUKU_C6_FASTCALL_STAGE2_INT_KERNEL_ENTRY_POST_SAVE 1
#define SHIZUKU_C6_FASTCALL_STAGE2_INT_KERNEL_ENTRY           1  // Compatibility alias
#define SHIZUKU_C6_FASTCALL_STAGE2_INT_CALLEE                 2
#define SHIZUKU_C6_FASTCALL_STAGE2_INT_KERNEL_RETURN_POST_SAVE 3
#define SHIZUKU_C6_FASTCALL_STAGE2_INT_KERNEL_RETURN          3  // Compatibility alias
#define SHIZUKU_C6_FASTCALL_STAGE2_INT_CALLER_POST            4

#define SHIZUKU_C6_FASTCALL_STAGE2_REC_MAGIC       0
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_VERSION     4
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_STATUS      8
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_ERROR_CODE  12
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_ROUNDS_REQ  16
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_ROUNDS_COMP 20
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_PLACEMENT   24
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_FLAGS       28
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_WINDOW1     96
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_WINDOW2     112
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_E2E         128
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_INTERVALS   160
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_SAMPLES     320
#define SHIZUKU_C6_FASTCALL_STAGE2_REC_SIZE        8512

#define SHIZUKU_C6_FASTCALL_STAGE2_CURR_TS0  0
#define SHIZUKU_C6_FASTCALL_STAGE2_CURR_TS1  4
#define SHIZUKU_C6_FASTCALL_STAGE2_CURR_TS2  8
#define SHIZUKU_C6_FASTCALL_STAGE2_CURR_TS3  12
#define SHIZUKU_C6_FASTCALL_STAGE2_CURR_TS4  16
#define SHIZUKU_C6_FASTCALL_STAGE2_CURR_TS5  20
#define SHIZUKU_C6_FASTCALL_STAGE2_CURR_TS6  24
#define SHIZUKU_C6_FASTCALL_STAGE2_CURR_TS7  28
#define SHIZUKU_C6_FASTCALL_STAGE2_CURR_SIZE 32

#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_TS0          0
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_TS1          4
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_TS2          8
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_TS3          12
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_TS4          16
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_TS5          20
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_TS6          24
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_TS7          28
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_CALLER_PRE   32
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_KERNEL_ENTRY 36
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_CALLEE       40
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_KERNEL_RET   44
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_CALLER_POST  48
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_E2E          52
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_WIN1         56
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_WIN2         60
#define SHIZUKU_C6_FASTCALL_STAGE2_SMP_SIZE         64

#ifndef __ASSEMBLER__

#include <stdint.h>
#include <stdbool.h>

// Private M-mode fixed descriptor metadata (O(1) lookup/depth/latest-call)
// All fields are fixed uint32_t to maintain exact 20-byte RV32 ABI geometry
// even when host concepts tests are compiled on a 64-bit architecture.
typedef struct {
    uint32_t entry_pc;       // 0: Fixed Callee entry PC (caller does not supply)
    uint32_t caller_sp;      // 4: Saved caller SP during call
    uint32_t caller_pc;      // 8: Saved caller return PC (mepc + 4)
    uint32_t depth;          // 12: Call depth (0: idle, 1: active)
    uint32_t call_count;     // 16: Total completed calls
} shizuku_c6_fastcall_descriptor_t;

// Independent fastcall RAM record (dumpable via sysbus / nm)
typedef struct {
    uint32_t magic;                // 0: 0x46433152 ("FC1R")
    uint32_t version;              // 4: 1
    uint32_t status;               // 8: 0: UNINIT, 1: PASSED, 2: FAILED
    uint32_t error_code;           // 12: SHIZUKU_C6_FASTCALL_ERR_*
    uint32_t rounds_requested;     // 16: 128
    uint32_t rounds_completed;     // 20: completed sample count
    uint32_t placement_mode;       // 24: 1 (IRAM asm)
    uint32_t reserved0;            // 28: SHIZUKU_C6_FASTCALL_MODE_* bits

    // HP CPU core clock verification (SYSTIMER 16MHz vs PCCR 0x7E2)
    uint32_t systimer_ticks;       // 32: SYSTIMER delta (ticks at 16MHz)
    uint32_t cpu_cycle_delta;      // 36: CPU cycle counter delta
    uint32_t cpu_mhz;              // 40: Computed CPU MHz (e.g. 160)
    uint32_t clock_verified;       // 44: 1 if within valid range

    // Overhead and end-to-end statistics (cycles)
    uint32_t calibrated_overhead;  // 48: 2-read measurement hook cost
    uint32_t raw_min;              // 52: End-to-end raw min cycles (stub included)
    uint32_t raw_mean;             // 56: End-to-end raw mean cycles (stub included)
    uint32_t net_min;              // 60: raw_min - calibrated_overhead
    uint32_t net_mean;             // 64: raw_mean - calibrated_overhead

    // Measured asm body duration per trap (cycles; does NOT represent full architectural IRQ-off window)
    uint32_t max_trap1_cycles;     // 68: Measured Trap 1 (CALL) asm body duration
    uint32_t max_trap2_cycles;     // 72: Measured Trap 2 (RETURN) asm body duration

    // Assertion verification mask
    uint32_t assert_pass_mask;     // 76: SHIZUKU_C6_FASTCALL_ASSERT_* mask
} shizuku_c6_fastcall_record_t;

// Stage 2: Conservative IRQ-off window summary stats (16 bytes)
typedef struct {
    uint32_t raw_min;          // Raw minimum cycles for this conservative window
    uint32_t raw_mean;         // Raw mean cycles
    uint32_t raw_max;          // Raw maximum cycles (Note: empirical sample max, NOT a formal WCET guarantee!)
    uint32_t is_full_irqoff;   // Always 0: Conservative window strictly envelopes IRQ-off, but is not full IRQ-off
} shizuku_c6_fastcall_stage2_window_stats_t;

// Stage 2: Interval summary statistics (32 bytes)
typedef struct {
    uint32_t interval_id;      // Interval ID (0..4 or 99 for E2E)
    uint32_t raw_min;          // Raw minimum cycles
    uint32_t raw_mean;         // Raw mean cycles
    uint32_t raw_max;          // Raw maximum cycles
    uint32_t net_min;          // Net min (0 if net_valid == 0)
    uint32_t net_mean;         // Net mean (0 if net_valid == 0)
    uint32_t net_max;          // Net max (0 if net_valid == 0)
    uint32_t net_valid;        // SHIZUKU_C6_FASTCALL_NET_UNKNOWN (0) or NET_VALID (1)
} shizuku_c6_fastcall_stage2_interval_t;

// Stage 2: Per-sample record preserving all 8 raw timestamps and interval cycles (64 bytes)
typedef struct {
    // Exact raw timestamps for this round (32 bytes)
    uint32_t ts0_callsite_start;              // 0: Outer caller callsite measurement start
    uint32_t ts1_call_ecall_pre;               // 4: CALL ecall immediately before
    uint32_t ts2_call_router_post_prologue;    // 8: Router entry post-preservation sample (instruction 4)
    uint32_t ts3_callee_entry;                 // 12: Callee entry
    uint32_t ts4_ret_ecall_pre;                // 16: RETURN ecall immediately before
    uint32_t ts5_ret_router_post_prologue;     // 20: Return router entry post-preservation sample (instruction 4)
    uint32_t ts6_caller_resume;                // 24: Caller resume immediately after Trap 2 mret
    uint32_t ts7_callsite_end;                 // 28: Outer caller callsite measurement end

    // 5 per-interval raw cycle deltas (20 bytes)
    uint32_t caller_pre_cycles;                // 32: ts1 - ts0
    uint32_t kernel_entry_post_save_cycles;    // 36: ts3 - ts2 (post-preservation router sample -> callee entry)
    uint32_t callee_cycles;                    // 40: ts4 - ts3 (callee entry -> RETURN ecall pre)
    uint32_t kernel_return_post_save_cycles;   // 44: ts6 - ts5 (post-preservation ret router sample -> caller resume)
    uint32_t caller_post_cycles;               // 48: ts7 - ts6 (caller resume -> outer callsite end)

    // End-to-end and conservative IRQ-off windows (12 bytes)
    uint32_t end_to_end_cycles;                // 52: ts7 - ts0 (outer callsite start -> outer callsite end)
    uint32_t irqoff_window1_call;              // 56: ts3 - ts1 (CALL ecall pre -> callee entry; full conservative envelope)
    uint32_t irqoff_window2_ret;               // 60: ts6 - ts4 (RETURN ecall pre -> caller resume; full conservative envelope)
} shizuku_c6_fastcall_stage2_sample_t;

#define kernel_entry_cycles  kernel_entry_post_save_cycles
#define kernel_return_cycles kernel_return_post_save_cycles

// Stage 2: Staging timestamps captured across caller, trap handlers, and callee (32 bytes)
typedef struct {
    uint32_t ts0_callsite_start;              // 0
    uint32_t ts1_call_ecall_pre;               // 4
    uint32_t ts2_call_router_post_prologue;    // 8: Router entry post-preservation sample
    uint32_t ts3_callee_entry;                 // 12
    uint32_t ts4_ret_ecall_pre;                // 16
    uint32_t ts5_ret_router_post_prologue;     // 20: Return router entry post-preservation sample
    uint32_t ts6_caller_resume;                // 24
    uint32_t ts7_callsite_end;                 // 28
} shizuku_c6_fastcall_stage2_current_timestamps_t;

#define ts2_call_trap_entry ts2_call_router_post_prologue
#define ts5_ret_trap_entry  ts5_ret_router_post_prologue

// Stage 2: Independent fastcall RAM record (8512 bytes)
typedef struct {
    // 0..31: Header & Identification (32 bytes)
    uint32_t magic;                // 0: 0x46433252 ("FC2R")
    uint32_t version;              // 4: 1
    uint32_t status;               // 8: 0: UNINIT, 1: PASSED, 2: FAILED, 3: BENCH_ONLY
    uint32_t error_code;           // 12: SHIZUKU_C6_FASTCALL_ERR_*
    uint32_t rounds_requested;     // 16: 128
    uint32_t rounds_completed;     // 20: completed sample count (>= 128)
    uint32_t placement_mode;       // 24: 1 (IRAM asm)
    uint32_t flags;                // 28: macro mode flags

    // 32..63: CPU Clock & Calibration Metadata (32 bytes)
    uint32_t systimer_ticks;       // 32: SYSTIMER delta (ticks at 16MHz)
    uint32_t cpu_cycle_delta;      // 36: CPU cycle counter delta
    uint32_t cpu_mhz;              // 40: Computed CPU MHz (e.g. 160)
    uint32_t clock_verified;       // 44: 1 if verified via SYSTIMER
    uint32_t calibrated_overhead;  // 48: 2-read measurement overhead (cycles)
    uint32_t e2e_net_valid;        // 52: 1 if E2E net is valid, 0 if unknown
    uint32_t interval_net_valid;   // 56: 0: Unknown (no defensible cross-boundary calibration)
    uint32_t wcet_guaranteed;      // 60: 0: Sample max is empirical only, NOT a formal WCET guarantee!

    // 64..95: MIE / Interruptibility Metadata (32 bytes)
    uint32_t mie_caller_pre;       // 64: SHIZUKU_C6_MIE_UNKNOWN (caller context unverified)
    uint32_t mie_kernel_entry;     // 68: SHIZUKU_C6_MIE_DISABLED (cleared by hardware on ecall)
    uint32_t mie_callee;           // 72: SHIZUKU_C6_MIE_UNKNOWN (restored from caller MPIE)
    uint32_t mie_kernel_return;    // 76: SHIZUKU_C6_MIE_DISABLED (cleared by hardware on return ecall)
    uint32_t mie_caller_post;      // 80: SHIZUKU_C6_MIE_UNKNOWN (restored from callee MPIE)
    uint32_t assert_pass_mask;     // 84: SHIZUKU_C6_FASTCALL_ASSERT_* mask
    uint32_t trap1_body_max;       // 88: Measured Trap 1 asm body (18 cyc; NOT full IRQ-off)
    uint32_t trap2_body_max;       // 92: Measured Trap 2 asm body (16 cyc; NOT full IRQ-off)

    // 96..127: Conservative IRQ-Off Windows (32 bytes)
    shizuku_c6_fastcall_stage2_window_stats_t window1_call; // 96: CALL ecall pre -> callee entry (16 bytes)
    shizuku_c6_fastcall_stage2_window_stats_t window2_ret;  // 112: RETURN ecall pre -> caller resume (16 bytes)

    // 128..159: End-to-End Summary Statistics (32 bytes)
    shizuku_c6_fastcall_stage2_interval_t e2e_summary;     // 128: E2E raw min/mean/max, net min/mean/max, validity

    // 160..319: 5 Intervals Summary Statistics (160 bytes = 5 * 32)
    //   intervals[0]: caller-pre    (ts0 -> ts1)
    //   intervals[1]: kernel-entry  (ts2 -> ts3)
    //   intervals[2]: callee        (ts3 -> ts4)
    //   intervals[3]: kernel-return (ts5 -> ts6)
    //   intervals[4]: caller-post   (ts6 -> ts7)
    shizuku_c6_fastcall_stage2_interval_t intervals[SHIZUKU_C6_FASTCALL_STAGE2_INTERVAL_COUNT];

    // 320..8511: All 128 Raw Samples (8192 bytes = 128 * 64 bytes)
    shizuku_c6_fastcall_stage2_sample_t samples[SHIZUKU_C6_FASTCALL_ROUNDS];
} shizuku_c6_fastcall_stage2_record_t;

// Stage 2 pure calculation & validation helper functions
static inline uint32_t shizuku_c6_fastcall_stage2_compute_sample(
    const shizuku_c6_fastcall_stage2_current_timestamps_t* ts,
    shizuku_c6_fastcall_stage2_sample_t* out_sample)
{
    if (!ts || !out_sample) {
        return SHIZUKU_C6_FASTCALL_ERR_SAMPLE_SHORTAGE;
    }

    const uint32_t ts0 = ts->ts0_callsite_start;
    const uint32_t ts1 = ts->ts1_call_ecall_pre;
    const uint32_t ts2 = ts->ts2_call_trap_entry;
    const uint32_t ts3 = ts->ts3_callee_entry;
    const uint32_t ts4 = ts->ts4_ret_ecall_pre;
    const uint32_t ts5 = ts->ts5_ret_trap_entry;
    const uint32_t ts6 = ts->ts6_caller_resume;
    const uint32_t ts7 = ts->ts7_callsite_end;

    // Unsigned 32-bit modulo subtraction correctly resolves counter wrapping
    const uint32_t d0 = ts1 - ts0; // caller-pre: outer callsite start -> CALL ecall pre
    const uint32_t d1 = ts3 - ts2; // kernel-entry-post-save: post-preservation router sample -> callee entry
    const uint32_t d2 = ts4 - ts3; // callee: callee entry -> RETURN ecall pre
    const uint32_t d3 = ts6 - ts5; // kernel-return-post-save: post-preservation ret router sample -> caller resume
    const uint32_t d4 = ts7 - ts6; // caller-post: caller resume -> outer callsite end

    const uint32_t w1 = ts3 - ts1; // conservative window 1: CALL ecall pre -> callee entry (full envelope)
    const uint32_t w2 = ts6 - ts4; // conservative window 2: RETURN ecall pre -> caller resume (full envelope)
    const uint32_t e2e = ts7 - ts0; // full end-to-end: outer callsite start -> outer callsite end

    // Sanity checks:
    // 1. Frozen counter or extreme delay detection (> 500k cycles or e2e == 0)
    if (e2e == 0 || e2e > 500000u) {
        return SHIZUKU_C6_FASTCALL_ERR_SAMPLE_SHORTAGE;
    }
    // 2. Individual interval anomaly (> 200k cycles)
    if (d0 > 200000u || d1 > 200000u || d2 > 200000u || d3 > 200000u || d4 > 200000u) {
        return SHIZUKU_C6_FASTCALL_ERR_SAMPLE_SHORTAGE;
    }
    // 3. Conservative window nesting invariant:
    //    window1 strictly envelopes kernel_entry_post_save
    //    (w1 >= d1, diff = ts2 - ts1 = ecall hardware trap + vector jump + router prologue)
    //    window2 strictly envelopes kernel_return_post_save
    //    (w2 >= d3, diff = ts5 - ts4 = ret ecall hardware trap + vector jump + router prologue)
    if (w1 < d1 || w2 < d3) {
        return SHIZUKU_C6_FASTCALL_ERR_SAMPLE_SHORTAGE;
    }

    out_sample->ts0_callsite_start   = ts0;
    out_sample->ts1_call_ecall_pre   = ts1;
    out_sample->ts2_call_trap_entry  = ts2;
    out_sample->ts3_callee_entry     = ts3;
    out_sample->ts4_ret_ecall_pre    = ts4;
    out_sample->ts5_ret_trap_entry   = ts5;
    out_sample->ts6_caller_resume    = ts6;
    out_sample->ts7_callsite_end     = ts7;

    out_sample->caller_pre_cycles    = d0;
    out_sample->kernel_entry_cycles  = d1;
    out_sample->callee_cycles        = d2;
    out_sample->kernel_return_cycles = d3;
    out_sample->caller_post_cycles   = d4;

    out_sample->end_to_end_cycles    = e2e;
    out_sample->irqoff_window1_call  = w1;
    out_sample->irqoff_window2_ret   = w2;

    return SHIZUKU_C6_FASTCALL_ERR_NONE;
}

// Stage 2: Store sample into volatile destination preserving volatile semantics across all 16 fields
static inline void shizuku_c6_fastcall_stage2_store_sample(
    volatile shizuku_c6_fastcall_stage2_sample_t* dst,
    const shizuku_c6_fastcall_stage2_sample_t* src)
{
    if (!dst || !src) return;
    dst->ts0_callsite_start            = src->ts0_callsite_start;
    dst->ts1_call_ecall_pre            = src->ts1_call_ecall_pre;
    dst->ts2_call_router_post_prologue = src->ts2_call_router_post_prologue;
    dst->ts3_callee_entry              = src->ts3_callee_entry;
    dst->ts4_ret_ecall_pre             = src->ts4_ret_ecall_pre;
    dst->ts5_ret_router_post_prologue  = src->ts5_ret_router_post_prologue;
    dst->ts6_caller_resume             = src->ts6_caller_resume;
    dst->ts7_callsite_end              = src->ts7_callsite_end;

    dst->caller_pre_cycles             = src->caller_pre_cycles;
    dst->kernel_entry_post_save_cycles = src->kernel_entry_post_save_cycles;
    dst->callee_cycles                 = src->callee_cycles;
    dst->kernel_return_post_save_cycles= src->kernel_return_post_save_cycles;
    dst->caller_post_cycles            = src->caller_post_cycles;

    dst->end_to_end_cycles             = src->end_to_end_cycles;
    dst->irqoff_window1_call           = src->irqoff_window1_call;
    dst->irqoff_window2_ret            = src->irqoff_window2_ret;
}

static inline void shizuku_c6_fastcall_stage2_init_record(
    volatile shizuku_c6_fastcall_stage2_record_t* rec,
    uint32_t flags)
{
    if (!rec) return;

    rec->magic = SHIZUKU_C6_FASTCALL_STAGE2_MAGIC;
    rec->version = SHIZUKU_C6_FASTCALL_STAGE2_VERSION;
    rec->status = SHIZUKU_C6_FASTCALL_STATUS_UNINIT;
    rec->error_code = SHIZUKU_C6_FASTCALL_ERR_NONE;
    rec->rounds_requested = SHIZUKU_C6_FASTCALL_ROUNDS;
    rec->rounds_completed = 0;
    rec->placement_mode = 1u; // 1: IRAM asm
    rec->flags = flags;

    rec->systimer_ticks = 0;
    rec->cpu_cycle_delta = 0;
    rec->cpu_mhz = 0;
    rec->clock_verified = 0;
    rec->calibrated_overhead = 0;
    rec->e2e_net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;
    rec->interval_net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;
    rec->wcet_guaranteed = 0; // Sample max is empirical only, NOT a formal WCET guarantee!

    // MIE Metadata:
    // Kernel entry and kernel return are provably MIE_DISABLED (0) by RISC-V hardware ecall.
    // Caller and callee are MIE_UNKNOWN (2) because caller context before trap is not statically fixed.
    rec->mie_caller_pre    = SHIZUKU_C6_MIE_UNKNOWN;
    rec->mie_kernel_entry  = SHIZUKU_C6_MIE_DISABLED;
    rec->mie_callee        = SHIZUKU_C6_MIE_UNKNOWN;
    rec->mie_kernel_return = SHIZUKU_C6_MIE_DISABLED;
    rec->mie_caller_post   = SHIZUKU_C6_MIE_UNKNOWN;

    rec->assert_pass_mask = 0;
    rec->trap1_body_max = 0;
    rec->trap2_body_max = 0;

    rec->window1_call.raw_min = 0xFFFFFFFFu;
    rec->window1_call.raw_mean = 0;
    rec->window1_call.raw_max = 0;
    rec->window1_call.is_full_irqoff = 0; // Conservative window strictly envelopes IRQ-off, but is not full IRQ-off

    rec->window2_ret.raw_min = 0xFFFFFFFFu;
    rec->window2_ret.raw_mean = 0;
    rec->window2_ret.raw_max = 0;
    rec->window2_ret.is_full_irqoff = 0;

    rec->e2e_summary.interval_id = 99u;
    rec->e2e_summary.raw_min = 0xFFFFFFFFu;
    rec->e2e_summary.raw_mean = 0;
    rec->e2e_summary.raw_max = 0;
    rec->e2e_summary.net_min = 0;
    rec->e2e_summary.net_mean = 0;
    rec->e2e_summary.net_max = 0;
    rec->e2e_summary.net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;

    for (uint32_t i = 0; i < SHIZUKU_C6_FASTCALL_STAGE2_INTERVAL_COUNT; ++i) {
        rec->intervals[i].interval_id = i;
        rec->intervals[i].raw_min = 0xFFFFFFFFu;
        rec->intervals[i].raw_mean = 0;
        rec->intervals[i].raw_max = 0;
        rec->intervals[i].net_min = 0;
        rec->intervals[i].net_mean = 0;
        rec->intervals[i].net_max = 0;
        rec->intervals[i].net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;
    }
}

static inline void shizuku_c6_fastcall_stage2_finalize_stats(
    volatile shizuku_c6_fastcall_stage2_record_t* rec,
    uint32_t calibrated_overhead,
    uint32_t clock_verified)
{
    if (!rec) return;

    rec->calibrated_overhead = calibrated_overhead;
    rec->clock_verified = clock_verified;

    // Fail closed: if record is already marked failed, error code set, or shortage (< 128 rounds)
    if (rec->status == SHIZUKU_C6_FASTCALL_STATUS_FAILED ||
        rec->error_code != SHIZUKU_C6_FASTCALL_ERR_NONE ||
        rec->rounds_completed < rec->rounds_requested) {
        rec->status = SHIZUKU_C6_FASTCALL_STATUS_FAILED;
        rec->e2e_net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;
        rec->e2e_summary.net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;
        rec->e2e_summary.net_min = 0;
        rec->e2e_summary.net_mean = 0;
        rec->e2e_summary.net_max = 0;
        rec->interval_net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;
        for (uint32_t i = 0; i < SHIZUKU_C6_FASTCALL_STAGE2_INTERVAL_COUNT; ++i) {
            rec->intervals[i].net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;
            rec->intervals[i].net_min = 0;
            rec->intervals[i].net_mean = 0;
            rec->intervals[i].net_max = 0;
        }
        return;
    }

    const uint32_t N = rec->rounds_completed;
    if (N == 0) return;

    // Accumulators (uint64_t to prevent mean overflow)
    uint64_t sum_intervals[SHIZUKU_C6_FASTCALL_STAGE2_INTERVAL_COUNT] = {0};
    uint64_t sum_w1 = 0;
    uint64_t sum_w2 = 0;
    uint64_t sum_e2e = 0;

    for (uint32_t r = 0; r < N; ++r) {
        const uint32_t d0 = rec->samples[r].caller_pre_cycles;
        const uint32_t d1 = rec->samples[r].kernel_entry_cycles;
        const uint32_t d2 = rec->samples[r].callee_cycles;
        const uint32_t d3 = rec->samples[r].kernel_return_cycles;
        const uint32_t d4 = rec->samples[r].caller_post_cycles;
        const uint32_t w1 = rec->samples[r].irqoff_window1_call;
        const uint32_t w2 = rec->samples[r].irqoff_window2_ret;
        const uint32_t e2e = rec->samples[r].end_to_end_cycles;

        sum_intervals[0] += d0;
        sum_intervals[1] += d1;
        sum_intervals[2] += d2;
        sum_intervals[3] += d3;
        sum_intervals[4] += d4;
        sum_w1 += w1;
        sum_w2 += w2;
        sum_e2e += e2e;

        if (d0 < rec->intervals[0].raw_min) rec->intervals[0].raw_min = d0;
        if (d0 > rec->intervals[0].raw_max) rec->intervals[0].raw_max = d0;
        if (d1 < rec->intervals[1].raw_min) rec->intervals[1].raw_min = d1;
        if (d1 > rec->intervals[1].raw_max) rec->intervals[1].raw_max = d1;
        if (d2 < rec->intervals[2].raw_min) rec->intervals[2].raw_min = d2;
        if (d2 > rec->intervals[2].raw_max) rec->intervals[2].raw_max = d2;
        if (d3 < rec->intervals[3].raw_min) rec->intervals[3].raw_min = d3;
        if (d3 > rec->intervals[3].raw_max) rec->intervals[3].raw_max = d3;
        if (d4 < rec->intervals[4].raw_min) rec->intervals[4].raw_min = d4;
        if (d4 > rec->intervals[4].raw_max) rec->intervals[4].raw_max = d4;

        if (w1 < rec->window1_call.raw_min) rec->window1_call.raw_min = w1;
        if (w1 > rec->window1_call.raw_max) rec->window1_call.raw_max = w1;
        if (w2 < rec->window2_ret.raw_min) rec->window2_ret.raw_min = w2;
        if (w2 > rec->window2_ret.raw_max) rec->window2_ret.raw_max = w2;

        if (e2e < rec->e2e_summary.raw_min) rec->e2e_summary.raw_min = e2e;
        if (e2e > rec->e2e_summary.raw_max) rec->e2e_summary.raw_max = e2e;
    }

    // All per-interval net values remain Unknown (0).
    // Specifically:
    // - intervals[1] (router post-prologue -> callee) and intervals[3] (ret router post-prologue -> caller resume)
    //   exclude hardware trap entry, vector jump, and router prologue (csrrw, beqz, sw t1).
    // - Strict architectural trap-entry -> callee and trap-return -> caller-resume intervals are unmeasured
    //   in internal software and remain strictly Unknown / Invalid.
    // - Conservative envelopes window1_call (ts3 - ts1) and window2_ret (ts6 - ts4) remain valid.
    for (uint32_t i = 0; i < SHIZUKU_C6_FASTCALL_STAGE2_INTERVAL_COUNT; ++i) {
        rec->intervals[i].raw_mean = (uint32_t)(sum_intervals[i] / N);
        rec->intervals[i].net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;
        rec->intervals[i].net_min = 0;
        rec->intervals[i].net_mean = 0;
        rec->intervals[i].net_max = 0;
    }
    rec->interval_net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;

    rec->window1_call.raw_mean = (uint32_t)(sum_w1 / N);
    rec->window1_call.is_full_irqoff = 0;
    rec->window2_ret.raw_mean = (uint32_t)(sum_w2 / N);
    rec->window2_ret.is_full_irqoff = 0;

    rec->e2e_summary.raw_mean = (uint32_t)(sum_e2e / N);

    // E2E Net calibration validity check:
    // Requires verified CPU clock, positive overhead, overhead strictly smaller than raw_min,
    // and matching external t_start/t_end read pattern.
    if (clock_verified == 1 &&
        calibrated_overhead > 0 &&
        calibrated_overhead < rec->e2e_summary.raw_min &&
        calibrated_overhead != 0xFFFFFFFFu) {
        rec->e2e_summary.net_valid = SHIZUKU_C6_FASTCALL_NET_VALID;
        rec->e2e_summary.net_min = rec->e2e_summary.raw_min - calibrated_overhead;
        rec->e2e_summary.net_mean = (rec->e2e_summary.raw_mean > calibrated_overhead)
                                    ? (rec->e2e_summary.raw_mean - calibrated_overhead) : 0;
        rec->e2e_summary.net_max = (rec->e2e_summary.raw_max > calibrated_overhead)
                                    ? (rec->e2e_summary.raw_max - calibrated_overhead) : 0;
        rec->e2e_net_valid = SHIZUKU_C6_FASTCALL_NET_VALID;
    } else {
        rec->e2e_summary.net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;
        rec->e2e_summary.net_min = 0;
        rec->e2e_summary.net_mean = 0;
        rec->e2e_summary.net_max = 0;
        rec->e2e_net_valid = SHIZUKU_C6_FASTCALL_NET_UNKNOWN;
    }
}

#ifdef __cplusplus
extern "C" {
#endif

extern volatile shizuku_c6_fastcall_descriptor_t shizuku_c6_fastcall_descriptor;
extern volatile shizuku_c6_fastcall_record_t shizuku_c6_fastcall_record;
#if SHIZUKU_C6_FASTCALL_STAGE2_TIMING
extern volatile shizuku_c6_fastcall_stage2_record_t shizuku_c6_fastcall_stage2_record;
extern volatile shizuku_c6_fastcall_stage2_current_timestamps_t shizuku_c6_fastcall_stage2_current_sample;
#endif
extern volatile uint32_t shizuku_c6_fastcall_s_nonzero;
extern volatile uint32_t shizuku_c6_fastcall_gp_tp_nonzero;
extern volatile uint32_t shizuku_c6_fastcall_nonarg_nonzero;
extern volatile uint32_t shizuku_c6_fastcall_probe_return;
extern volatile uint32_t shizuku_c6_fastcall_probe_scratch;
extern volatile uint32_t shizuku_c6_fastcall_probe_gp_before;
extern volatile uint32_t shizuku_c6_fastcall_probe_gp_after;
extern volatile uint32_t shizuku_c6_fastcall_probe_tp_before;
extern volatile uint32_t shizuku_c6_fastcall_probe_tp_after;
extern volatile uint32_t shizuku_c6_fastcall_probe_s_after[12];

// Asm routines
uint32_t shizuku_c6_fastcall_stub(uint32_t arg);
void shizuku_c6_fastcall_return_stub(void);
uint32_t shizuku_c6_fastcall_leaf_callee(uint32_t arg);
void shizuku_c6_fastcall_probe(void);

// Runner / selftest entry point
void shizuku_c6_run_fastcall_selftests(void);

#ifdef __cplusplus
}
#endif

#endif // __ASSEMBLER__

#endif // SHIZUKU_C6_FASTCALL_H
