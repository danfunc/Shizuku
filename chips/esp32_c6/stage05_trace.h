#ifndef SHIZUKU_C6_STAGE05_TRACE_H
#define SHIZUKU_C6_STAGE05_TRACE_H

#include <stdint.h>
#include <stdbool.h>

#define SHIZUKU_C6_STAGE05_MAGIC   0x53303554u // "S05T"
#define SHIZUKU_C6_STAGE05_VERSION 1u

#define SHIZUKU_C6_STAGE05_STATUS_UNINIT   0u
#define SHIZUKU_C6_STAGE05_STATUS_PASSED   1u
#define SHIZUKU_C6_STAGE05_STATUS_FAILED   2u

#define SHIZUKU_C6_STAGE05_ERR_NONE             0u
#define SHIZUKU_C6_STAGE05_ERR_COUNTER_INIT     1u
#define SHIZUKU_C6_STAGE05_ERR_SAMPLE_SHORTAGE  2u
#define SHIZUKU_C6_STAGE05_ERR_SEQUENCE_ORDER   3u
#define SHIZUKU_C6_STAGE05_ERR_COUNTER_FROZEN   4u
#define SHIZUKU_C6_STAGE05_ERR_CLOCK_UNVERIFIED 5u

#define SHIZUKU_C6_STAGE05_INTERVAL_COUNT 12u
#define SHIZUKU_C6_STAGE05_ROUNDS         128u

#if defined(SHIZUKU_C6_STAGE05_IRAM) && SHIZUKU_C6_STAGE05_IRAM
#define SHIZUKU_STAGE05_ATTR __attribute__((section(".iram1.text")))
#else
#define SHIZUKU_STAGE05_ATTR
#endif

// 各隣接区間の統計
typedef struct {
    uint32_t from_id;
    uint32_t to_id;
    uint32_t raw_min;
    uint32_t raw_mean;
    uint32_t net_min;
    uint32_t net_mean;
} shizuku_c6_stage05_interval_t;

// 校正中間値の診断記録 (BOARD::diag_printf 出力用)
typedef struct {
    uint32_t csrr_min;
    uint32_t csrr_mean;
    uint32_t hook_te_net_min;
    uint32_t hook_te_net_mean;
    uint32_t hook_te_raw_min;
    uint32_t hook_te_raw_mean;
    uint32_t hook_sd_net_min;
    uint32_t hook_sd_net_mean;
    uint32_t hook_sd_raw_min;
    uint32_t hook_sd_raw_mean;
    uint32_t hook_hd_net_min;
    uint32_t hook_hd_net_mean;
    uint32_t hook_hd_raw_min;
    uint32_t hook_hd_raw_mean;
    uint32_t hook_rp_net_min;
    uint32_t hook_rp_net_mean;
    uint32_t hook_rp_raw_min;
    uint32_t hook_rp_raw_mean;
} shizuku_c6_stage05_calib_diag_t;

