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

#define SHIZUKU_C6_FASTCALL_MODE_EMPTY_BENCH       (1u << 0)
#define SHIZUKU_C6_FASTCALL_MODE_TRAP_STATS_OFF    (1u << 1)
#define SHIZUKU_C6_FASTCALL_MODE_CLEAR_IN_CALLER   (1u << 2)

#define SHIZUKU_C6_FASTCALL_ERR_NONE             0
#define SHIZUKU_C6_FASTCALL_ERR_RETURN_VAL       1
#define SHIZUKU_C6_FASTCALL_ERR_CALLEE_S_DIRTY   2
#define SHIZUKU_C6_FASTCALL_ERR_CALLER_S_CORRUPT 3
#define SHIZUKU_C6_FASTCALL_ERR_CALLEE_LEAK      4
#define SHIZUKU_C6_FASTCALL_ERR_SAMPLE_SHORTAGE  5
#define SHIZUKU_C6_FASTCALL_ERR_GP_TP_CORRUPT    6
#define SHIZUKU_C6_FASTCALL_ERR_CALLEE_GP_TP_DIRTY 7
#define SHIZUKU_C6_FASTCALL_ERR_CALLEE_NONARG_DIRTY 8

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

#ifdef __cplusplus
extern "C" {
#endif

extern volatile shizuku_c6_fastcall_descriptor_t shizuku_c6_fastcall_descriptor;
extern volatile shizuku_c6_fastcall_record_t shizuku_c6_fastcall_record;
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
