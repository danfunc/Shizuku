#include "ports/esp32/c6/selftest_record.h"
#include <cstring>

extern "C" {
volatile shizuku_c6_selftest_record_t shizuku_c6_selftest_record = {
    SHIZUKU_C6_SELFTEST_MAGIC,
    SHIZUKU_C6_SELFTEST_STATUS_RUNNING,
    0, 0, 0, 0, 0, 0
};

struct name_id_entry {
    const char *name;
    uint32_t id;
};

static const name_id_entry kMapping[] = {
    // 0x1000 番台: call_ladder
    {"create object", 0x1001},
    {"call/1: error", 0x1002},
    {"call/1: value", 0x1003},
    {"call/1: callee identity", 0x1004},
    {"call/1: caller identity", 0x1005},
    {"call/1: depth restored", 0x1006},
    {"call/1 (fp): value", 0x1007},
    {"call/1 (fp): float preserved", 0x1008},
    {"return_stub: replace leaf with dirty scratch", 0x1009},
    {"return_stub: dirty scratch return error", 0x100A},
    {"return_stub: dirty scratch return value", 0x100B},
    {"return_stub: depth restored", 0x100C},
    {"return_stub: restore leaf", 0x100D},
    {"call/6 nested: value", 0x100E},
    {"call/6 nested: depth restored", 0x100F},
    {"nested: levels entered", 0x1010},
    {"nested: identity at every level", 0x1011},
    {"nested: depth grows by 2 per level", 0x1012},
    {"unknown api: rejected", 0x1013},
    {"absent object: rejected", 0x1014},
    {"stack exhaustion: returned NO_STACK", 0x1015},
    {"stack exhaustion: depth restored", 0x1016},
    {"name: the object named itself", 0x1017},
    {"name: it can be read back", 0x1018},
    {"name: renaming is refused", 0x1019},
    {"name: a colliding id is refused, loudly", 0x101A},
    {"name: an absent object has no name", 0x101B},

    // 0x2000 番台: handler_binding_probe
    {"create handler_a with INTERNAL_FLAG_HANDLER", 0x2001},
    {"create handler_b with INTERNAL_FLAG_HANDLER", 0x2002},
    {"handler_a parent is root kobj", 0x2003},
    {"handler_b parent is root kobj", 0x2004},
    {"create privileged non-root object", 0x2005},
    {"non-root privileged cannot specify INTERNAL_FLAG_HANDLER", 0x2006},
    {"handler_a created child_a", 0x2007},
    {"child_a parent automatically derived as handler_a", 0x2008},
    {"handler_b created child_b", 0x2009},
    {"child_b parent automatically derived as handler_b", 0x200A},
    {"child_a SVC routed to parent_a (+100) and exit forwarded", 0x200B},
    {"child_b SVC routed to parent_b (+200) and exit forwarded", 0x200C},
    {"handler_a own SVC routed to root kobj", 0x200D},
    {"handler_a replaced child_a method with primitive probe", 0x200E},
    {"child_a parent still handler_a after handler-initiated replace", 0x200F},
    {"child cannot invoke kernel primitive directly (routed to handler as SVC)", 0x2010},
    {"handler_a restored child_a to compute", 0x2011},
    {"nested CALL/RETURN across handlers", 0x2012},
    {"depth fully restored after nested calls", 0x2013},

    // 0x3000 番台: thread_ladder
    {"spawn: error", 0x3001},
    {"spawn: thread id", 0x3002},
    {"yield: worker finished", 0x3003},
    {"yield: worker rounds", 0x3004},
    {"grant: spawn hog", 0x3005},
    {"grant: returned", 0x3006},
    {"grant: reclaimed by budget", 0x3007},
    {"grant: within the lent budget", 0x3008},
    {"grant: borrower ran", 0x3009},
    {"sleep: spawn sleeper", 0x300A},
    {"sleep: the borrower gave the grant back", 0x300B},
    {"sleep: the lender resumed at once", 0x300C},
    {"sleep: the sleeper woke up later", 0x300D},
    {"overshoot: spawn hog", 0x300E},
    {"overshoot: every grant was reclaimed", 0x300F},
    {"overshoot: bounded", 0x3010},
    {"grant: a budget too small to honour is refused", 0x3011},
    {"yield borrower: spawn", 0x3012},
    {"yield borrower: returned to lender", 0x3013},
    {"yield borrower: ran before yield", 0x3014},
    {"yield borrower: returned immediately (not expired)", 0x3015},
    {"yield borrower: resumed after yield", 0x3016},
    {"nested A->B->C: spawn B", 0x3017},
    {"nested A->B->C: B returned to A", 0x3018},
    {"nested A->B->C: C actually ran", 0x3019},
    {"nested A->B->C: C yielded back to B", 0x301A},
    {"nested margin: spawn B", 0x301B},
    {"nested margin: C reclaimed by budget", 0x301C},
    {"nested margin: C elapsed bounded by B lend (~2ms)", 0x301D},
    {"nested margin: B returned to A", 0x301E},
    {"nested margin: total elapsed within B budget", 0x301F},
    {"nested clamp: spawn B clamp", 0x3020},
    {"nested clamp: B left no mark after budget exhaustion", 0x3021},
    {"nested clamp: B unwound to A with budget expired", 0x3022},
    {"nested clamp: total elapsed bounded by B budget (~2ms, not 50ms)", 0x3023},
    {"ancestor grant: spawn B", 0x3024},
    {"ancestor grant: B returned to A via ancestor unwind", 0x3025},
    {"ancestor grant: B actually ran", 0x3026},
    {"ancestor grant: returned immediately", 0x3027},
    {"ancestor grant: grant depth not increased (grant_active false)", 0x3028},
    {"ancestor grant: B resumed after ancestor unwind", 0x3029},
    {"multi ancestor: spawn B", 0x302A},
    {"multi ancestor: returned to A via multi-unwind", 0x302B},
    {"multi ancestor: C ran before unwind", 0x302C},
    {"multi ancestor: B post-grant logic not executed yet", 0x302D},
    {"multi ancestor: C post-grant logic not executed yet", 0x302E},
    {"multi ancestor: depth unwound to 0", 0x302F},
    {"multi ancestor: B returned after resume sequence", 0x3030},
    {"multi ancestor: B resumed from saved GRANT(C)", 0x3031},
    {"multi ancestor: C resumed from saved GRANT(A)", 0x3032},
    {"multi ancestor: full sequence completed", 0x3033},
    {"mutex: owner A acquires lock", 0x3034},
    {"mutex: spawn contender B", 0x3035},
    {"mutex: contender returned to owner A via GRANT(owner)", 0x3036},
    {"mutex: contender initial CAS failed while held by A", 0x3037},
    {"mutex: lock still held by owner after contender returned", 0x3038},
    {"mutex: contender post-grant logic not executed before unlock", 0x3039},
    {"mutex: owner A releases lock", 0x303A},
    {"mutex: contender returned after lock acquisition", 0x303B},
    {"mutex: contender re-check CAS succeeded after owner unlock", 0x303C},
    {"mutex: final mutex is unlocked", 0x303D},

    // 0x4000 番台: memory_ladder
    {"memory: allocate", 0x4001},
    {"memory: the borrower is recorded", 0x4002},
    {"memory: a bad handle is refused", 0x4003},
    {"memory: release", 0x4004},
    {"memory: holes were actually made", 0x4005},
    {"memory: the cost does not follow the number of holes", 0x4006},

    // 0x5000 番台: stream_ladder
    {"stream: created", 0x5001},
    {"stream: opened by id", 0x5002},
    {"stream: the producer sent every record", 0x5003},
    {"stream: the consumer received every record", 0x5004},
    {"stream: nothing was lost", 0x5005},
    {"stream: nothing arrived out of order", 0x5006},
    {"stream: back-pressure actually happened", 0x5007},
    {"stream: the second producer is refused", 0x5008},
};

static const size_t kMappingCount = sizeof(kMapping) / sizeof(kMapping[0]);

uint32_t shizuku_c6_selftest_name_to_id(const char *name) {
    if (!name) return 0xFFFFu;
    for (size_t i = 0; i < kMappingCount; ++i) {
        if (std::strcmp(name, kMapping[i].name) == 0) {
            return kMapping[i].id;
        }
    }
    return 0xFFFFu;
}

void shizuku_c6_selftest_record_init(void) {
    shizuku_c6_selftest_record.magic = SHIZUKU_C6_SELFTEST_MAGIC;
    shizuku_c6_selftest_record.completed = SHIZUKU_C6_SELFTEST_STATUS_RUNNING;
    shizuku_c6_selftest_record.passed = 0;
    shizuku_c6_selftest_record.failed = 0;
    shizuku_c6_selftest_record.first_failure_id = 0;
    shizuku_c6_selftest_record.ladders_mask = 0;
    shizuku_c6_selftest_record.last_test_id = 0;
    shizuku_c6_selftest_record.reserved = 0;
}

void shizuku_c6_selftest_record_assert(const char *name, bool ok, unsigned long got, unsigned long want) {
    (void)got;
    (void)want;
    const uint32_t id = shizuku_c6_selftest_name_to_id(name);
    shizuku_c6_selftest_record.last_test_id = id;
    if (id == 0xFFFFu) {
        // 未登録のテスト名は成功とみなさない (失敗として記録)
        shizuku_c6_selftest_record.failed = shizuku_c6_selftest_record.failed + 1;
        if (shizuku_c6_selftest_record.first_failure_id == 0) {
            shizuku_c6_selftest_record.first_failure_id = 0xFFFFu;
        }
    } else if (ok) {
        shizuku_c6_selftest_record.passed = shizuku_c6_selftest_record.passed + 1;
    } else {
        shizuku_c6_selftest_record.failed = shizuku_c6_selftest_record.failed + 1;
        if (shizuku_c6_selftest_record.first_failure_id == 0) {
            shizuku_c6_selftest_record.first_failure_id = id;
        }
    }
}

void shizuku_c6_selftest_record_ladder_done(uint32_t ladder_bit, uint32_t last_ladder_assert_id) {
    shizuku_c6_selftest_record.ladders_mask |= ladder_bit;
    shizuku_c6_selftest_record.last_test_id = last_ladder_assert_id;
}

void shizuku_c6_selftest_record_finish(void) {
    // 全 5 ladder が完了した場合のみ completed を PASSED (1) または FAILED (2) にする
    if ((shizuku_c6_selftest_record.ladders_mask & SHIZUKU_C6_LADDER_ALL_MASK) == SHIZUKU_C6_LADDER_ALL_MASK) {
        if (shizuku_c6_selftest_record.failed == 0) {
            shizuku_c6_selftest_record.completed = SHIZUKU_C6_SELFTEST_STATUS_PASSED;
        } else {
            shizuku_c6_selftest_record.completed = SHIZUKU_C6_SELFTEST_STATUS_FAILED;
        }
    } else {
        // 未完の場合: RUNNING (0) のまま維持 (FAILED にしない)
        shizuku_c6_selftest_record.completed = SHIZUKU_C6_SELFTEST_STATUS_RUNNING;
    }
}

} // extern "C"

// source/selftest/selftest.hpp で宣言された record_fail の C6 用実装
namespace shizuku {
namespace selftest {

extern uint32_t passed;
extern uint32_t failed;

void record_fail(const char *name, unsigned long got, unsigned long want) {
    (void)got;
    (void)want;
    const uint32_t id = shizuku_c6_selftest_name_to_id(name);
    shizuku_c6_selftest_record.last_test_id = id;
    if (id == 0xFFFFu) {
        if (shizuku_c6_selftest_record.first_failure_id == 0) {
            shizuku_c6_selftest_record.first_failure_id = 0xFFFFu;
        }
    } else {
        if (shizuku_c6_selftest_record.first_failure_id == 0) {
            shizuku_c6_selftest_record.first_failure_id = id;
        }
    }
    // check() 内で ++failed された最新の失敗数を RAM record へ即座に反映
    shizuku_c6_selftest_record.failed = failed;
}

} // namespace selftest
} // namespace shizuku
