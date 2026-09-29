/* Keep RP2350's current 1 MiB firmware partition. */
ASSERT((__flash_binary_end - 0x10000000) <= __shizuku_firmware_limit,
       "Shizuku firmware exceeds the configured RP2350 partition")
ASSERT((__flash_binary_end - 0x10000000) <= __shizuku_logical_flash_limit,
       "Shizuku firmware exceeds the configured logical RP2350 flash limit")
