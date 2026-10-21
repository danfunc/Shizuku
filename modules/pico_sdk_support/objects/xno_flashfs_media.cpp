// ===========================================================================
//  xno::flash_fs の媒体境界 (xno_flashfs_media) — RP2040 / RP2350 の実装
// ===========================================================================
//  旧 flash_fs.cpp の XIP 変換・消去/書込み呼出し・時刻・RP2040 の診断フックから、
//  チップ依存の部分だけを写す。挙動は変えない。
//  ★消去・書込みの実体 (停止・割り込み禁止・保護解除・RAM 転送) は flash_ops の
//    責務で、ここには無い。目録・書込み順序・再試行は XNO flash_fs の責務。
//  ★旧 flash_fs.cpp にはミュート (flash_quiet) もバンク退避も無い。flash_quiet は
//    ota.cpp の機能で、BTstack のバンクは flash_map が静的に確保する (実行時の退避は無い)。
// ===========================================================================
#include "shizuku/objects/xno_flashfs_media.h"

#include "shizuku/objects/flash_map.hpp"
#include "shizuku/objects/flash_ops.hpp"
#if defined(SHIZUKU_CRASH_RECORD)
#include "shizuku/objects/usb_cdc.hpp"
#endif

#include <cstring>

extern "C" {
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/error.h"
#include "pico/platform.h"
#include "pico/time.h"
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
#include "hardware/structs/scb.h"
#include "hardware/structs/watchdog.h"
#endif
}

// リンカが置いたファーム末尾。旧 flash_fs.cpp と同じく、名前空間の外で受ける。
extern "C" {
extern uint8_t __flash_binary_end[];
}

#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
extern "C" void shizuku_selftest_progress_mark(uint32_t, uint32_t, uint32_t);
#endif
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
extern "C" void shizuku_boot_trace_phase(uint32_t stage);
#endif

// flash_ops::erase / program が拒否されるかどうかを見る、弱い hook (flash_ops.cpp と同じ宣言)。
extern "C" bool shizuku_flash_operation_allowed() __attribute__((weak));

#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
// ---------------------------------------------------------------------------
//  RP2040 の selftest 用の fault 経路 (旧 flash_fs.cpp の shizuku_fs_fault_* と同じ)。
//  書込み区間だけ HardFault のベクタを差し替え、フレームを watchdog scratch へ残す。
//  ★名前は旧 flash_fs と衝突しないよう xno_flashfs_ を付ける。
// ---------------------------------------------------------------------------
extern "C" void __not_in_flash_func(xno_flashfs_fault_c)(uint32_t *frame) {
  watchdog_hw->scratch[4] = 0xFA17FA17u;
  watchdog_hw->scratch[5] = frame[6];
  watchdog_hw->scratch[6] = frame[5];
  watchdog_hw->scratch[7] = frame[7];
  for (;;) {
  }
}
extern "C" __attribute__((naked, section(".time_critical.fs_fault_entry"))) void
xno_flashfs_fault_entry() {
  __asm volatile("mov r0, lr\n\t"
                 "movs r1, #4\n\t"
                 "tst r0, r1\n\t"
                 "bne 1f\n\t"
                 "mrs r0, msp\n\t"
                 "b 2f\n\t"
                 "1: mrs r0, psp\n\t"
                 "2: ldr r1, =xno_flashfs_fault_c\n\t"
                 "bx r1\n\t"
                 ".ltorg\n\t");
}
#endif

namespace {

// 旧 flash_fs.cpp の DIRECTORY_VERSION。既に焼かれている像と互換を保つため 2 のまま。
constexpr uint32_t kFormatVersion = 2;

#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
uint32_t g_saved_fault_vector = 0;
#endif

// 起動段階のトレース。RP2040 かつトレース有効のときだけ出す。
void boot_trace(uint32_t stage) {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_phase(stage);
#else
  (void)stage;
#endif
}

// selftest の進行マーク。RP2040 の selftest 進捗ビルドのときだけ出す。
void selftest_progress(uint32_t stage, uint32_t value) {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_progress_mark(stage, value, 0);
#else
  (void)stage;
  (void)value;
#endif
}

// offset / bytes が flash の中に収まるか。読みだけに使う (消去・書込みは flash_ops が検査する)。
bool inside_flash(uint32_t offset, uint32_t bytes) {
  return offset <= PICO_FLASH_SIZE_BYTES && bytes <= PICO_FLASH_SIZE_BYTES - offset;
}

} // namespace

