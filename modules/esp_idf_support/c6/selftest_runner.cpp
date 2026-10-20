#include "chips/esp32_c6/selftest_record.h"
#if defined(SHIZUKU_EXP_FASTCALL) && SHIZUKU_EXP_FASTCALL
#include "chips/esp32_c6/fastcall.h"
#endif

namespace shizuku {
namespace selftest {
extern uint32_t passed;
extern uint32_t failed;
void call_ladder();
void handler_binding_probe();
void thread_ladder();
void memory_ladder();
void stream_ladder();
void call_cost();
void c6_stage1_safety();
}
}

extern "C" void shizuku_c6_run_selftests(void) {
    // 0. global counters を明示初期化
    shizuku::selftest::passed = 0;
    shizuku::selftest::failed = 0;
    shizuku_c6_selftest_record_init();

#if defined(SHIZUKU_EXP_FASTCALL) && SHIZUKU_EXP_FASTCALL
    // Fastcall candidate selftest & 128-round bench executed before legacy call_ladder.
    // Results are recorded in shizuku_c6_fastcall_record in RAM and diag ring even if
    // subsequent legacy call_ladder encounters E005.
    shizuku_c6_run_fastcall_selftests();
#endif

    // 1. call_ladder
    shizuku::selftest::call_ladder();
    shizuku_c6_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c6_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c6_selftest_record_ladder_done(SHIZUKU_C6_LADDER_CALL, 0x101Bu);

    // 2. handler_binding_probe
    shizuku::selftest::handler_binding_probe();
    shizuku_c6_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c6_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c6_selftest_record_ladder_done(SHIZUKU_C6_LADDER_BINDING, 0x2013u);

    // 3. thread_ladder
    shizuku::selftest::thread_ladder();
    shizuku_c6_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c6_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c6_selftest_record_ladder_done(SHIZUKU_C6_LADDER_THREAD, 0x3011u);

    // 4. memory_ladder
    shizuku::selftest::memory_ladder();
    shizuku_c6_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c6_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c6_selftest_record_ladder_done(SHIZUKU_C6_LADDER_MEMORY, 0x4006u);

    // 5. stream_ladder
    shizuku::selftest::stream_ladder();
    shizuku_c6_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c6_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c6_selftest_record_ladder_done(SHIZUKU_C6_LADDER_STREAM, 0x5008u);

    // 6. call_cost (段0: 既存 call_cost の有効化)
    shizuku::selftest::call_cost();
    shizuku_c6_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c6_selftest_record.failed = shizuku::selftest::failed;

    // 7. c6_stage1_safety (段1: U-mode + PMP + 0x802 診断)
    shizuku::selftest::c6_stage1_safety();
    shizuku_c6_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c6_selftest_record.failed = shizuku::selftest::failed;

    shizuku_c6_selftest_record_finish();
}
