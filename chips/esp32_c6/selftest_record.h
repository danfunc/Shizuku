#ifndef SHIZUKU_C6_SELFTEST_RECORD_H
#define SHIZUKU_C6_SELFTEST_RECORD_H

#include <stdint.h>
#include <stdbool.h>

#define SHIZUKU_C6_SELFTEST_MAGIC 0x53545354u  // "STST"

#define SHIZUKU_C6_LADDER_CALL    (1u << 0)
#define SHIZUKU_C6_LADDER_BINDING (1u << 1)
#define SHIZUKU_C6_LADDER_THREAD  (1u << 2)
#define SHIZUKU_C6_LADDER_MEMORY  (1u << 3)
#define SHIZUKU_C6_LADDER_STREAM  (1u << 4)
#define SHIZUKU_C6_LADDER_ALL_MASK (SHIZUKU_C6_LADDER_CALL | \
                                    SHIZUKU_C6_LADDER_BINDING | \
                                    SHIZUKU_C6_LADDER_THREAD | \
                                    SHIZUKU_C6_LADDER_MEMORY | \
                                    SHIZUKU_C6_LADDER_STREAM)

#define SHIZUKU_C6_SELFTEST_STATUS_RUNNING 0u
#define SHIZUKU_C6_SELFTEST_STATUS_PASSED  1u
#define SHIZUKU_C6_SELFTEST_STATUS_FAILED  2u

typedef struct {
    uint32_t magic;            // 0x53545354 ("STST")
    uint32_t completed;        // 0: 未完了/実行中, 1: 正常完了(全PASS), 2: 失敗完了(FAILあり)
    uint32_t passed;           // 通過アサーション総数
    uint32_t failed;           // 失敗アサーション総数
    uint32_t first_failure_id; // 最初の失敗アサーションID (0: なし)
    uint32_t ladders_mask;     // 完了 ladder ビットマスク (0x1f: 5 ladder 全完了)
    uint32_t last_test_id;     // 最後に到達/完了したアサーションID (ladder完了時はその最終assert ID、失敗時は失敗assert ID)
    uint32_t reserved;
} shizuku_c6_selftest_record_t;

#ifdef __cplusplus
extern "C" {
#endif

extern volatile shizuku_c6_selftest_record_t shizuku_c6_selftest_record;

uint32_t shizuku_c6_selftest_name_to_id(const char *name);
void shizuku_c6_selftest_record_init(void);
void shizuku_c6_selftest_record_assert(const char *name, bool ok, unsigned long got, unsigned long want);
void shizuku_c6_selftest_record_ladder_done(uint32_t ladder_bit, uint32_t last_ladder_assert_id);
void shizuku_c6_selftest_record_finish(void);
void shizuku_c6_run_selftests(void);

#ifdef __cplusplus
}
#endif

#endif // SHIZUKU_C6_SELFTEST_RECORD_H
