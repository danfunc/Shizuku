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

#endif

