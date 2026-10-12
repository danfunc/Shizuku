#include "chips/esp32_c5/selftest_record.h"

namespace shizuku {
namespace selftest {
extern uint32_t passed;
extern uint32_t failed;
void call_ladder();
void handler_binding_probe();
void thread_ladder();
void memory_ladder();
void stream_ladder();
void compat_premise_probe();
}
}

extern "C" void shizuku_c5_run_selftests(void) {
    // 0. global counters を明示初期化
    shizuku::selftest::passed = 0;
    shizuku::selftest::failed = 0;
    shizuku_c5_selftest_record_init();

    // 1. call_ladder
    shizuku::selftest::call_ladder();
    shizuku_c5_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c5_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c5_selftest_record_ladder_done(SHIZUKU_C5_LADDER_CALL, 0x101Bu);

    // 2. handler_binding_probe
    shizuku::selftest::handler_binding_probe();
    shizuku_c5_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c5_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c5_selftest_record_ladder_done(SHIZUKU_C5_LADDER_BINDING, 0x2013u);

    // 3. thread_ladder
    shizuku::selftest::thread_ladder();
    shizuku_c5_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c5_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c5_selftest_record_ladder_done(SHIZUKU_C5_LADDER_THREAD, 0x3011u);

    // 4. memory_ladder
    shizuku::selftest::memory_ladder();
    shizuku_c5_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c5_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c5_selftest_record_ladder_done(SHIZUKU_C5_LADDER_MEMORY, 0x4006u);

    // 5. stream_ladder
    shizuku::selftest::stream_ladder();
    shizuku_c5_selftest_record.passed = shizuku::selftest::passed;
    shizuku_c5_selftest_record.failed = shizuku::selftest::failed;
    shizuku_c5_selftest_record_ladder_done(SHIZUKU_C5_LADDER_STREAM, 0x5008u);

    shizuku_c5_selftest_record_finish();

    // 6. FreeRTOS 互換 段1 の前提 probe。★selftest の記録確定 (finish) のあとに、
    //    別カウンタで走らせる。136/0 とは混ぜない。
    shizuku::selftest::compat_premise_probe();
}
