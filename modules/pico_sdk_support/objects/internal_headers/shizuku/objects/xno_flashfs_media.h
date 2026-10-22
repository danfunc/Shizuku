#ifndef SHIZUKU_OBJECTS_XNO_FLASHFS_MEDIA_H
#define SHIZUKU_OBJECTS_XNO_FLASHFS_MEDIA_H

// ===========================================================================
//  xno::flash_fs の媒体境界 (plain C) の RP2040 / RP2350 実装の宣言
//  ★XNO objects/flash_fs/include/xno/flash_fs/flash_fs_media.h と同一シグネチャを保つこと。
//    Shizuku は XNO のヘッダを include しない (依存の向きは XNO → Shizuku)。
//    XNO 側の宣言を変えたら、ここも同じ形に直す。
// ===========================================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XNO_FLASH_FS_MEDIA_ABI_VERSION 1u
#define XNO_FLASH_FS_SECTOR_SIZE 4096u
#define XNO_FLASH_FS_PAGE_SIZE 256u
#define XNO_FLASH_FS_BLOCK_ERASE_SIZE (64u * 1024u)

typedef struct xno_flash_fs_geometry {
  uint32_t flash_bytes;
  uint32_t region_offset;
  uint32_t region_bytes;
  uintptr_t mapped_base;
  uint32_t sector_size;
  uint32_t page_size;
  uint32_t block_erase_size;
  uint32_t format_version;
} xno_flash_fs_geometry;

typedef struct xno_flash_fs_timing {
  uint32_t elapsed_us;
} xno_flash_fs_timing;

int32_t xno_flashfs_media_initialize(void);
const xno_flash_fs_geometry *xno_flashfs_media_layout(void);
uintptr_t xno_flashfs_media_mapped_address(uint32_t flash_offset);
uint32_t xno_flashfs_media_offset_of_address(uintptr_t mapped_address);
uint32_t xno_flashfs_media_persistent_address(uintptr_t mapped_address);
uintptr_t xno_flashfs_media_runtime_address(uint32_t persisted_address);
uintptr_t xno_flashfs_media_firmware_end_address(void);
int32_t xno_flashfs_media_read(uint32_t flash_offset, void *borrowed_destination,
                               uint32_t bytes);
int32_t xno_flashfs_media_erase(uint32_t flash_offset, uint32_t bytes,
                                xno_flash_fs_timing *borrowed_timing);
int32_t xno_flashfs_media_program(uint32_t flash_offset,
                                  const uint8_t *borrowed_source,
                                  uint32_t bytes,
                                  xno_flash_fs_timing *borrowed_timing);
uint64_t xno_flashfs_media_time_us(void);
uint8_t xno_flashfs_media_writes_allowed(void);
void xno_flashfs_media_before_write(uint32_t flash_offset);
void xno_flashfs_media_before_erase(uint32_t flash_offset);
void xno_flashfs_media_after_erase(uint32_t flash_offset);
void xno_flashfs_media_before_program(uint32_t flash_offset);
void xno_flashfs_media_after_program(uint32_t flash_offset);
void xno_flashfs_media_after_write(uint32_t flash_offset);
void xno_flashfs_media_breadcrumb(uint8_t entering);
void xno_flashfs_media_trace_phase(uint32_t stage);
void xno_flashfs_media_progress_mark(uint32_t stage, uint32_t flash_offset);
void xno_flashfs_media_store_metrics(uint32_t total_us, uint32_t erase_us,
                                     uint32_t program_us);

#ifdef __cplusplus
}
#endif

#endif // SHIZUKU_OBJECTS_XNO_FLASHFS_MEDIA_H