// ---------------------------------------------------------------------------
//  初期化・区画 (旧 flash_fs.cpp は flash_map の定数をそのまま使う)
// ---------------------------------------------------------------------------

int32_t xno_flashfs_media_initialize(void) {
  // 区画は flash_map.hpp が一箇所で決め、static_assert が重なりを確かめている。
  // 実行時に確かめる項目が無いので、常に成功を返す。
  return 0;
}

const xno_flash_fs_geometry *xno_flashfs_media_layout(void) {
  static const xno_flash_fs_geometry geometry = {
      .flash_bytes = PICO_FLASH_SIZE_BYTES,
      .region_offset = shizuku::objects::flash_map::FS_OFFSET,
      .region_bytes = shizuku::objects::flash_map::FS_BYTES,
      .mapped_base = shizuku::objects::flash_map::FS_ADDRESS,
      .sector_size = XNO_FLASH_FS_SECTOR_SIZE,
      .page_size = XNO_FLASH_FS_PAGE_SIZE,
      .block_erase_size = XNO_FLASH_FS_BLOCK_ERASE_SIZE,
      .format_version = kFormatVersion,
  };
  return &geometry;
}

// ---------------------------------------------------------------------------
//  アドレス変換 (XIP)。旧 flash_fs.cpp の `bump - XIP_BASE` / `REGION_ADDRESS` と同じ。
// ---------------------------------------------------------------------------

uintptr_t xno_flashfs_media_mapped_address(uint32_t flash_offset) {
  return (uintptr_t)(XIP_BASE + flash_offset);
}

uint32_t xno_flashfs_media_offset_of_address(uintptr_t mapped_address) {
  return (uint32_t)(mapped_address - XIP_BASE);
}

// 旧 flash_fs は目録に絶対アドレスをそのまま焼いていた (fresh.address = bump)。
// その形式を変えると既存の像が読めなくなるので、RP では恒等変換にする。
uint32_t xno_flashfs_media_persistent_address(uintptr_t mapped_address) {
  return (uint32_t)mapped_address;
}

uintptr_t xno_flashfs_media_runtime_address(uint32_t persisted_address) {
  return (uintptr_t)persisted_address;
}

uintptr_t xno_flashfs_media_firmware_end_address(void) {
  return (uintptr_t)__flash_binary_end;
}

// ---------------------------------------------------------------------------
//  読出し・消去・書込み
// ---------------------------------------------------------------------------

int32_t xno_flashfs_media_read(uint32_t flash_offset, void *borrowed_destination,
                               uint32_t bytes) {
  if (borrowed_destination == nullptr || !inside_flash(flash_offset, bytes))
    return PICO_ERROR_INVALID_ARG;
  // XIP の読みは素のメモリ走査。消去・書込みの最中に呼ばれることは無い (XNO が直列化する)。
  std::memcpy(borrowed_destination,
              (const void *)xno_flashfs_media_mapped_address(flash_offset), bytes);
  return PICO_OK;
}

// 消去・書込みの停止・割り込み禁止・RAM 転送は flash_ops が行う。
// 戻り値は flash_ops のまま返す (0 = 成功、それ以外 = 失敗。XNO が閉じた側で失敗にする)。
int32_t xno_flashfs_media_erase(uint32_t flash_offset, uint32_t bytes,
                                xno_flash_fs_timing *borrowed_timing) {
  shizuku::objects::flash_ops::timing measure{};
  const int rc = shizuku::objects::flash_ops::erase(flash_offset, bytes, &measure);
  if (borrowed_timing != nullptr)
    borrowed_timing->elapsed_us = measure.elapsed_us;
  return rc;
}

