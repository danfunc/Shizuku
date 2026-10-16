#ifndef SHIZUKU_FIRMWARE_LOADER_H
#define SHIZUKU_FIRMWARE_LOADER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uintptr_t id;
    void (*entry)(void);
    uintptr_t parent_id;
    bool privileged;
} shizuku_object_manifest_entry_t;

// Stable SDK-independent loader syscall result
typedef struct {
    uintptr_t error;
    uintptr_t value;
} shizuku_loader_syscall_result_t;

// Syscall boundary seam:
// - In production: defined in firmware_loader_syscall.cpp forwarding to shizuku::ARCH::syscall.
// - In host tests: defined in test_manifest_loader.cpp providing the test mock seam.
shizuku_loader_syscall_result_t shizuku_loader_seam_syscall(
    uintptr_t number, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);

// Common SDK-independent loader: validates manifest, calls CREATE_OBJECT with exact flags,
// verifies returns, enforces root parent authority, and spawns thread for each object.
bool shizuku_firmware_load_and_start(const shizuku_object_manifest_entry_t *manifest, uint32_t count);

// C ABI firmware entry defined in generated manifest TU
bool shizuku_firmware_entry(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // SHIZUKU_FIRMWARE_LOADER_H
