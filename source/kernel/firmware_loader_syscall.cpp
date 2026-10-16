#include "shizuku/firmware_loader.hpp"
#include "shizuku/config.hpp"

// Production syscall boundary translation unit: connects loader seam to genuine ARCH::syscall.
// Included strictly in production firmware archives (e.g. shizuku_kernel_c6_core).
extern "C" shizuku_loader_syscall_result_t shizuku_loader_seam_syscall(
    uintptr_t number, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {
    auto res = shizuku::ARCH::syscall(number, arg1, arg2, arg3);
    return {res.error, res.value};
}