// data は flash_ops の要件どおり SRAM にあること (XNO は静的な page buffer を渡す)。
int32_t xno_flashfs_media_program(uint32_t flash_offset,
                                  const uint8_t *borrowed_source,
                                  uint32_t bytes,
                                  xno_flash_fs_timing *borrowed_timing) {
  shizuku::objects::flash_ops::timing measure{};
  const int rc = shizuku::objects::flash_ops::program(flash_offset, borrowed_source,
                                                      bytes, &measure);
  if (borrowed_timing != nullptr)
    borrowed_timing->elapsed_us = measure.elapsed_us;
  return rc;
}

uint64_t xno_flashfs_media_time_us(void) { return time_us_64(); }

// flash_ops と同じ許可条件。hook が無ければ常に許可する (旧 flash_fs に独自の拒否は無かった)。
uint8_t xno_flashfs_media_writes_allowed(void) {
  return (shizuku_flash_operation_allowed == nullptr ||
          shizuku_flash_operation_allowed())
             ? 1u
             : 0u;
}

// ---------------------------------------------------------------------------
//  書込み区間の前後 (旧 flash_fs.cpp の flash_write の段階と同じ位置)
// ---------------------------------------------------------------------------

// 書込み全体 (消去と書込みの両方) を囲む。RP2040 selftest 時は HardFault のベクタを差し替える。
void xno_flashfs_media_before_write(uint32_t flash_offset) {
  (void)flash_offset;
  boot_trace(810);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  volatile uint32_t *vtable = (volatile uint32_t *)scb_hw->vtor;
  g_saved_fault_vector = vtable[3];
  vtable[3] = (uint32_t)xno_flashfs_fault_entry | 1u;
  __asm volatile("dsb\n\tisb" ::: "memory");
  watchdog_hw->scratch[4] = 0;
#endif
}

void xno_flashfs_media_after_write(uint32_t flash_offset) {
  (void)flash_offset;
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  volatile uint32_t *vtable = (volatile uint32_t *)scb_hw->vtor;
  vtable[3] = g_saved_fault_vector;
#endif
  boot_trace(816);
}

// 消去・書込みそれぞれの前後。旧 flash_write の 812〜815 の位置に対応する。
void xno_flashfs_media_before_erase(uint32_t flash_offset) {
  (void)flash_offset;
  boot_trace(812);
}

void xno_flashfs_media_after_erase(uint32_t flash_offset) {
  (void)flash_offset;
  boot_trace(813);
}

void xno_flashfs_media_before_program(uint32_t flash_offset) {
  (void)flash_offset;
  boot_trace(814);
}

void xno_flashfs_media_after_program(uint32_t flash_offset) {
  (void)flash_offset;
  boot_trace(815);
}

// ---------------------------------------------------------------------------
//  診断 (旧 flash_fs.cpp の FS_PROG / BOOT_STAGE_TRACE / breadcrumb と同じ位置)
// ---------------------------------------------------------------------------

void xno_flashfs_media_breadcrumb(uint8_t entering) {
#if defined(SHIZUKU_CRASH_RECORD)
  shizuku::objects::usb_cdc_boot_diag_breadcrumb(
      entering ? shizuku::objects::boot_diag_breadcrumb::IRQ_DISABLE_ENTRY
               : shizuku::objects::boot_diag_breadcrumb::IRQ_RESTORE_EXIT);
#else
  (void)entering;
#endif
}

void xno_flashfs_media_trace_phase(uint32_t stage) { boot_trace(stage); }

void xno_flashfs_media_progress_mark(uint32_t stage, uint32_t flash_offset) {
  selftest_progress(stage, flash_offset);
}

// 旧 flash_fs は消去・書込みの時間の内訳を selftest の印字に使っていた。
// XNO 側の flash_fs.cpp が持つ計測の出口はここだけで、RP では何もしない
// (印字は旧 flash_fs の selftest が担当し、旧 flash_fs を消す回で扱う)。
void xno_flashfs_media_store_metrics(uint32_t total_us, uint32_t erase_us,
                                     uint32_t program_us) {
  (void)total_us;
  (void)erase_us;
  (void)program_us;
}
