#include "early_diag.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "hal/wdt_hal.h"
#include "soc/lp_wdt_struct.h"
#include "soc/pcr_reg.h"
#include "soc/pcr_struct.h"
#include "soc/soc_caps.h"
#include "soc/timer_group_struct.h"

_Static_assert(sizeof(shizuku_c6_early_diag_record_t) == 92, "early diagnostic record layout changed");
_Static_assert(offsetof(shizuku_c6_early_diag_log_t, magic) == 0, "early diagnostic magic offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_log_t, version) == 4, "early diagnostic version offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_log_t, records) == 8, "early diagnostic log header changed");
_Static_assert(sizeof(shizuku_c6_early_diag_log_t) == 376, "early diagnostic log size changed");
_Static_assert(offsetof(shizuku_c6_early_diag_record_t, raw_reset_reason) == 4, "reset reason offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_record_t, rwdt_config0) == 8, "RWDT offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_record_t, mwdt0_config0) == 28, "MWDT0 offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_record_t, mwdt1_config0) == 48, "MWDT1 offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_record_t, super_wdt_config) == 68, "Super WDT offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_record_t, writer_status) == 72, "writer status offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_record_t, checksum) == 76, "record checksum offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_record_t, timg0_status) == 80, "TIMG0 status offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_record_t, timg1_status) == 84, "TIMG1 status offset changed");
_Static_assert(offsetof(shizuku_c6_early_diag_record_t, rwdt_config0_after_stop) == 88, "post-stop RWDT offset changed");

// Internal DRAM .noinit is NOLOAD and outside the startup BSS clear range.
__NOINIT_ATTR volatile shizuku_c6_early_diag_log_t shizuku_c6_early_diag_log;
__NOINIT_ATTR volatile uint32_t shizuku_c6_early_diag_progress;

static uint32_t s_active_slot;
static bool s_active_record_valid;

static uint32_t record_checksum(const volatile shizuku_c6_early_diag_record_t *record)
{
    const uint32_t checksum = SHIZUKU_C6_EARLY_DIAG_MAGIC ^ SHIZUKU_C6_EARLY_DIAG_VERSION ^ 0xA71D39C5u ^
           record->sequence ^ record->raw_reset_reason ^
           record->rwdt_config0 ^ record->rwdt_config1 ^ record->rwdt_config2 ^ record->rwdt_config3 ^ record->rwdt_config4 ^
           record->mwdt0_config0 ^ record->mwdt0_config1 ^ record->mwdt0_config2 ^ record->mwdt0_config3 ^ record->mwdt0_config4 ^
           record->mwdt1_config0 ^ record->mwdt1_config1 ^ record->mwdt1_config2 ^ record->mwdt1_config3 ^ record->mwdt1_config4 ^
           record->super_wdt_config ^ record->writer_status ^ record->timg0_status ^ record->timg1_status ^
           record->rwdt_config0_after_stop;
    return checksum == 0 ? UINT32_MAX : checksum;
}

static bool record_is_valid(const volatile shizuku_c6_early_diag_record_t *record)
{
    return record->checksum != 0 && record->checksum == record_checksum(record);
}

static void initialize_log(void)
{
    shizuku_c6_early_diag_log.magic = 0;
    shizuku_c6_early_diag_log.version = 0;
    for (unsigned i = 0; i < SHIZUKU_C6_EARLY_DIAG_RECORD_COUNT; ++i) {
        volatile shizuku_c6_early_diag_record_t *record = &shizuku_c6_early_diag_log.records[i];
        record->sequence = 0;
        record->raw_reset_reason = 0;
        record->rwdt_config0 = 0;
        record->rwdt_config1 = 0;
        record->rwdt_config2 = 0;
        record->rwdt_config3 = 0;
        record->rwdt_config4 = 0;
        record->mwdt0_config0 = 0;
        record->mwdt0_config1 = 0;
        record->mwdt0_config2 = 0;
        record->mwdt0_config3 = 0;
        record->mwdt0_config4 = 0;
        record->mwdt1_config0 = 0;
        record->mwdt1_config1 = 0;
        record->mwdt1_config2 = 0;
        record->mwdt1_config3 = 0;
        record->mwdt1_config4 = 0;
        record->super_wdt_config = 0;
        record->writer_status = 0;
        record->checksum = 0;
        record->timg0_status = SHIZUKU_C6_DIAG_UNKNOWN;
        record->timg1_status = SHIZUKU_C6_DIAG_UNKNOWN;
        record->rwdt_config0_after_stop = SHIZUKU_C6_DIAG_UNKNOWN;
    }
    shizuku_c6_early_diag_log.version = SHIZUKU_C6_EARLY_DIAG_VERSION;
    shizuku_c6_early_diag_log.magic = SHIZUKU_C6_EARLY_DIAG_MAGIC;
}

