#include "hardware/flash.h"
#include "shizuku/objects/flash_layout_config.hpp"

#define SHIZUKU_STRINGIFY_INNER(value) #value
#define SHIZUKU_STRINGIFY(value) SHIZUKU_STRINGIFY_INNER(value)

// Absolute symbol shared with the firmware linker ASSERT scripts.
__asm__(".global __shizuku_firmware_limit\n"
        ".set __shizuku_firmware_limit, "
        SHIZUKU_STRINGIFY(SHIZUKU_FIRMWARE_BYTES));
__asm__(".global __shizuku_logical_flash_limit\n"
        ".set __shizuku_logical_flash_limit, "
        SHIZUKU_STRINGIFY(SHIZUKU_LOGICAL_FLASH_BYTES));
