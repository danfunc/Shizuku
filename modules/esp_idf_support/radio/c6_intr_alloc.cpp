// C6 line-selection policy for the shared ESP-IDF interrupt allocator.
#include "trap_api.hpp"

#define SHIZUKU_INTR_CHIP shizuku::chips::esp32_c6
#define SHIZUKU_INTR_ALLOC_NAME shizuku_c6_intr_alloc
#define SHIZUKU_INTR_FREE_NAME shizuku_c6_intr_free
#define SHIZUKU_INTR_SLOT_COUNT 24
#define SHIZUKU_INTR_FIRST_IRQ 1
#define SHIZUKU_INTR_LAST_IRQ 31
#define SHIZUKU_INTR_LINE_COUNT 31
#define SHIZUKU_INTR_RESERVED_IRQ 17
#define SHIZUKU_INTR_MAX_LEVEL 7
#define SHIZUKU_INTR_LINE_AVAILABLE(irq) \
  ((irq) != 0 && (irq) != 1 && (irq) != 3 && (irq) != 4 && (irq) != 6 && \
   (irq) != 7 && (irq) != 17 && (irq) != 31)
#define SHIZUKU_INTR_SOURCE_AVAILABLE(source) ((source) != 57)
#include "intr_alloc_impl.inc"
