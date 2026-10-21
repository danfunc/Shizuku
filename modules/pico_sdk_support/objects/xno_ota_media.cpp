// ===========================================================================
//  xno::ota の媒体境界 (xno_ota_media) — RP2040 / RP2350 の実装
// ===========================================================================
//  旧 ota.cpp の erase_range / program_range / commit_blast / say /
//  begin_commit から、チップ依存の部分だけを写す。挙動は変えない。
//  ★消去・書込みのリトライ (flash_ops_retry)、flash_quiet、commit の手順は
//    ota 側 (XNO) の責務で、ここには無い。
// ===========================================================================
#include "shizuku/objects/xno_ota_media.h"

#include "shizuku/objects/flash_ops.hpp"
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
#include "shizuku/objects/usb_cdc.hpp"
#endif

extern "C" {
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#if !defined(SHIZUKU_RP2040)
#include "boot/picoboot_constants.h"
#include "pico/bootrom.h"
#endif
#include "pico/error.h"
#include "pico/platform.h"
}

#include <cstdint>

// commit のコピー元・先の範囲。リンカから取得。
extern "C" {
extern uint8_t __flash_binary_start[];
extern uint8_t __flash_binary_end[];
}

namespace {

// 旧 ota.cpp の ota_erase_stage 列挙と同じ値。消去の段階を診断へ残すときに使う。
constexpr uint32_t OTA_ERASE_SAFE_ZONE_ENTERED = 2;
constexpr uint32_t OTA_ERASE_FLASH_CALL = 3;
constexpr uint32_t OTA_ERASE_CALLBACK_DONE = 4;
constexpr uint32_t OTA_ERASE_FAILED = 6;

// flash_ops は PICO_OK / PICO_ERROR_INVALID_ARG / PICO_ERROR_RESOURCE_IN_USE しか返さない。
// RESOURCE_IN_USE だけ「再試行してよい」(BUSY) に写し、それ以外はそのまま失敗として返す。
int32_t to_media_rc(int rc) {
  if (rc == PICO_OK)
    return XNO_OTA_MEDIA_OK;
  if (rc == PICO_ERROR_RESOURCE_IN_USE)
    return XNO_OTA_MEDIA_BUSY;
  return rc;
}

#if !defined(SHIZUKU_RP2040)
// 旧 ota.cpp の reboot_fn と同じ。RP2350 の ROM REBOOT2 の引数の形。
using reboot_fn = int (*)(uint32_t, uint32_t, uint32_t, uint32_t);
#endif

} // namespace

// ---------------------------------------------------------------------------
//  消去・書込み (RAM 常駐。旧 erase_range / program_range / commit_blast の
//  flash_ops 呼出しと同じ)
// ---------------------------------------------------------------------------

int32_t __no_inline_not_in_flash_func(xno_ota_media_erase)(uint32_t flash_offset,
                                                           uint32_t bytes) {
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  // 旧 erase_range と同じ位置で段階を残す (ota 側の STARTED / COMPLETE とは別)。
  usb_cdc_boot_diag_ota_erase_stage(OTA_ERASE_SAFE_ZONE_ENTERED, flash_offset);
  usb_cdc_boot_diag_ota_erase_stage(OTA_ERASE_FLASH_CALL, flash_offset);
#endif
  const int rc = shizuku::objects::flash_ops::erase(flash_offset, bytes);
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  usb_cdc_boot_diag_ota_erase_stage(
      rc == PICO_OK ? OTA_ERASE_CALLBACK_DONE : OTA_ERASE_FAILED, flash_offset);
#endif
  return to_media_rc(rc);
}

int32_t __no_inline_not_in_flash_func(xno_ota_media_program)(uint32_t flash_offset,
                                                             const uint8_t *data,
                                                             uint32_t bytes) {
  return to_media_rc(
      shizuku::objects::flash_ops::program(flash_offset, data, bytes));
}

// ---------------------------------------------------------------------------
//  XIP・イメージの位置 (RAM 常駐は不要)
// ---------------------------------------------------------------------------

uintptr_t xno_ota_media_xip_address(uint32_t flash_offset) {
  return (uintptr_t)(XIP_BASE + flash_offset);
}

