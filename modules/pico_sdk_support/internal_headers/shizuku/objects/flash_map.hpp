#ifndef SHIZUKU_OBJECTS_FLASH_MAP_HPP
#define SHIZUKU_OBJECTS_FLASH_MAP_HPP
#include "hardware/flash.h"
#include <cstdint>

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
// btstack の bonding バンク。★SDK の pico_btstack が
//   PICO_FLASH_BANK_STORAGE_OFFSET
//     = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE - PICO_FLASH_BANK_TOTAL_SIZE
//   (RP2350)、PICO_FLASH_BANK_TOTAL_SIZE = FLASH_SECTOR_SIZE * 2 で置いている。
//   ★ここは**こちらが管理しない**。cyw43_arch_init() の中で SDK が張るので、
//     自前で TLV を張り直すと二重管理になって板が起動不能になる (実測 4 回)。
constexpr uint32_t BT_RESERVED_BYTES = FLASH_SECTOR_SIZE * 3;
constexpr uint32_t BT_RESERVED_OFFSET = PICO_FLASH_SIZE_BYTES - BT_RESERVED_BYTES;

// ---- 先頭: 走っているファームウェア ---------------------------------------
constexpr uint32_t FIRMWARE_OFFSET = 0;
constexpr uint32_t FIRMWARE_BYTES = 1024 * 1024;

// ---- flash FS ------------------------------------------------------------
constexpr uint32_t FS_OFFSET = FIRMWARE_OFFSET + FIRMWARE_BYTES;
constexpr uint32_t FS_BYTES = 1024 * 1024;
constexpr uintptr_t FS_ADDRESS = XIP_BASE + FS_OFFSET;

// ---- ota のステージング (bonding バンクの直下で終わる) --------------------
constexpr uint32_t STAGING_OFFSET = FS_OFFSET + FS_BYTES;
constexpr uint32_t STAGING_BYTES = BT_RESERVED_OFFSET - STAGING_OFFSET;

// ---- 重なっていないことを機械に確かめさせる -------------------------------
static_assert(FIRMWARE_OFFSET + FIRMWARE_BYTES <= FS_OFFSET,
              "ファームウェアが flash FS に食い込んでいる");
static_assert(FS_OFFSET + FS_BYTES <= STAGING_OFFSET,
              "flash FS が ota のステージングに食い込んでいる");
static_assert(STAGING_OFFSET + STAGING_BYTES <= BT_RESERVED_OFFSET,
              "ota のステージングが btstack の bonding バンクに食い込んでいる");
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
