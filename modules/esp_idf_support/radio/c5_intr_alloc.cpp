// C5 line-selection policy for the shared ESP-IDF interrupt allocator.
#include "trap_api.hpp"

#define SHIZUKU_INTR_CHIP shizuku::chips::esp32_c5
#define SHIZUKU_INTR_ALLOC_NAME shizuku_c5_intr_alloc
#define SHIZUKU_INTR_FREE_NAME shizuku_c5_intr_free
#define SHIZUKU_INTR_SLOT_COUNT 14
#define SHIZUKU_INTR_FIRST_IRQ 18
#define SHIZUKU_INTR_LAST_IRQ 31
#define SHIZUKU_INTR_LINE_COUNT 14
#define SHIZUKU_INTR_RESERVED_IRQ UINT32_MAX
#define SHIZUKU_INTR_MAX_LEVEL 3
#define SHIZUKU_INTR_LINE_AVAILABLE(irq) ((irq) != SHIZUKU_INTR_RESERVED_IRQ)
#define SHIZUKU_INTR_SOURCE_AVAILABLE(source) ((source) != 61)
#include "intr_alloc_impl.inc"
