#ifndef SHIZUKU_RP2350_BOOT_DIAG_HPP
#define SHIZUKU_RP2350_BOOT_DIAG_HPP
#include <cstdint>
// SDK / board diagnostics, not a kernel API. No register knowledge in kernel.
extern "C" void shizuku_rp2350_boot_stage(uint32_t stage);
extern "C" void shizuku_rp2350_boot_poll();
extern "C" void shizuku_rp2350_boot_recover_if_needed();
extern "C" void shizuku_rp2350_boot_fault(uint32_t exc_return, uintptr_t raw_sp);
#endif
