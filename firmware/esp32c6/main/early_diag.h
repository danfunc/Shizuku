#ifndef SHIZUKU_ESP32C6_EARLY_DIAG_H
#define SHIZUKU_ESP32C6_EARLY_DIAG_H

#include <stdint.h>

#define SHIZUKU_C6_EARLY_DIAG_MAGIC 0x53485A44u
#define SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC 0x5348454Eu
#define SHIZUKU_C6_EARLY_DIAG_VERSION 2u
#define SHIZUKU_C6_DIAG_UNKNOWN 0xFFFFFFFFu
#define SHIZUKU_C6_TG_APB_CLOCK_ENABLED (1u << 0)
#define SHIZUKU_C6_TG_RESET_RELEASED (1u << 1)
#define SHIZUKU_C6_TG_WDT_CLOCK_ENABLED (1u << 2)
#define SHIZUKU_C6_TG_REGS_READ (1u << 3)
#define SHIZUKU_C6_EARLY_DIAG_RECORD_COUNT 4u

#define SHIZUKU_C6_EARLY_WRITER_PENDING 0xFFFFFFFFu
#define SHIZUKU_C6_EARLY_WRITER_OK 1u
#define SHIZUKU_C6_EARLY_WRITER_FAILED 2u

typedef struct {
    uint32_t sequence;
    uint32_t raw_reset_reason;
    uint32_t rwdt_config0;
    uint32_t rwdt_config1;
    uint32_t rwdt_config2;
    uint32_t rwdt_config3;
    uint32_t rwdt_config4;
    uint32_t mwdt0_config0;
    uint32_t mwdt0_config1;
    uint32_t mwdt0_config2;
    uint32_t mwdt0_config3;
    uint32_t mwdt0_config4;
    uint32_t mwdt1_config0;
    uint32_t mwdt1_config1;
    uint32_t mwdt1_config2;
    uint32_t mwdt1_config3;
    uint32_t mwdt1_config4;
    uint32_t super_wdt_config;
    uint32_t writer_status;
    uint32_t checksum;
    uint32_t timg0_status;
    uint32_t timg1_status;
    uint32_t rwdt_config0_after_stop;
} shizuku_c6_early_diag_record_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    shizuku_c6_early_diag_record_t records[SHIZUKU_C6_EARLY_DIAG_RECORD_COUNT];
} shizuku_c6_early_diag_log_t;

extern volatile shizuku_c6_early_diag_log_t shizuku_c6_early_diag_log;

void shizuku_c6_early_diag_begin(void);
void shizuku_c6_early_diag_writer_result(uint32_t result);

#endif
