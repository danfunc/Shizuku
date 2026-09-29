/* Add this assertion to the pico-sdk linker script. RP2040 XIP starts at
 * 0x10000000; __flash_binary_end includes the load image, including .data. */
ASSERT((__flash_binary_end - 0x10000000) <= __shizuku_firmware_limit,
       "Shizuku firmware exceeds the configured RP2040 partition")
ASSERT((__flash_binary_end - 0x10000000) <= __shizuku_logical_flash_limit,
       "Shizuku firmware exceeds the configured logical RP2040 flash limit")
