// RP2040 Pico SDK CMake firmware startup.
// Calls shizuku_firmware_entry() strictly from the root thread after kernel/bootstrap setup.
#include <cstdio>
#include <cstdint>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "shizuku/objects/usb_cdc.hpp"

// Flash boundary symbols consumed by rp2040_flash_limit.ld ASSERTs
__asm__(".global __shizuku_firmware_limit\n"
        ".set __shizuku_firmware_limit, (512 * 1024)\n"
        ".global __shizuku_logical_flash_limit\n"
        ".set __shizuku_logical_flash_limit, (2 * 1024 * 1024)\n");

extern "C" {
// Non-weak, canonical C ABI firmware loader entry
bool shizuku_firmware_entry(void);

// Required SDK-free kernel root-thread bootstrap symbol for RP2040.
// Must be provided once kernel init/bootstrap/root context path is established.
// Declared as a strong reference: if the repo does not link the SDK-free kernel bootstrap,
// the build fails at link time with an explicit undefined reference.
[[noreturn]] void shizuku_rp2040_kernel_bootstrap_root_thread(void (*root_thread_fn)(void));

// SDK-free yield
void shizuku_yield(void);
}

// Function called strictly in root-thread context after kernel/bootstrap initialization
static void rp2040_root_thread(void) {
    printf("[SHIZUKU-RP2040] Root thread active. Invoking shizuku_firmware_entry()...\n");
    bool ok = shizuku_firmware_entry();
    if (!ok) {
        printf("[SHIZUKU-RP2040] FATAL: shizuku_firmware_entry() returned false. Halting.\n");
        for (;;) {
            tight_loop_contents();
        }
    }

    printf("[SHIZUKU-RP2040] shizuku_firmware_entry() succeeded. Entering dispatch loop.\n");
    for (;;) {
        shizuku_yield();
    }
}

int main(void) {
    stdio_init_all();
    shizuku::objects::usb_cdc_init();

    printf("\n=== Shizuku RP2040 Pico SDK Firmware ===\n");
    printf("Hardware: RP2040 (Cortex-M0+)\n");

    // Strictly enforce call order:
    // Kernel init/bootstrap must run first and hand off to the root thread.
    // Fails at link time if shizuku_rp2040_kernel_bootstrap_root_thread is not provided.
    shizuku_rp2040_kernel_bootstrap_root_thread(rp2040_root_thread);

    for (;;) {
        tight_loop_contents();
    }
    return 0;
}
