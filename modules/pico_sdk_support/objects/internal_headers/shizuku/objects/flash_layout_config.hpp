#ifndef SHIZUKU_OBJECTS_FLASH_LAYOUT_CONFIG_HPP
#define SHIZUKU_OBJECTS_FLASH_LAYOUT_CONFIG_HPP

// Generated capacity value comes from //configs:flash_capacity_bytes. Zero
// means use the physical size selected by the SDK board configuration.
#include "shizuku/objects/generated_flash_layout_config.hpp"

#if SHIZUKU_FLASH_CAPACITY_BYTES == 0
#define SHIZUKU_LOGICAL_FLASH_BYTES PICO_FLASH_SIZE_BYTES
#else
#define SHIZUKU_LOGICAL_FLASH_BYTES SHIZUKU_FLASH_CAPACITY_BYTES
#endif

// Single source for firmware/staging ceilings; the linker consumes the
// absolute symbols emitted by flash_layout_limit.cpp.
#if defined(SHIZUKU_RP2040)
#define SHIZUKU_FIRMWARE_BYTES (512 * 1024)
#define SHIZUKU_STAGING_BYTES SHIZUKU_FIRMWARE_BYTES
#else
#define SHIZUKU_FIRMWARE_BYTES (1024 * 1024)
#define SHIZUKU_FS_BYTES (1024 * 1024)
#endif

#endif
