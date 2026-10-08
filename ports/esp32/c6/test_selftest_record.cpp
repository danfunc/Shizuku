#include <cassert>
#include <cstdio>
#include <cstring>
#include "ports/esp32/c6/selftest_record.h"

// call_ladder.cpp 相当の global counters を単体テスト環境で定義
namespace shizuku {
namespace selftest {
uint32_t passed = 0;
uint32_t failed = 0;
void record_fail(const char *name, unsigned long got, unsigned long want);
}
}

int main() {
    std::printf("[TEST] Running C6 selftest RAM record unit tests...\n");

    // 1. 初期化状態の検証
    shizuku_c6_selftest_record_init();
    assert(shizuku_c6_selftest_record.magic == SHIZUKU_C6_SELFTEST_MAGIC);
    assert(shizuku_c6_selftest_record.completed == SHIZUKU_C6_SELFTEST_STATUS_RUNNING);
    assert(shizuku_c6_selftest_record.passed == 0);
    assert(shizuku_c6_selftest_record.failed == 0);
    assert(shizuku_c6_selftest_record.first_failure_id == 0);
    assert(shizuku_c6_selftest_record.ladders_mask == 0);
    assert(shizuku_c6_selftest_record.last_test_id == 0);
    std::printf("[TEST]   1. Initialization assertions passed.\n");

    // 2. PASS アサーションの記録検証
    shizuku_c6_selftest_record_assert("create object", true, 0, 0);
    assert(shizuku_c6_selftest_record.passed == 1);
    assert(shizuku_c6_selftest_record.failed == 0);
    assert(shizuku_c6_selftest_record.first_failure_id == 0);
    assert(shizuku_c6_selftest_record.last_test_id == 0x1001);

    shizuku_c6_selftest_record_assert("call/1: error", true, 0, 0);
    assert(shizuku_c6_selftest_record.passed == 2);
    assert(shizuku_c6_selftest_record.failed == 0);
    assert(shizuku_c6_selftest_record.last_test_id == 0x1002);
    std::printf("[TEST]   2. PASS recording assertions passed.\n");

    // 3. FAIL アサーションと first_failure_id 固定の検証
    shizuku_c6_selftest_record_assert("call/1: value", false, 40, 42);
    assert(shizuku_c6_selftest_record.passed == 2);
    assert(shizuku_c6_selftest_record.failed == 1);
    assert(shizuku_c6_selftest_record.first_failure_id == 0x1003);
    assert(shizuku_c6_selftest_record.last_test_id == 0x1003);

    // 4. 2回目の FAIL で first_failure_id が上書きされないことの検証
    shizuku_c6_selftest_record_assert("call/1: callee identity", false, 99, 10);
    assert(shizuku_c6_selftest_record.passed == 2);
    assert(shizuku_c6_selftest_record.failed == 2);
    assert(shizuku_c6_selftest_record.first_failure_id == 0x1003); // 最初のIDが維持される
    assert(shizuku_c6_selftest_record.last_test_id == 0x1004);
    std::printf("[TEST]   3. FAIL recording & first_failure_id retention passed.\n");

    // 4. 未登録テスト名の検証 (unknown name を成功 / ID 0 にしない)
    shizuku_c6_selftest_record_init();
    shizuku_c6_selftest_record_assert("unknown_random_probe", true, 0, 0);
    assert(shizuku_c6_selftest_record.passed == 0); // 成功にしない
    assert(shizuku_c6_selftest_record.failed == 1); // 失敗として記録
    assert(shizuku_c6_selftest_record.first_failure_id == 0xFFFFu); // ID 0 にしない
    assert(shizuku_c6_selftest_record.last_test_id == 0xFFFFu);
    std::printf("[TEST]   4. Unknown test name rejection assertions passed.\n");

    // 5. 不完全 ladder mask 時の record_finish セマンティクス検証 (RUNNING のまま維持)
    shizuku_c6_selftest_record_init();
    shizuku_c6_selftest_record_assert("create object", true, 0, 0);
    shizuku_c6_selftest_record_ladder_done(SHIZUKU_C6_LADDER_CALL | SHIZUKU_C6_LADDER_BINDING, 0x2013u);
    shizuku_c6_selftest_record_finish();
    assert(shizuku_c6_selftest_record.completed == SHIZUKU_C6_SELFTEST_STATUS_RUNNING); // 未完了は RUNNING (0)
    assert(shizuku_c6_selftest_record.last_test_id == 0x2013u);

    shizuku_c6_selftest_record_assert("call/1: value", false, 40, 42);
    shizuku_c6_selftest_record_finish();
    assert(shizuku_c6_selftest_record.completed == SHIZUKU_C6_SELFTEST_STATUS_RUNNING); // 失敗があっても未完了は RUNNING (0)
    assert(shizuku_c6_selftest_record.first_failure_id == 0x1003u);
    std::printf("[TEST]   5. Incomplete mask keeps RUNNING assertions passed.\n");

    // 6. 全 5 ladder 完了時の終了ステータス検証 (失敗あり -> FAILED (2))
    shizuku_c6_selftest_record_ladder_done(SHIZUKU_C6_LADDER_THREAD | SHIZUKU_C6_LADDER_MEMORY | SHIZUKU_C6_LADDER_STREAM, 0x5008u);
    assert((shizuku_c6_selftest_record.ladders_mask & SHIZUKU_C6_LADDER_ALL_MASK) == SHIZUKU_C6_LADDER_ALL_MASK);
    shizuku_c6_selftest_record_finish();
    assert(shizuku_c6_selftest_record.completed == SHIZUKU_C6_SELFTEST_STATUS_FAILED); // 全完了かつ失敗あり -> FAILED (2)
    assert(shizuku_c6_selftest_record.last_test_id == 0x5008u);
    std::printf("[TEST]   6. Completed with failure status assertions passed.\n");

    // 7. 全 5 ladder 完了時の終了ステータス検証 (全 PASS -> PASSED (1) かつ last_test_id == 0x5008)
    shizuku_c6_selftest_record_init();
    shizuku_c6_selftest_record_assert("create object", true, 0, 0);
    shizuku_c6_selftest_record_assert("spawn: error", true, 0, 0);
    shizuku_c6_selftest_record_ladder_done(SHIZUKU_C6_LADDER_ALL_MASK, 0x5008u);
    shizuku_c6_selftest_record_finish();
    assert(shizuku_c6_selftest_record.passed == 2);
    assert(shizuku_c6_selftest_record.failed == 0);
    assert(shizuku_c6_selftest_record.first_failure_id == 0);
    assert(shizuku_c6_selftest_record.last_test_id == 0x5008u);
    assert(shizuku_c6_selftest_record.completed == SHIZUKU_C6_SELFTEST_STATUS_PASSED); // 全完了かつ全PASS -> PASSED (1)
    std::printf("[TEST]   7. Success completion status assertions passed.\n");

    // 8. record_fail と global counters の連動検証 (runner/check 連携シミュレーション)
    shizuku::selftest::passed = 0;
    shizuku::selftest::failed = 0;
    shizuku_c6_selftest_record_init();
    // 模擬 check PASS
    ++shizuku::selftest::passed;
    // 模擬 check FAIL
    ++shizuku::selftest::failed;
    shizuku::selftest::record_fail("call/1: value", 40, 42);
    assert(shizuku_c6_selftest_record.failed == 1); // record_fail で即座に失敗数が連動
    assert(shizuku_c6_selftest_record.first_failure_id == 0x1003);
    assert(shizuku_c6_selftest_record.last_test_id == 0x1003);

    // 模擬 check FAIL 2回目
    ++shizuku::selftest::failed;
    shizuku::selftest::record_fail("call/1: callee identity", 99, 10);
    assert(shizuku_c6_selftest_record.failed == 2); // 失敗数が 2 に更新
    assert(shizuku_c6_selftest_record.first_failure_id == 0x1003); // 最初のIDが維持される
    assert(shizuku_c6_selftest_record.last_test_id == 0x1004);

    // 模擬 runner の ladder 完了処理 (ladder 境界で最終到達 assert ID を設定)
    shizuku_c6_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c6_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c6_selftest_record_ladder_done(SHIZUKU_C6_LADDER_CALL, 0x101Bu);
    assert(shizuku_c6_selftest_record.passed == 1);
    assert(shizuku_c6_selftest_record.failed == 2);
    assert(shizuku_c6_selftest_record.first_failure_id == 0x1003);
    assert(shizuku_c6_selftest_record.last_test_id == 0x101Bu);
    std::printf("[TEST]   8. Global counters & record_fail sync assertions passed.\n");

    // 9. 5 つの ladder の代表的アサーション名マッピング検証
    assert(shizuku_c6_selftest_name_to_id("call/6 nested: value") == 0x100E);
    assert(shizuku_c6_selftest_name_to_id("handler_a created child_a") == 0x2007);
    assert(shizuku_c6_selftest_name_to_id("grant: spawn hog") == 0x3005);
    assert(shizuku_c6_selftest_name_to_id("memory: allocate") == 0x4001);
    assert(shizuku_c6_selftest_name_to_id("stream: nothing was lost") == 0x5005);
    assert(shizuku_c6_selftest_name_to_id("unregistered test") == 0xFFFF);
    std::printf("[TEST]   9. Name-to-ID mapping assertions passed.\n");

    std::printf("[TEST] All C6 selftest RAM record unit tests PASSED successfully!\n");
    return 0;
}