uint32_t xno_ota_media_xip_base(void) { return (uint32_t)XIP_BASE; }

uint32_t xno_ota_media_sram_base(void) { return (uint32_t)SRAM_BASE; }

uint32_t xno_ota_media_image_flash_offset(void) {
  return (uint32_t)((uintptr_t)__flash_binary_start - XIP_BASE);
}

uint32_t xno_ota_media_image_bytes(void) {
  return (uint32_t)((uintptr_t)__flash_binary_end -
                    (uintptr_t)__flash_binary_start);
}

uint32_t xno_ota_media_vector_table_offset(void) {
  // RP2040 の .bin は linker script 上、__flash_binary_start に続いて boot2
  // (1 flash page = 256B) を含み、その後に vector table が始まる。RP2350 の
  // OTA .bin は vector table が先頭なので 0。
#if defined(SHIZUKU_RP2040)
  return FLASH_PAGE_SIZE;
#else
  return 0;
#endif
}

// ---------------------------------------------------------------------------
//  再起動・割り込み待ち (RAM 常駐。旧 commit_blast の末尾と同じ)
// ---------------------------------------------------------------------------

void __no_inline_not_in_flash_func(xno_ota_media_reboot_after_commit)(void) {
#if defined(SHIZUKU_RP2040)
  // RP2040 には REBOOT2 ROM API がないため watchdog reset を使う。戻ってきたら
  // ota 側が wfi で止まる (上書き済み flash へ戻らないため)。
  watchdog_reboot(0, 0, 0);
#else
  // ROM の REBOOT2 が使えれば使い、戻ってきたら SCB の SYSRESETREQ で再起動する。
  const reboot_fn reboot =
      (reboot_fn)rom_func_lookup_inline(ROM_FUNC_REBOOT);
  if (reboot != nullptr) {
    reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS,
           10, 0, 0);
  }
  *((volatile uint32_t *)0xE000ED0C) = 0x05FA0004;
#endif
}

void __no_inline_not_in_flash_func(xno_ota_media_wait_for_interrupt)(void) {
  __asm volatile("wfi");
}

// ---------------------------------------------------------------------------
//  緊急出力・診断 (RAM 常駐は不要)
// ---------------------------------------------------------------------------

void xno_ota_media_emergency_puts(const char *text) {
#if defined(SHIZUKU_CRASH_RECORD)
  // クラッシュ記録ビルドは非ブロッキングで 1 文字ずつ送る (旧 uart_try_puts と同じ)。
  if (!uart_is_enabled(uart0))
    return;
  while (*text != '\0' && uart_is_writable(uart0))
    uart_putc_raw(uart0, *text++);
#else
  if (uart_is_enabled(uart0))
    uart_puts(uart0, text);
#endif
}

uint8_t xno_ota_media_hot_path_trace_enabled(void) {
  // クラッシュ記録ビルドは、フラッシュ操作の最中に出力しないため 0 を返す (旧 OTA_TRACE と同じ)。
#if defined(SHIZUKU_CRASH_RECORD)
  return 0;
#else
  return 1;
#endif
}

void xno_ota_media_diag_note_thread_yield(uint32_t thread_id) {
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  usb_cdc_boot_diag_note_thread_yield(thread_id);
#else
  (void)thread_id;
#endif
}

void xno_ota_media_diag_erase_stage(uint32_t stage, uint32_t flash_offset) {
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  usb_cdc_boot_diag_ota_erase_stage(stage, flash_offset);
#else
  (void)stage;
  (void)flash_offset;
#endif
}

void xno_ota_media_diag_erase_progress(uint32_t count, uint32_t elapsed_us) {
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  usb_cdc_boot_diag_ota_erase_progress(count, elapsed_us);
#else
  (void)count;
  (void)elapsed_us;
#endif
}

void xno_ota_media_diag_state(uint32_t state) {
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  usb_cdc_boot_diag_ota_state(state);
#else
  (void)state;
#endif
}

void xno_ota_media_diag_init_begin(void) {
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  usb_cdc_boot_diag_ota_init_begin();
#endif
}