static uint32_t timer_group_status(bool group0)
{
    const uint32_t conf = group0 ? PCR.timergroup0_conf.val : PCR.timergroup1_conf.val;
    const uint32_t wdt_clk = group0 ? PCR.timergroup0_wdt_clk_conf.val : PCR.timergroup1_wdt_clk_conf.val;
    const uint32_t apb_clock_mask = group0 ? PCR_TG0_CLK_EN : PCR_TG1_CLK_EN;
    const uint32_t reset_mask = group0 ? PCR_TG0_RST_EN : PCR_TG1_RST_EN;
    const uint32_t wdt_clock_mask = group0 ? PCR_TG0_WDT_CLK_EN : PCR_TG1_WDT_CLK_EN;
    uint32_t status = 0;
    if ((conf & apb_clock_mask) != 0) status |= SHIZUKU_C6_TG_APB_CLOCK_ENABLED;
    // PCR tg*_rst_en is active-high: zero means the timer group is released from reset.
    if ((conf & reset_mask) == 0) status |= SHIZUKU_C6_TG_RESET_RELEASED;
    if ((wdt_clk & wdt_clock_mask) != 0) status |= SHIZUKU_C6_TG_WDT_CLOCK_ENABLED;
    return status;
}

static void snapshot_watchdogs(volatile shizuku_c6_early_diag_record_t *record)
{
#if SOC_RTC_WDT_SUPPORTED
    record->rwdt_config0 = LP_WDT.config0.val;
    record->rwdt_config1 = LP_WDT.config1.val;
    record->rwdt_config2 = LP_WDT.config2.val;
    record->rwdt_config3 = LP_WDT.config3.val;
    record->rwdt_config4 = LP_WDT.config4.val;
    record->super_wdt_config = LP_WDT.swd_config.val;
#else
    record->rwdt_config0 = record->rwdt_config1 = record->rwdt_config2 = record->rwdt_config3 = record->rwdt_config4 = SHIZUKU_C6_DIAG_UNKNOWN;
    record->super_wdt_config = SHIZUKU_C6_DIAG_UNKNOWN;
#endif

#if SOC_WDT_SUPPORTED
    record->timg0_status = timer_group_status(true);
    const uint32_t required = SHIZUKU_C6_TG_APB_CLOCK_ENABLED | SHIZUKU_C6_TG_RESET_RELEASED | SHIZUKU_C6_TG_WDT_CLOCK_ENABLED;
    if ((record->timg0_status & required) == required) {
        record->mwdt0_config0 = TIMERG0.wdtconfig0.val;
        record->mwdt0_config1 = TIMERG0.wdtconfig1.val;
        record->mwdt0_config2 = TIMERG0.wdtconfig2.val;
        record->mwdt0_config3 = TIMERG0.wdtconfig3.val;
        record->mwdt0_config4 = TIMERG0.wdtconfig4.val;
        record->timg0_status |= SHIZUKU_C6_TG_REGS_READ;
    } else {
        record->mwdt0_config0 = record->mwdt0_config1 = record->mwdt0_config2 = record->mwdt0_config3 = record->mwdt0_config4 = SHIZUKU_C6_DIAG_UNKNOWN;
    }
    record->timg1_status = timer_group_status(false);
    if ((record->timg1_status & required) == required) {
        record->mwdt1_config0 = TIMERG1.wdtconfig0.val;
        record->mwdt1_config1 = TIMERG1.wdtconfig1.val;
        record->mwdt1_config2 = TIMERG1.wdtconfig2.val;
        record->mwdt1_config3 = TIMERG1.wdtconfig3.val;
        record->mwdt1_config4 = TIMERG1.wdtconfig4.val;
        record->timg1_status |= SHIZUKU_C6_TG_REGS_READ;
    } else {
        record->mwdt1_config0 = record->mwdt1_config1 = record->mwdt1_config2 = record->mwdt1_config3 = record->mwdt1_config4 = SHIZUKU_C6_DIAG_UNKNOWN;
    }
#else
    record->timg0_status = record->timg1_status = SHIZUKU_C6_DIAG_UNKNOWN;
    record->mwdt0_config0 = record->mwdt0_config1 = record->mwdt0_config2 = record->mwdt0_config3 = record->mwdt0_config4 = SHIZUKU_C6_DIAG_UNKNOWN;
    record->mwdt1_config0 = record->mwdt1_config1 = record->mwdt1_config2 = record->mwdt1_config3 = record->mwdt1_config4 = SHIZUKU_C6_DIAG_UNKNOWN;
#endif
}

