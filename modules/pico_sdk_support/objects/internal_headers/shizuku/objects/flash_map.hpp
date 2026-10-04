#ifndef SHIZUKU_OBJECTS_FLASH_MAP_HPP
#define SHIZUKU_OBJECTS_FLASH_MAP_HPP
#include "hardware/flash.h"
#include <cstdint>
#if defined(CYW43_WL_GPIO_LED_PIN)
#include "pico/btstack_flash_bank.h"
#ifdef pico_flash_bank_get_storage_offset_func
#error "Flash map requires a compile-time BTstack bank offset"
#endif
#endif
#include "shizuku/objects/flash_layout_config.hpp"

// ===========================================================================
//  flash の割り付け — **一箇所で決め、重なりを機械に確かめさせる**
// ===========================================================================
//  ★ここを作った理由: 以前は各オブジェクトが自分の都合で場所を選んでいた。
//    flash_fs は「末尾 1MB を取る」、ota は「0x100000 から 1MB」。どちらも
//    単体では妥当だが、**重なっていないことを誰も確かめていなかった** —
//    実際 flash_fs の末尾 1MB は btstack の bonding バンク (SDK が持っている)
//    を物理的に含んでいて、両方を有効にするとペアリング鍵が壊れる関係だった。
//    場所を分散して持つ限り、この種の事故は「気をつける」でしか防げない。
//
//  ★末尾に置くものの選び方: **bonding バンクの隣は ota のステージング**にする。
//    - ota のステージングは書くたびに丸ごと使い捨てで、大きさも用途も固定。
//      「ここから先は触らない」という上限を 1 本引けば守り切れる
//    - flash_fs は bump 割り付けで伸びるので、上限を実行時に守り続ける必要が
//      ある。守り損ねたときに壊れるのが**消えては困るデータのほう**になる
//    つまり、境界の見張りが要る側を境界から遠ざけている。
namespace shizuku {
namespace objects {
namespace flash_map {

// ---- SDK が持っている末尾 -------------------------------------------------
// btstack の bonding バンク。offset と bank size はSDKヘッダから取得する。
// RP2040では末尾2セクタ、RP2350 A2では末尾から3セクタ目に2セクタを置く
// (末尾1セクタはRP2350-E10 workaround用)。SDK設定が変わってもここを追従させる。
#if defined(CYW43_WL_GPIO_LED_PIN)
constexpr uint32_t BT_RESERVED_OFFSET = PICO_FLASH_BANK_STORAGE_OFFSET;
constexpr uint32_t BT_RESERVED_BYTES = PICO_FLASH_SIZE_BYTES - BT_RESERVED_OFFSET;
static_assert(PICO_FLASH_BANK_TOTAL_SIZE % FLASH_SECTOR_SIZE == 0,
              "BTstack flash bank must use whole sectors");
static_assert(PICO_FLASH_BANK_STORAGE_OFFSET % FLASH_SECTOR_SIZE == 0,
              "BTstack flash bank offset must be sector aligned");
static_assert(PICO_FLASH_BANK_STORAGE_OFFSET <= PICO_FLASH_SIZE_BYTES &&
                  PICO_FLASH_BANK_TOTAL_SIZE <=
                      PICO_FLASH_SIZE_BYTES - PICO_FLASH_BANK_STORAGE_OFFSET,
              "BTstack flash bank exceeds board flash size");
static_assert(BT_RESERVED_BYTES >= PICO_FLASH_BANK_TOTAL_SIZE,
              "BTstack reservation does not contain both flash banks");
static_assert(BT_RESERVED_BYTES % FLASH_SECTOR_SIZE == 0,
              "BTstack reserved tail must use whole sectors");
#else
// Non-W boards have no BTstack persistence bank to reserve.
constexpr uint32_t BT_RESERVED_OFFSET = PICO_FLASH_SIZE_BYTES;
constexpr uint32_t BT_RESERVED_BYTES = 0;
#endif
static_assert(PICO_FLASH_SIZE_BYTES % FLASH_SECTOR_SIZE == 0,
              "board flash size must contain whole sectors");
static_assert(SHIZUKU_FLASH_CAPACITY_BYTES >= 0,
              "logical flash capacity must not be negative");
static_assert(SHIZUKU_LOGICAL_FLASH_BYTES <= PICO_FLASH_SIZE_BYTES,
              "logical flash capacity exceeds the physical board flash size");
static_assert(SHIZUKU_LOGICAL_FLASH_BYTES % FLASH_SECTOR_SIZE == 0,
              "logical flash capacity must be sector aligned");
constexpr uint32_t LOGICAL_FLASH_LIMIT =
    SHIZUKU_LOGICAL_FLASH_BYTES < BT_RESERVED_OFFSET
        ? SHIZUKU_LOGICAL_FLASH_BYTES
        : BT_RESERVED_OFFSET;

// ---- 先頭: 走っているファームウェア ---------------------------------------
constexpr uint32_t FIRMWARE_OFFSET = 0;
#if defined(SHIZUKU_RP2040)
// RP2040 reserves fixed, equal firmware and OTA windows. Keep the firmware
// ceiling in this one constant; the remaining pre-BT area belongs to flash FS.
constexpr uint32_t RP2040_FIRMWARE_BYTES = SHIZUKU_FIRMWARE_BYTES;
constexpr uint32_t RP2040_STAGING_BYTES = SHIZUKU_STAGING_BYTES;
static_assert(RP2040_FIRMWARE_BYTES <= LOGICAL_FLASH_LIMIT,
              "firmware reservation exceeds flash before BT storage");
static_assert(RP2040_STAGING_BYTES <=
                  LOGICAL_FLASH_LIMIT - RP2040_FIRMWARE_BYTES,
              "firmware and OTA reservations exceed flash before BT storage");
static_assert(LOGICAL_FLASH_LIMIT >=
                  RP2040_FIRMWARE_BYTES + RP2040_STAGING_BYTES + FLASH_SECTOR_SIZE,
              "logical flash capacity cannot contain firmware, OTA, and one FS sector");
constexpr uint32_t FIRMWARE_BYTES = RP2040_FIRMWARE_BYTES;
constexpr uint32_t FS_OFFSET = FIRMWARE_OFFSET + FIRMWARE_BYTES;
constexpr uint32_t FS_BYTES =
    LOGICAL_FLASH_LIMIT - RP2040_FIRMWARE_BYTES - RP2040_STAGING_BYTES -
    FIRMWARE_OFFSET;
constexpr uintptr_t FS_ADDRESS = XIP_BASE + FS_OFFSET;
constexpr uint32_t STAGING_OFFSET = LOGICAL_FLASH_LIMIT - RP2040_STAGING_BYTES;
#else
constexpr uint32_t FIRMWARE_BYTES = SHIZUKU_FIRMWARE_BYTES;

// ---- flash FS ------------------------------------------------------------
constexpr uint32_t FS_OFFSET = FIRMWARE_OFFSET + FIRMWARE_BYTES;
constexpr uint32_t FS_BYTES = SHIZUKU_FS_BYTES;
constexpr uintptr_t FS_ADDRESS = XIP_BASE + FS_OFFSET;

// ---- ota のステージング (bonding バンクの直下で終わる) --------------------
constexpr uint32_t STAGING_OFFSET = FS_OFFSET + FS_BYTES;
#endif
static_assert(LOGICAL_FLASH_LIMIT >= STAGING_OFFSET + FIRMWARE_BYTES,
              "logical flash capacity cannot contain the firmware and OTA partitions");
static_assert(STAGING_OFFSET <= LOGICAL_FLASH_LIMIT,
              "ota staging offset exceeds btstack reserved region");
constexpr uint32_t STAGING_BYTES = LOGICAL_FLASH_LIMIT - STAGING_OFFSET;

// ---- 重なっていないことを機械に確かめさせる -------------------------------
static_assert(FIRMWARE_OFFSET + FIRMWARE_BYTES <= FS_OFFSET,
              "ファームウェアが flash FS に食い込んでいる");
static_assert(FIRMWARE_OFFSET + FIRMWARE_BYTES <= BT_RESERVED_OFFSET,
              "firmware reservation overlaps BTstack or exceeds board flash");
static_assert(FS_OFFSET + FS_BYTES <= STAGING_OFFSET,
              "flash FS が ota のステージングに食い込んでいる");
static_assert(STAGING_OFFSET + STAGING_BYTES <= LOGICAL_FLASH_LIMIT,
              "ota staging overlaps the logical flash boundary");
static_assert(LOGICAL_FLASH_LIMIT <= BT_RESERVED_OFFSET,
              "logical flash partitions overlap BTstack storage");
static_assert(BT_RESERVED_OFFSET + BT_RESERVED_BYTES == PICO_FLASH_SIZE_BYTES,
              "SDK の末尾予約の計算が合っていない");
// ★ステージングには**今のファームより大きな像**が入る必要がある (OTA は
//   「自分より新しい自分」を受ける)。
static_assert(STAGING_BYTES >= FIRMWARE_BYTES,
              "ステージングがファームウェアより小さい");
// 消去はセクタ単位でしかできないので、境界は全部セクタ境界に乗せる。
static_assert(FS_OFFSET % FLASH_SECTOR_SIZE == 0, "FS_OFFSET がセクタ境界でない");
static_assert(FS_BYTES % FLASH_SECTOR_SIZE == 0, "FS_BYTES がセクタ境界でない");
static_assert(STAGING_OFFSET % FLASH_SECTOR_SIZE == 0,
              "STAGING_OFFSET がセクタ境界でない");
static_assert(STAGING_BYTES % FLASH_SECTOR_SIZE == 0,
              "STAGING_BYTES がセクタ境界でない");

} // namespace flash_map
} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_FLASH_MAP_HPP
