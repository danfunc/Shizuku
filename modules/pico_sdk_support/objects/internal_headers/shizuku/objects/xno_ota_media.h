#ifndef SHIZUKU_OBJECTS_XNO_OTA_MEDIA_H
#define SHIZUKU_OBJECTS_XNO_OTA_MEDIA_H

// ===========================================================================
//  xno::ota の媒体境界 (plain C) の RP2040 / RP2350 実装の宣言
//  ★XNO objects/ota/include/xno/ota/ota_media.h と同一シグネチャを保つこと。
//    Shizuku は XNO のヘッダを include しない (依存の向きは XNO → Shizuku)。
//    XNO 側の宣言を変えたら、ここも同じ形に直す。
// ===========================================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XNO_OTA_MEDIA_OK 0
#define XNO_OTA_MEDIA_BUSY (-1)

#ifndef XNO_OTA_MEDIA_SECTOR_SIZE
#define XNO_OTA_MEDIA_SECTOR_SIZE 4096u
#endif
#ifndef XNO_OTA_MEDIA_PAGE_SIZE
#define XNO_OTA_MEDIA_PAGE_SIZE 256u
#endif
#ifndef XNO_OTA_MEDIA_BLOCK_SIZE
#define XNO_OTA_MEDIA_BLOCK_SIZE (64u * 1024u)
#endif

// 消去: flash_offset から bytes バイトを消す。★RAM 常駐 (XIP 停止中に呼ばれる)。
int32_t xno_ota_media_erase(uint32_t flash_offset, uint32_t bytes);

// 書込み: flash_offset へ data から bytes バイトを書く。★RAM 常駐。
int32_t xno_ota_media_program(uint32_t flash_offset, const uint8_t *data,
                              uint32_t bytes);

// XIP で flash_offset を読むための CPU アドレス。
uintptr_t xno_ota_media_xip_address(uint32_t flash_offset);

// XIP 領域の先頭アドレス。
uint32_t xno_ota_media_xip_base(void);

// SRAM 領域の先頭アドレス。
uint32_t xno_ota_media_sram_base(void);

// 現在の本体イメージの書込み先 flash オフセット。
uint32_t xno_ota_media_image_flash_offset(void);

// 現在の本体イメージの長さ (バイト)。
uint32_t xno_ota_media_image_bytes(void);

// イメージ先頭から、ベクタ表 (SP, reset) までのオフセット。RP2040 は 256、RP2350 は 0。
uint32_t xno_ota_media_vector_table_offset(void);

// commit 後の再起動を要求する。戻るのは失敗時だけ。★RAM 常駐。
void xno_ota_media_reboot_after_commit(void);

// 割り込みを待つ (wfi)。★RAM 常駐。
void xno_ota_media_wait_for_interrupt(void);

// 緊急 UART 出力。未初期化なら黙って戻る。
void xno_ota_media_emergency_puts(const char *text);

// 消去・書込みのホットパスに足跡を出すか。1 = 出す、0 = 出さない。
uint8_t xno_ota_media_hot_path_trace_enabled(void);

// 診断フック。段階の値は旧 ota.cpp の ota_erase_stage 列挙に従う。
void xno_ota_media_diag_note_thread_yield(uint32_t thread_id);
void xno_ota_media_diag_erase_stage(uint32_t stage, uint32_t flash_offset);
void xno_ota_media_diag_erase_progress(uint32_t count, uint32_t elapsed_us);
void xno_ota_media_diag_state(uint32_t state);
void xno_ota_media_diag_init_begin(void);

#ifdef __cplusplus
}
#endif

#endif // SHIZUKU_OBJECTS_XNO_OTA_MEDIA_H
