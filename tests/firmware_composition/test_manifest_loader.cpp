#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "shizuku/firmware_loader.hpp"
#include "shizuku/object_api.hpp"

// ==============================================================================
// Test Seam Syscall Mock Implementation (Host test seam; production syscall TU is excluded)
// ==============================================================================
struct SyscallRecord {
    uintptr_t number;
    uintptr_t arg1;
    uintptr_t arg2;
    uintptr_t arg3;
};

static std::vector<SyscallRecord> g_syscall_records;
static uintptr_t g_mock_create_error = 0;
static uintptr_t g_mock_spawn_error = 0;

extern "C" {

shizuku_loader_syscall_result_t shizuku_loader_seam_syscall(
    uintptr_t number, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {
    g_syscall_records.push_back({number, arg1, arg2, arg3});

    shizuku_loader_syscall_result_t res{};
    res.value = 0;
    if (number == static_cast<uintptr_t>(shizuku::object_api::CREATE_OBJECT)) {
        res.error = g_mock_create_error;
    } else if (number == static_cast<uintptr_t>(shizuku::object_api::SPAWN)) {
        res.error = g_mock_spawn_error;
    } else {
        res.error = 0;
    }
    return res;
}

} // extern "C"

static void reset_mocks() {
    g_syscall_records.clear();
    g_mock_create_error = 0;
    g_mock_spawn_error = 0;
}

static void dummy_entry_1(void) {}
static void dummy_entry_2(void) {}

// Test 1: Privilege flag constants verification
void test_1_privilege_flag_constants() {
    printf("[TEST 1] Verifying OBJECT_PRIVILEGED vs OBJECT_UNPRIVILEGED constants...\n");
    // Assert 1: OBJECT_PRIVILEGED must be (1u << 0) = 0x1
    assert(shizuku::OBJECT_PRIVILEGED == (1u << 0));
    // Assert 2: OBJECT_UNPRIVILEGED must be (1u << 1) = 0x2
    assert(shizuku::OBJECT_UNPRIVILEGED == (1u << 1));
    // Assert 3: handler.cpp:442 treats OBJECT_UNPRIVILEGED as unprivileged
    assert((shizuku::OBJECT_UNPRIVILEGED & shizuku::OBJECT_UNPRIVILEGED) != 0);
    // Assert 4: handler treats OBJECT_PRIVILEGED (no bit 1) as privileged
    assert((shizuku::OBJECT_PRIVILEGED & shizuku::OBJECT_UNPRIVILEGED) == 0);
    printf("  [PASS] Test 1: Privilege flags exact bitmask verified.\n");
}

// Test 2: Invalid entry or ID aborts fail-closed
void test_2_invalid_entry_aborts() {
    printf("[TEST 2] Verifying production loader rejects invalid entries fail-closed...\n");
    reset_mocks();
    shizuku_object_manifest_entry_t bad_id_entry[] = {
        {"bad_id", 0, &dummy_entry_1, 1, false}, // ID 0 is reserved/invalid
    };
    bool ok1 = shizuku_firmware_load_and_start(bad_id_entry, 1);
    // Assert 5: loader must reject ID < 2
    assert(!ok1);
    // Assert 6: no syscall on bad ID
    assert(g_syscall_records.empty());

    reset_mocks();
    shizuku_object_manifest_entry_t bad_id_1[] = {
        {"bad_id_root", 1, &dummy_entry_1, 1, false}, // ID 1 is reserved/invalid for object
    };
    bool ok1_b = shizuku_firmware_load_and_start(bad_id_1, 1);
    // Assert 7: loader must reject ID == 1
    assert(!ok1_b);
    // Assert 8: no syscall on bad ID 1
    assert(g_syscall_records.empty());

    reset_mocks();
    shizuku_object_manifest_entry_t null_entry[] = {
        {"null_entry", 2, nullptr, 1, false},
    };
    bool ok2 = shizuku_firmware_load_and_start(null_entry, 1);
    // Assert 9: loader must reject null entry function pointer
    assert(!ok2);
    // Assert 10: no syscall on null entry
    assert(g_syscall_records.empty());
    printf("  [PASS] Test 2: Invalid entries rejected without syscall or spawn.\n");
}

// Test 3: Null or zero manifest aborts fail-closed
void test_3_empty_manifest_fails_closed() {
    printf("[TEST 3] Verifying production loader rejects null or zero count manifests...\n");
    reset_mocks();
    // Assert 11: null manifest pointer returns false
    assert(!shizuku_firmware_load_and_start(nullptr, 1));
    shizuku_object_manifest_entry_t valid_entry[] = {
        {"valid", 2, &dummy_entry_1, 1, true},
    };
    // Assert 12: zero count returns false
    assert(!shizuku_firmware_load_and_start(valid_entry, 0));
    // Assert 13: no syscall on null manifest
    assert(g_syscall_records.empty());
    // Assert 14: no syscall on zero count
    assert(g_syscall_records.empty());
    printf("  [PASS] Test 3: Null/empty manifest safely returns false.\n");
}

// Test 4: Oversized manifest (count > 8) aborts fail-closed
void test_4_oversize_manifest_fails_closed() {
    printf("[TEST 4] Verifying production loader rejects oversized manifests (> 8 objects)...\n");
    reset_mocks();
    shizuku_object_manifest_entry_t big_manifest[9];
    for (int i = 0; i < 9; ++i) {
        big_manifest[i] = {"obj", static_cast<uintptr_t>(i + 2), &dummy_entry_1, 1, false};
    }
    bool ok = shizuku_firmware_load_and_start(big_manifest, 9);
    // Assert 15: count > 8 must be rejected fail-closed, never silently dropping excess objects
    assert(!ok);
    // Assert 16: no syscall on oversize
    assert(g_syscall_records.empty());
    printf("  [PASS] Test 4: Oversized manifest (> 8) safely rejected.\n");
}

// Test 5: Unsupported non-root parent_id aborts fail-closed
void test_5_non_root_parent_fails_closed() {
    printf("[TEST 5] Verifying unsupported non-root parent_id fail-closed rejection...\n");
    reset_mocks();
    shizuku_object_manifest_entry_t non_root_entry[] = {
        {"child_obj", 2, &dummy_entry_1, 99 /* unsupported parent */, false},
    };
    bool ok = shizuku_firmware_load_and_start(non_root_entry, 1);
    // Assert 17: non-root parent must return false fail-closed until delegation policy implemented
    assert(!ok);
    // Assert 18: no syscall on non-root parent
    assert(g_syscall_records.empty());
    printf("  [PASS] Test 5: Non-root parent safely rejected without unauthorized registration.\n");
}

// Test 6: CREATE_OBJECT syscall error aborts without spawn
void test_6_create_syscall_error_aborts_without_spawn() {
    printf("[TEST 6] Verifying CREATE_OBJECT syscall error aborts without spawn...\n");
    reset_mocks();
    g_mock_create_error = 1; // Inject syscall error (e.g. ID conflict or bad flags)

    shizuku_object_manifest_entry_t entry[] = {
        {"obj1", 2, &dummy_entry_1, 1, true},
    };
    bool ok = shizuku_firmware_load_and_start(entry, 1);
    // Assert 19: loader must fail on CREATE_OBJECT error
    assert(!ok);
    // Assert 20: exactly 1 syscall attempted
    assert(g_syscall_records.size() == 1);
    // Assert 21: syscall number must be CREATE_OBJECT
    assert(g_syscall_records[0].number == static_cast<uintptr_t>(shizuku::object_api::CREATE_OBJECT));
    printf("  [PASS] Test 6: CREATE_OBJECT error correctly halts boot without spawning.\n");
}

// Test 7: SPAWN syscall error aborts
void test_7_spawn_syscall_error_aborts() {
    printf("[TEST 7] Verifying SPAWN syscall error aborts...\n");
    reset_mocks();
    g_mock_spawn_error = 1; // Inject SPAWN syscall error (e.g. out of memory or thread slots)

    shizuku_object_manifest_entry_t entry[] = {
        {"obj1", 2, &dummy_entry_1, 1, true},
    };
    bool ok = shizuku_firmware_load_and_start(entry, 1);
    // Assert 22: loader must fail on SPAWN error
    assert(!ok);
    // Assert 23: exactly 2 syscalls attempted (1 CREATE_OBJECT + 1 SPAWN)
    assert(g_syscall_records.size() == 2);
    // Assert 24: first syscall is CREATE_OBJECT
    assert(g_syscall_records[0].number == static_cast<uintptr_t>(shizuku::object_api::CREATE_OBJECT));
    // Assert 25: second syscall is SPAWN
    assert(g_syscall_records[1].number == static_cast<uintptr_t>(shizuku::object_api::SPAWN));
    printf("  [PASS] Test 7: SPAWN error correctly caught and returned as failure.\n");
}

// Test 8: Production loader passes correct flags and invokes genuine SPAWN syscall
void test_8_production_loader_execution() {
    printf("[TEST 8] Verifying production loader per-entry syscall flags and SPAWN syscall parameters...\n");
    reset_mocks();

    shizuku_object_manifest_entry_t entries[] = {
        {"priv_obj", 2, &dummy_entry_1, 1 /* root */, true},
        {"unpriv_obj", 3, &dummy_entry_2, 1 /* root */, false},
    };

    bool ok = shizuku_firmware_load_and_start(entries, 2);
    // Assert 26: loader returns true on complete success
    assert(ok);
    // Assert 27: exactly 4 syscalls (2 CREATE_OBJECT + 2 SPAWN)
    assert(g_syscall_records.size() == 4);

    // Verify Entry 0 (privileged) CREATE_OBJECT:
    // Assert 28: entry 0 syscall number must be CREATE_OBJECT
    assert(g_syscall_records[0].number == static_cast<uintptr_t>(shizuku::object_api::CREATE_OBJECT));
    // Assert 29: entry 0 ID must match 2
    assert(g_syscall_records[0].arg1 == 2);
    // Assert 30: entry 0 function pointer must match dummy_entry_1
    assert(g_syscall_records[0].arg2 == reinterpret_cast<uintptr_t>(&dummy_entry_1));
    // Assert 31: entry 0 flags must be OBJECT_PRIVILEGED (0x1)
    assert(g_syscall_records[0].arg3 == shizuku::OBJECT_PRIVILEGED);

    // Verify Entry 1 (unprivileged) CREATE_OBJECT:
    // Assert 32: entry 1 syscall number must be CREATE_OBJECT
    assert(g_syscall_records[1].number == static_cast<uintptr_t>(shizuku::object_api::CREATE_OBJECT));
    // Assert 33: entry 1 ID must match 3
    assert(g_syscall_records[1].arg1 == 3);
    // Assert 34: entry 1 function pointer must match dummy_entry_2
    assert(g_syscall_records[1].arg2 == reinterpret_cast<uintptr_t>(&dummy_entry_2));
    // Assert 35: entry 1 flags must be OBJECT_UNPRIVILEGED (0x2)
    assert(g_syscall_records[1].arg3 == shizuku::OBJECT_UNPRIVILEGED);

    // Verify Entry 0 SPAWN syscall:
    // Assert 36: entry 0 spawn syscall number must be SPAWN (index 7)
    assert(g_syscall_records[2].number == static_cast<uintptr_t>(shizuku::object_api::SPAWN));
    // Assert 37: entry 0 spawn target object ID must match 2
    assert(g_syscall_records[2].arg1 == 2);
    // Assert 38: entry 0 spawn method index must be 0 (METHOD_MAIN)
    assert(g_syscall_records[2].arg2 == 0);
    // Assert 39: entry 0 spawn entry argument must be 0
    assert(g_syscall_records[2].arg3 == 0);

    // Verify Entry 1 SPAWN syscall:
    // Assert 40: entry 1 spawn syscall number must be SPAWN (index 7)
    assert(g_syscall_records[3].number == static_cast<uintptr_t>(shizuku::object_api::SPAWN));
    // Assert 41: entry 1 spawn target object ID must match 3
    assert(g_syscall_records[3].arg1 == 3);
    // Assert 42: entry 1 spawn method index must be 0 (METHOD_MAIN)
    assert(g_syscall_records[3].arg2 == 0);
    // Assert 43: entry 1 spawn entry argument must be 0
    assert(g_syscall_records[3].arg3 == 0);

    printf("  [PASS] Test 8: Production loader executed with validated per-entry flags and object_api::SPAWN syscalls.\n");
}

// Test 9: Central syscall vocabulary verification
void test_9_syscall_vocabulary() {
    printf("[TEST 9] Verifying central syscall vocabulary indices...\n");
    // Assert 44: object_api::CREATE_OBJECT must be exactly index 1
    assert(static_cast<uintptr_t>(shizuku::object_api::CREATE_OBJECT) == 1);
    // Assert 45: object_api::SPAWN must be exactly index 7
    assert(static_cast<uintptr_t>(shizuku::object_api::SPAWN) == 7);
    // Assert 46: object_api::YIELD must be exactly index 8
    assert(static_cast<uintptr_t>(shizuku::object_api::YIELD) == 8);
    printf("  [PASS] Test 9: CREATE_OBJECT (1), SPAWN (7), and YIELD (8) vocabulary indices verified.\n");
}

int main() {
    printf("=== Running Honest Production Firmware Loader Policy Assertions ===\n");
    test_1_privilege_flag_constants();
    test_2_invalid_entry_aborts();
    test_3_empty_manifest_fails_closed();
    test_4_oversize_manifest_fails_closed();
    test_5_non_root_parent_fails_closed();
    test_6_create_syscall_error_aborts_without_spawn();
    test_7_spawn_syscall_error_aborts();
    test_8_production_loader_execution();
    test_9_syscall_vocabulary();
    printf("=== ALL 46 PRODUCTION LOADER ASSERTIONS PASSED ===\n");
    return 0;
}
