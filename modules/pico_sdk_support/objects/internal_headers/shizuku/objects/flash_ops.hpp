#pragma once

#include <cstdint>

namespace shizuku::objects::flash_ops {

// These operations serialize flash access, park the other core, disable local
// interrupts and temporarily remove kernel memory protection. `data` must be
// in SRAM and remain valid until program() returns.
struct timing {
  uint32_t elapsed_us = 0;
};

int erase(uint32_t offset, uint32_t bytes, timing *measurement = nullptr);
int program(uint32_t offset, const uint8_t *data, uint32_t bytes,
            timing *measurement = nullptr);

} // namespace shizuku::objects::flash_ops