static void stop_rtc_wdt(void)
{
#if SOC_RTC_WDT_SUPPORTED
    wdt_hal_context_t rtc_wdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
    if (wdt_hal_is_enabled(&rtc_wdt_ctx)) {
        wdt_hal_write_protect_disable(&rtc_wdt_ctx);
        wdt_hal_disable(&rtc_wdt_ctx);
        wdt_hal_write_protect_enable(&rtc_wdt_ctx);
    }
#endif
}

void shizuku_c6_early_diag_begin(void)
{
    // Commit an entry marker before calling ROM: if the ROM reset-reason helper
    // stalls or faults, a live sysbus snapshot can locate progress. Keep this
    // separate from ring.magic: overwriting magic discards retained history.
    shizuku_c6_early_diag_progress = SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC;
    __asm__ volatile("fence rw, rw" ::: "memory");
    const uint32_t raw_reset_reason = (uint32_t)esp_rom_get_reset_reason(0);
    shizuku_c6_early_diag_progress = 2;

    if (shizuku_c6_early_diag_log.magic != SHIZUKU_C6_EARLY_DIAG_MAGIC ||
        shizuku_c6_early_diag_log.version != SHIZUKU_C6_EARLY_DIAG_VERSION) {
        initialize_log();
    }

    bool found = false;
    uint32_t latest_sequence = 0;
    for (unsigned i = 0; i < SHIZUKU_C6_EARLY_DIAG_RECORD_COUNT; ++i) {
        const volatile shizuku_c6_early_diag_record_t *record = &shizuku_c6_early_diag_log.records[i];
        if (record_is_valid(record) && (!found || record->sequence > latest_sequence)) {
            latest_sequence = record->sequence;
            found = true;
        }
    }

    const uint32_t sequence = found ? latest_sequence + 1u : 0u;
    s_active_slot = sequence % SHIZUKU_C6_EARLY_DIAG_RECORD_COUNT;
    volatile shizuku_c6_early_diag_record_t *record = &shizuku_c6_early_diag_log.records[s_active_slot];
    record->checksum = 0;
    record->sequence = sequence;
    // Raw ROM reset reason is captured before USB writes or diagnostic status updates.
    record->raw_reset_reason = raw_reset_reason;
    record->rwdt_config0_after_stop = SHIZUKU_C6_DIAG_UNKNOWN;
    shizuku_c6_early_diag_progress = 3;
    snapshot_watchdogs(record);
    record->writer_status = SHIZUKU_C6_EARLY_WRITER_PENDING;
    s_active_record_valid = false;
    stop_rtc_wdt();
#if SOC_RTC_WDT_SUPPORTED
    record->rwdt_config0_after_stop = LP_WDT.config0.val;
#endif
    record->checksum = record_checksum(record);
    s_active_record_valid = true;
    shizuku_c6_early_diag_progress = 4;
    __asm__ volatile("fence rw,rw" ::: "memory");
}

void shizuku_c6_early_diag_writer_result(uint32_t result)
{
    if (!s_active_record_valid || s_active_slot >= SHIZUKU_C6_EARLY_DIAG_RECORD_COUNT) {
        return;
    }
    volatile shizuku_c6_early_diag_record_t *record = &shizuku_c6_early_diag_log.records[s_active_slot];
    if (!record_is_valid(record)) {
        return;
    }
    record->checksum = 0;
    record->writer_status = result;
    record->checksum = record_checksum(record);
}