// OpenOCD sysbus read (mdw / dump_image) で回収可能な RAM レコード (368 バイト / 92 語)
typedef struct {
    uint32_t magic;              // 0x53303554 ("S05T")
    uint32_t version;            // 1
    uint32_t status;             // 0: UNINIT, 1: PASSED, 2: FAILED
    uint32_t error_code;         // SHIZUKU_C6_STAGE05_ERR_*
    uint32_t rounds_requested;   // 128
    uint32_t rounds_completed;   // 有効測定サンプル数
    uint32_t placement_mode;     // 0: Flash baseline, 1: IRAM candidate
    uint32_t reserved0;

    // HP CPU クロック独立検証 (SYSTIMER 16MHz 対比)
    uint32_t systimer_ticks;     // SYSTIMER 差分 (16MHz tick)
    uint32_t cpu_cycle_delta;    // CPU サイクルカウンタ差分
    uint32_t cpu_mhz;            // 算出された CPU MHz (未検証・異常時は 0)
    uint32_t clock_verified;     // 1: SYSTIMER により検証完了, 0: 未検証(unknown)

    // フック費用見積り・摂動
    uint32_t hook_single_raw_min;  // 実フック4種中の最大 net min (cyc)
    uint32_t hook_single_raw_mean; // 実フック4種中の最大 net mean (cyc)
    uint32_t hooks_per_round;      // 1 ラウンドあたりの C フック実行回数 (7)
    uint32_t total_perturbation;   // ラウンド合計の推定フック摂動 (7フック + 4 csrr)

    // 往復全体の合計統計
    uint32_t total_raw_min;
    uint32_t total_raw_mean;
    uint32_t total_net_min;
    uint32_t total_net_mean;

    // 12 隣接区間の統計配列 (288 バイト)
    //  [0] T0 caller_pre       -> T1 trap_entry       : caller ecall命令 + asm例外進入開始
    //  [1] T1 trap_entry       -> T2 c_dispatch       : asm GPR全退避 + Cディスパッチャ呼出し
    //  [2] T2 c_dispatch       -> T3 svc_dispatch     : route_trap 判定 + frame_to_context
    //  [3] T3 svc_dispatch     -> T5 return_prep      : call_frame_push + context_to_frame
    //  [4] T5 return_prep      -> T6 restore_begin    : C dispatch 戻り + asm mepc/mstatus 復元
    //  [5] T6 restore_begin    -> T4 handler_entry    : asm GPR約30本復元(lw) + mret + handler本体C入口
    //  [6] T4 handler_entry    -> T1_ret trap_entry   : handler本体(GET_CURRENT_OBJECT) + return_stub + ecall進入
    //  [7] T1_ret trap_entry   -> T2_ret c_dispatch   : asm GPR全退避 (Trap 2) + Cディスパッチャ呼出し
    //  [8] T2_ret c_dispatch   -> T3_ret svc_dispatch : Trap 2 route_trap + frame_to_context
    //  [9] T3_ret svc_dispatch -> T5_ret return_prep  : primitive::RETURN + call_frame_pop + context_to_frame
    // [10] T5_ret return_prep  -> T6_ret restore_begin: Trap 2 C dispatch 戻り + asm mepc/mstatus 復元
    // [11] T6_ret restore_begin-> T7 caller_post      : Trap 2 asm GPR約30本復元(lw) + mret + caller復帰
    shizuku_c6_stage05_interval_t intervals[SHIZUKU_C6_STAGE05_INTERVAL_COUNT];
} shizuku_c6_stage05_record_t;

// 1 ラウンド分の 13 タイムスタンプ
typedef struct {
    // 往路 (Trap 1: caller -> handler)
    uint32_t t0_caller_pre;          // 0: caller ecall直前
    uint32_t t1_trap_entry;          // 1: asm trap entry
    uint32_t t2_c_dispatch;          // 2: C trap dispatch entry
    uint32_t t3_svc_dispatch;        // 3: kernel svc_dispatch entry
    uint32_t t5_return_prep;         // 4: return preparation (C dispatch 戻り直前)
    uint32_t t6_restore_begin;       // 5: asm GPR復元開始 (mstatus復元直後、ra/t0等復元前; Trap 1)
    uint32_t t4_handler_entry;       // 6: handler本体 entry (current_object)
    // 復路 (Trap 2: return_stub -> caller)
    uint32_t t1_ret_trap_entry;      // 7: asm trap entry (Trap 2)
    uint32_t t2_ret_c_dispatch;      // 8: C trap dispatch entry (Trap 2)
    uint32_t t3_ret_svc_dispatch;    // 9: kernel svc_dispatch entry (Trap 2 RETURN)
    uint32_t t5_ret_return_prep;     // 10: return preparation (Trap 2)
    uint32_t t6_ret_restore_begin;   // 11: asm GPR復元開始 (Trap 2)
    uint32_t t7_caller_post;         // 12: caller復帰後
} shizuku_c6_stage05_sample_t;

#ifdef __cplusplus
extern "C" {
#endif

extern volatile shizuku_c6_stage05_record_t shizuku_c6_stage05_record;
extern volatile shizuku_c6_stage05_sample_t shizuku_c6_stage05_current_sample;
extern volatile shizuku_c6_stage05_calib_diag_t shizuku_c6_stage05_calib_diag;
extern volatile uint32_t shizuku_c6_stage05_active;
extern volatile uint32_t shizuku_c6_stage05_t6;
extern volatile uint32_t shizuku_c6_stage05_is_return;

void shizuku_c6_stage05_init(uint32_t placement_mode);
void shizuku_c6_stage05_calibrate_hooks(void);
bool shizuku_c6_stage05_record_sample(const shizuku_c6_stage05_sample_t *sample);
void shizuku_c6_stage05_finish(void);
void shizuku_c6_stage05_print_report(void);

// フック関数 (C dispatch / handler / svc_dispatch から呼出し)
void shizuku_c6_stage05_hook_trap_entry(uint32_t t1, uint32_t is_return);
void shizuku_c6_stage05_hook_svc_dispatch(uint32_t is_return);
void shizuku_c6_stage05_hook_handler(void);
void shizuku_c6_stage05_hook_return_prep(uint32_t is_return);

#ifdef __cplusplus
}
#endif

#endif // SHIZUKU_C6_STAGE05_TRACE_H
