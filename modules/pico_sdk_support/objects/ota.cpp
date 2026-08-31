// ===========================================================================
//  ota — BLE で受け取ってステージング領域へ置く / 本体へ移す
// ===========================================================================
#include "shizuku/objects/ota.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/ble_uart.hpp"
#include "shizuku/objects/inflate.hpp"
#include "shizuku/stream.hpp"
extern "C" {
#include "boot/picoboot_constants.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
}
#include <cstdint>
#include <cstdio>
#include <cstring>

// commit のコピー先。リンカから取得。
extern "C" {
extern uint8_t __flash_binary_start[];
extern uint8_t __flash_binary_end[];
}

namespace shizuku {
namespace objects {
namespace ota {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

uintptr_t export_method(method m, uintptr_t entry) {
  return api(object_api::EXPORT_METHOD, (uintptr_t)m, entry).error;
}

// 出口: 進捗・結果の行
stream::storage<frame_t, 8> g_out;
uintptr_t g_out_id = 0;

// 入口: ble_uart の OTA characteristic から来る生バイト
uintptr_t g_in_id = ble_uart::NO_STREAM;
stream::handle<frame_t> g_in;

uintptr_t g_ota_obj_id = 0;
uintptr_t g_status_sink_obj_id = 0;
uintptr_t g_ble_uart_obj_id = 0;

void say(const char *text) {
  frame_t f{};
  uint32_t n = (uint32_t)strlen(text);
  if (n > sizeof(f.data))
    n = sizeof(f.data);
  f.len = (uint16_t)n;
  memcpy(f.data, text, n);
  g_out.hdl().push(f);
  // ★UART0 は TX/RX が別線 (全二重) なので、host->device のバイナリ受信中
  //   (shizuku_shell の UART ブリッジ) でもこの行の送出は衝突しない。
  //   shizuku_shell が UART0 を初期化済みであることが前提 (起動順で保証)。
  if (uart_is_enabled(uart0)) {
    uart_puts(uart0, text);
  }
  BOARD::diag_printf("[OTA] %s", text);
}

// CRC32 (IEEE, 反転あり)
const uint32_t CRC_NIBBLE[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
    0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t length) {
  for (uint32_t i = 0; i < length; ++i) {
    crc ^= data[i];
    crc = (crc >> 4) ^ CRC_NIBBLE[crc & 0x0F];
    crc = (crc >> 4) ^ CRC_NIBBLE[crc & 0x0F];
  }
  return crc;
}

struct erase_op {
  uint32_t offset;
  uint32_t bytes;
};

struct program_op {
  uint32_t offset;
  const uint8_t *data;
  uint32_t bytes;
};

void erase_range(void *param) {
  const auto *op = (const erase_op *)param;
  ::flash_range_erase(op->offset, op->bytes);
}

void program_range(void *param) {
  const auto *op = (const program_op *)param;
  ::flash_range_program(op->offset, op->data, op->bytes);
}

using reboot_fn = int (*)(uint32_t, uint32_t, uint32_t, uint32_t);

struct commit_op {
  uint32_t src_offset;
  uint32_t dst_offset;
  uint32_t copy_sectors;
  uint32_t erase_blocks;
  uint8_t *buffer;
  reboot_fn reboot;
};

void __no_inline_not_in_flash_func(commit_blast)(void *param) {
  const volatile commit_op *op = (const volatile commit_op *)param;
  const uint32_t src_offset = op->src_offset;
  const uint32_t dst_offset = op->dst_offset;
  const uint32_t copy_sectors = op->copy_sectors;
  const uint32_t erase_blocks = op->erase_blocks;
  volatile uint32_t *const buffer = (volatile uint32_t *)op->buffer;
  const reboot_fn reboot = op->reboot;

  constexpr uint32_t SECTORS_PER_BLOCK = FLASH_BLOCK_SIZE / FLASH_SECTOR_SIZE;
  const uint32_t total_sectors = erase_blocks * SECTORS_PER_BLOCK;

  for (uint32_t s = 0; s < total_sectors; ++s) {
    const uint32_t at = s * FLASH_SECTOR_SIZE;

    // 1. 読む: XIP が生きている状態でステージングから SRAM バッファへ吸い出す (4KB)
    if (s < copy_sectors) {
      const volatile uint32_t *src =
          (const volatile uint32_t *)(XIP_BASE + src_offset + at);
      for (uint32_t i = 0; i < FLASH_SECTOR_SIZE / sizeof(uint32_t); ++i)
        buffer[i] = src[i];
    }

    // 2. 消す: 対象セクタを 4KB (FLASH_SECTOR_SIZE) 単位で消去
    ::flash_range_erase(dst_offset + at, FLASH_SECTOR_SIZE);

    // 3. 書く: コピー対象なら SRAM バッファの内容を 4KB 書き込む
    // (セクタ 0 書き込み完了と同時に boot2 が復元され、以降も XIP から読める)
    if (s < copy_sectors) {
      ::flash_range_program(dst_offset + at, (const uint8_t *)buffer,
                            FLASH_SECTOR_SIZE);
    }
  }

  // 4. 再起動: ROM reboot 関数を呼び出し
  if (reboot != nullptr) {
    reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS,
           10, 0, 0);
  }
  *((volatile uint32_t *)0xE000ED0C) = 0x05FA0004;
  while (true)
    __asm volatile("wfi");
}

enum struct state : uint32_t { IDLE, HEADER, DATA, ZLEN, ZDATA, DONE, FAILED };

constexpr uint32_t ZCHUNK_MAX = FLASH_SECTOR_SIZE + 256;

state g_state = state::IDLE;
uint8_t g_header[12];
// ★★「今フラッシュを触っている」を外へ見せる印。
//   消去・書き込みの間は IRQ を止め XIP も止まる。そこへ**他の口が喋る**と、
//   長い IRQ-off 中の BLE トラフィックが CYW43 の SPI/PIO を壊す (既知の罠)。
//
// ★★★**旗ではなく期限で持つこと**。最初は bool を RAII で上げ下げしたが、
//   転送が途中で失敗する経路でスコープを抜けず、**true のまま残って系全体が
//   永久に黙った** (2026-08-30 に踏んだ: BLE は繋がるのにテレメトリもシェルも
//   一切応答しない板ができた)。「黙る」の解除を、解除処理が走ることに賭けては
//   いけない —— 黙らせる側が死んでも、時間が経てば必ず喋り出す形にする。
//   期限は「最長の flash 操作 + 余裕」。64KB ブロック消去が数百 ms なので 2 秒。
constexpr uint64_t FLASH_QUIET_US = 2000000;
volatile uint64_t g_quiet_until_us = 0;

struct flash_quiet {
  flash_quiet() { g_quiet_until_us = KERNEL::BOARD::time_us() + FLASH_QUIET_US; }
  // ★抜けるときに**前倒しで**解除する。これは速く戻るための最適化であって、
  //   正しさは期限のほうが担保している (走らなくても 2 秒で戻る)。
  ~flash_quiet() { g_quiet_until_us = 0; }
};

uint32_t g_header_len = 0;
uint32_t g_total = 0;
uint32_t g_expect_crc = 0;
uint32_t g_received = 0;
uint32_t g_crc = 0xFFFFFFFFu;
uint8_t g_sector[FLASH_SECTOR_SIZE];
uint32_t g_sector_len = 0;
uint32_t g_last_report = 0;
uint32_t g_writes = 0;
uint32_t g_write_bytes = 0;
uint32_t g_last_write_report = 0;
uint32_t g_erased = 0;
uint64_t g_erase_us = 0;
uint64_t g_program_us = 0;
uint32_t g_erase_count = 0;
uint64_t g_start_us = 0;

bool g_compressed = false;
uint8_t g_zbuf[ZCHUNK_MAX];
uint8_t g_raw[FLASH_SECTOR_SIZE];
uint32_t g_zlen = 0;
uint32_t g_zgot = 0;
uint8_t g_zlen_bytes[2];
uint32_t g_zlen_got = 0;
uint64_t g_inflate_us = 0;

tiny_inflate::state g_inflate_state;

uint64_t g_last_byte_us = 0;
constexpr uint64_t IDLE_TIMEOUT_US =
    15000000ull; // 15秒 (CoreBluetooth 輻輳時の救済)

uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

void reset_transfer() {
  g_state = state::IDLE;
  g_header_len = 0;
  g_total = 0;
  g_expect_crc = 0;
  g_received = 0;
  g_crc = 0xFFFFFFFFu;
  g_sector_len = 0;
  g_last_report = 0;
  g_erased = 0;
  g_erase_us = 0;
  g_program_us = 0;
  g_erase_count = 0;
  g_start_us = 0;
  g_last_byte_us = BOARD::time_us();
  g_compressed = false;
  g_zlen = 0;
  g_zgot = 0;
  g_zlen_got = 0;
  g_inflate_us = 0;
}

// ★計測用の足跡。**kprintf は USB CDC へ出るので BLE には一切載らない** ——
//   だから OTA 中に使っても干渉を増やさない。しかも USB の列挙は割り込みだけで
//   回るので、スレッドが全部止まっても**最後の行は残る**。落ちる瞬間を掴む
//   唯一の窓なので、ここだけは残しておくこと。
#define OTA_TRACE(...) KERNEL::BOARD::diag_printf(__VA_ARGS__)

bool ensure_erased(uint32_t needed_bytes) {
  if (needed_bytes > STAGING_BYTES)
    return false;
  while (g_erased < needed_bytes) {
    uint32_t block_bytes = FLASH_BLOCK_SIZE;
    if (g_erased + block_bytes > STAGING_BYTES)
      block_bytes = STAGING_BYTES - g_erased;
    erase_op op{STAGING_OFFSET + g_erased, block_bytes};
    OTA_TRACE("[OTA] erase>  off=%lu bytes=%lu rx=%lu\n",
              (unsigned long)(STAGING_OFFSET + g_erased),
              (unsigned long)block_bytes, (unsigned long)g_received);
    const uint64_t t0 = BOARD::time_us();
    flash_quiet quiet;
    const auto res = ::flash_safe_execute(erase_range, &op, UINT32_MAX);
    const uint64_t elapsed = BOARD::time_us() - t0;
    OTA_TRACE("[OTA] erase<  rc=%d took=%lu us\n", (int)res,
              (unsigned long)elapsed);
    g_erase_us += elapsed;
    ++g_erase_count;
    if (res != PICO_OK) {
      say("erase failed\n");
      return false;
    }
    g_erased += block_bytes;
  }
  return true;
}

bool flush_sector() {
  if (g_sector_len == 0)
    return true;

  if (!ensure_erased(g_received))
    return false;

  const uint32_t sector_start = (g_received - g_sector_len);
  const uint32_t write_bytes =
      (g_sector_len + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);

  if (write_bytes > g_sector_len) {
    memset(g_sector + g_sector_len, 0xFF, write_bytes - g_sector_len);
  }

  // ★★1 セクタ (4KB) を一息に焼かず、**ページ (256B) ごとに区切って焼く**。
  //   一息に焼くと `flash_safe_execute` が **8ms 割り込みを止め** (実測
  //   took=8143us)、その間 CYW43 の SDIO 共有バスが取り残されて壊れる。
  //   実測でこうなった: 書き込み直後に
  //     [CYW43] Bus error condition detected 0xffff
  //     [ASSERT] host2bt_in_val < BTSDIO_FWBUF_SIZE (cybt_shared_bus_driver.c:527)
  //   → panic して系が止まり、OTA が転送の途中で必ず死んでいた。
  //   ページ単位なら窓は 1/16 (約 0.5ms) になり、**合間に poll を回せる**。
  //   ★合間の YIELD が要点。窓を縮めるだけでなく、実際に CYW43 を回してやらないと
  //     取り残しは解消しない (ble_uart の poll スレッドが cyw43_arch_poll を叩く)。
  const uint64_t t0 = BOARD::time_us();
  OTA_TRACE("[OTA] prog>   off=%lu bytes=%lu rx=%lu\n",
            (unsigned long)(STAGING_OFFSET + sector_start),
            (unsigned long)write_bytes, (unsigned long)g_received);
  int res = PICO_OK;
  uint64_t worst_page_us = 0;
  for (uint32_t at = 0; at < write_bytes; at += FLASH_PAGE_SIZE) {
    program_op op{STAGING_OFFSET + sector_start + at, g_sector + at,
                  FLASH_PAGE_SIZE};
    const uint64_t p0 = BOARD::time_us();
    {
      flash_quiet quiet;
      res = ::flash_safe_execute(program_range, &op, UINT32_MAX);
    }
    const uint64_t page_us = BOARD::time_us() - p0;
    if (page_us > worst_page_us)
      worst_page_us = page_us;
    if (res != PICO_OK)
      break;
    // ★ここで必ず譲る。譲らないとページに割っても CYW43 は回らない。
    api(object_api::YIELD);
  }
  const uint64_t elapsed = BOARD::time_us() - t0;
  g_program_us += elapsed;
  OTA_TRACE("[OTA] prog<   rc=%d took=%lu us (worst page %lu us)\n", res,
            (unsigned long)elapsed, (unsigned long)worst_page_us);

  if (res != PICO_OK) {
    say("program failed\n");
    return false;
  }
  g_sector_len = 0;
  return true;
}

void finish_upload() {
  if (!flush_sector()) {
    say("failed: flush\n");
    g_state = state::FAILED;
    return;
  }
  const uint32_t actual_crc = g_crc ^ 0xFFFFFFFFu;
  const uint64_t total_us = BOARD::time_us() - g_start_us;
  const uint32_t total_ms = (uint32_t)(total_us / 1000ull);
  const uint32_t erase_ms = (uint32_t)(g_erase_us / 1000ull);
  const uint32_t prog_ms = (uint32_t)(g_program_us / 1000ull);
  const uint32_t infl_ms = (uint32_t)(g_inflate_us / 1000ull);
  const int32_t link_ms = (int32_t)total_ms - (int32_t)erase_ms -
                          (int32_t)prog_ms - (int32_t)infl_ms;

  if (actual_crc != g_expect_crc) {
    char line[128];
    snprintf(line, sizeof(line),
             "crc MISMATCH: got=%08lx want=%08lx (%lu bytes, total %lums)\n",
             (unsigned long)actual_crc, (unsigned long)g_expect_crc,
             (unsigned long)g_received, (unsigned long)total_ms);
    say(line);
    g_state = state::FAILED;
    return;
  }

  char line[160];
  snprintf(line, sizeof(line),
           "done: %lu bytes crc=%08lx OK (staged at 0x%lx)\n",
           (unsigned long)g_received, (unsigned long)actual_crc,
           (unsigned long)STAGING_OFFSET);
  say(line);
  snprintf(line, sizeof(line),
           "time: %lums = erase %lu (%lu blk) + program %lu + inflate %lu + "
           "link %ld\n",
           (unsigned long)total_ms, (unsigned long)erase_ms,
           (unsigned long)g_erase_count, (unsigned long)prog_ms,
           (unsigned long)infl_ms, (long)link_ms);
  say(line);
  g_state = state::DONE;
}

// ★戻り値だけでなく**計算した値も返す**。成功時に何も言わないと、
//   「読み返し検査が本当に走ったのか」が外から分からない (走っていないのと
//   区別が付かない)。焼けたかを判断する材料なので、値ごと見せる。
uint32_t staged_crc(uint32_t total) {
  const uint8_t *staged = (const uint8_t *)(XIP_BASE + STAGING_OFFSET);
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t done = 0;
  while (done < total) {
    uint32_t n = total - done;
    if (n > 32 * 1024)
      n = 32 * 1024;
    crc = crc32_update(crc, staged + done, n);
    done += n;
    api(object_api::YIELD);
  }
  return crc ^ 0xFFFFFFFFu;
}

void reject_commit(const char *why) {
  char line[96];
  if (snprintf(line, sizeof(line), "commit rejected: %s\n", why) > 0)
    say(line);
  g_state = state::FAILED;
}

uint32_t sectors_for(uint32_t bytes) {
  return (bytes + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
}

void begin_commit() {
  const uint32_t total = read_le32(g_header + 4);
  const uint32_t want = read_le32(g_header + 8);
  const uint32_t dst_offset =
      (uint32_t)((uintptr_t)__flash_binary_start - XIP_BASE);
  const uint32_t old_bytes = (uint32_t)((uintptr_t)__flash_binary_end -
                                        (uintptr_t)__flash_binary_start);

  if (total == 0 || total > STAGING_BYTES) {
    reject_commit("size");
    return;
  }
  const uint32_t span = total > old_bytes ? total : old_bytes;
  const uint32_t erase_blocks =
      (span + FLASH_BLOCK_SIZE - 1) / FLASH_BLOCK_SIZE;
  if (dst_offset + erase_blocks * FLASH_BLOCK_SIZE > STAGING_OFFSET) {
    reject_commit("overlap");
    return;
  }

  const uint32_t *head = (const uint32_t *)(XIP_BASE + STAGING_OFFSET);
  if ((head[0] & 0xFF000000u) != SRAM_BASE ||
      (head[1] & 0xFF000000u) != XIP_BASE) {
    reject_commit("staged image does not start with a vector table");
    return;
  }

  say("verifying staged image before commit...\n");
  {
    const uint32_t got = staged_crc(total);
    char check[96];
    snprintf(check, sizeof(check), "readback crc=%08lx want=%08lx %s\n",
             (unsigned long)got, (unsigned long)want,
             got == want ? "OK" : "MISMATCH");
    say(check);
    if (got != want) {
      reject_commit("crc mismatch on readback");
      return;
    }
  }

  char line[128];
  const uint32_t sectors = sectors_for(total);
  snprintf(line, sizeof(line),
           "commit: %lu bytes -> 0x%lx (%lu sectors), no return\n",
           (unsigned long)total, (unsigned long)dst_offset,
           (unsigned long)sectors);
  say(line);

  // ★この行が実際に BLE から出るまで待つ (say → logger → notify は非同期)。
  //   出ないまま消し始めると、ホストからは黙って切れたようにしか見えない。
  api(object_api::SLEEP_US, 400000);

  // ★★焼く前に BLE を切る。commit は両コアを 1.7 秒止めるので、繋いだまま
  //   だと CYW43 の面倒を誰も見られない時間がそれだけ続く。
  if (g_ble_uart_obj_id != 0) {
    api(object_api::CALL_METHOD, g_ble_uart_obj_id,
        (uintptr_t)ble_uart::method::REQUEST_DISCONNECT, 0);
    api(object_api::SLEEP_US, 20000);
  }

  commit_op op{
      STAGING_OFFSET, dst_offset,
      sectors,        erase_blocks,
      g_sector,       (reboot_fn)rom_func_lookup_inline(ROM_FUNC_REBOOT)};

  flash_quiet quiet;
  const int rc = ::flash_safe_execute(commit_blast, &op, 10000);
  if (snprintf(line, sizeof(line), "commit could not start (rc=%d)\n", rc) > 0)
    say(line);
  g_state = state::FAILED;
}

void consume_header_12(const uint8_t *hdr) {
  g_total = read_le32(hdr + 4);
  g_expect_crc = read_le32(hdr + 8);
  g_received = 0;
  g_crc = 0xFFFFFFFFu;
  g_sector_len = 0;
  g_last_report = 0;
  g_writes = 0;
  g_write_bytes = 0;
  g_last_write_report = 0;
  g_erased = 0;
  g_erase_us = 0;
  g_program_us = 0;
  g_erase_count = 0;
  g_start_us = BOARD::time_us();

  if (hdr[0] == 'X' && hdr[1] == 'N' && hdr[2] == 'O' && hdr[3] == 'C') {
    begin_commit();
    return;
  }
  if (hdr[0] == 'X' && hdr[1] == 'N' && hdr[2] == 'O' && hdr[3] == 'Z') {
    g_compressed = true;
    g_state = state::ZLEN;
    g_zlen_got = 0;
    say("ready (compressed)\n");
    return;
  }
  if (hdr[0] == 'X' && hdr[1] == 'N' && hdr[2] == 'O' && hdr[3] == 'U') {
    g_compressed = false;
    g_state = state::DATA;
    say("ready\n");
    return;
  }

  say("unknown magic — upload を再開すること\n");
  g_state = state::FAILED;
}

void process_uncompressed_bytes(const uint8_t *p, uint32_t len) {
  g_crc = crc32_update(g_crc, p, len);
  while (len > 0) {
    const uint32_t space = sizeof(g_sector) - g_sector_len;
    const uint32_t take = len < space ? len : space;
    memcpy(g_sector + g_sector_len, p, take);
    g_sector_len += take;
    g_received += take;
    p += take;
    len -= take;
    if (g_sector_len == sizeof(g_sector)) {
      if (!flush_sector()) {
        say("failed: flush\n");
        g_state = state::FAILED;
        return;
      }
    }
  }
  if (g_received >= g_total) {
    finish_upload();
  }
}

void process_compressed_stream(const uint8_t *p, uint32_t len) {
  while (len > 0 && g_state != state::FAILED && g_state != state::DONE) {
    if (g_state == state::ZLEN) {
      g_zlen_bytes[g_zlen_got++] = *p++;
      --len;
      if (g_zlen_got == 2) {
        g_zlen = (uint32_t)g_zlen_bytes[0] | ((uint32_t)g_zlen_bytes[1] << 8);
        g_zgot = 0;
        g_zlen_got = 0;
        if (g_zlen == 0 || g_zlen > sizeof(g_zbuf)) {
          say("zchunk size invalid\n");
          g_state = state::FAILED;
          return;
        }
        g_state = state::ZDATA;
      }
      continue;
    }

    if (g_state == state::ZDATA) {
      const uint32_t need = g_zlen - g_zgot;
      const uint32_t take = len < need ? len : need;
      memcpy(g_zbuf + g_zgot, p, take);
      g_zgot += take;
      p += take;
      len -= take;
      if (g_zgot == g_zlen) {
        const uint64_t t0 = BOARD::time_us();
        const int32_t raw_bytes = tiny_inflate::run(
            g_inflate_state, g_zbuf, g_zlen, g_raw, sizeof(g_raw));
        g_inflate_us += (BOARD::time_us() - t0);

        if (raw_bytes <= 0) {
          say("inflate failed\n");
          g_state = state::FAILED;
          return;
        }

        process_uncompressed_bytes(g_raw, (uint32_t)raw_bytes);
        if (g_state != state::FAILED && g_state != state::DONE)
          g_state = state::ZLEN;
      }
    }
  }
}

void feed(const uint8_t *data, uint32_t len) {
  g_last_byte_us = BOARD::time_us();
  if (g_state == state::IDLE) {
    g_header_len = 0;
    g_state = state::HEADER;
  }
  if (g_state == state::HEADER) {
    while (len > 0 && g_header_len < sizeof(g_header)) {
      g_header[g_header_len++] = *data++;
      --len;
    }
    if (g_header_len == sizeof(g_header)) {
      consume_header_12(g_header);
    }
  }
  if (len == 0)
    return;

  if (g_state == state::DATA) {
    const uint32_t remain = g_total - g_received;
    const uint32_t take = len < remain ? len : remain;
    process_uncompressed_bytes(data, take);
  } else if (g_state == state::ZLEN || g_state == state::ZDATA) {
    process_compressed_stream(data, len);
  }
}

uintptr_t method_set_input_stream(uintptr_t stream_id, uintptr_t, uintptr_t,
                                  uintptr_t) {
  g_in_id = stream_id;
  const auto opened = api(object_api::STREAM_OPEN, g_in_id);
  if (opened.error == 0 && opened.value != 0) {
    g_in = stream::handle<frame_t>((stream::descriptor *)opened.value);
    api(object_api::STREAM_BIND, g_in_id, (uintptr_t)stream::role::CONSUMER);
  }
  BOARD::diag_printf("[OTA] in stream attached (%lu)\n",
                     (unsigned long)g_in_id);
  return 0;
}

uintptr_t method_get_stream(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return g_out_id;
}

uintptr_t poll_loop(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  while (true) {
    api(object_api::YIELD);
    if (!g_in.valid())
      continue;

    frame_t f{};
    uint32_t lost = 0;
    while (g_in.pop(&f, &lost)) {
      if (lost != 0) {
        say("input overrun — 転送をやり直すこと\n");
        reset_transfer();
        g_state = state::FAILED;
        continue;
      }
      ++g_writes;
      g_write_bytes += f.len;
      feed(f.data, f.len);
      if (g_state == state::DATA && g_received - g_last_report >= 8 * 1024) {
        g_last_report = g_received;
        char line[80];
        if (snprintf(line, sizeof(line), "%lu / %lu bytes\n",
                     (unsigned long)g_received, (unsigned long)g_total) > 0)
          say(line);
      }
    }
    if (g_state != state::IDLE && g_state != state::DONE &&
        g_state != state::FAILED &&
        BOARD::time_us() - g_last_byte_us > IDLE_TIMEOUT_US) {
      char line[96];
      if (snprintf(line, sizeof(line),
                   "timed out at %lu / %lu bytes — 捨てて待ち受けに戻る\n",
                   (unsigned long)g_received, (unsigned long)g_total) > 0)
        say(line);
      reset_transfer();
    }

    if (g_writes != g_last_write_report) {
      static uint64_t next_note = 0;
      const uint64_t now = BOARD::time_us();
      if (now >= next_note) {
        next_note = now + 2000000ull;
        g_last_write_report = g_writes;
        char line[96];
        if (snprintf(line, sizeof(line),
                     "rx %lu writes / %lu bytes (state %lu)\n",
                     (unsigned long)g_writes, (unsigned long)g_write_bytes,
                     (unsigned long)g_state) > 0)
          say(line);
      }
    }
    if (g_state == state::FAILED || g_state == state::DONE) {
      reset_transfer();
    }
  }
  return 0;
}

uintptr_t ota_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  uintptr_t failures = api(object_api::DECLARE_NAME, (uintptr_t)"ota").error;
  failures += export_method(method::SET_INPUT_STREAM,
                            (uintptr_t)&method_set_input_stream);
  failures += export_method(method::GET_STREAM, (uintptr_t)&method_get_stream);
  failures += export_method(method::POLL, (uintptr_t)&poll_loop);

  g_out.init();
  const auto created = api(object_api::STREAM_CREATE, (uintptr_t)&g_out.desc);
  failures += created.error;
  g_out_id = created.value;
  return failures;
}

} // namespace

// フラッシュを触っている最中か。★他の口 (シェル等) はこの間**喋らないこと**。
//   ★期限で判断するので、解除処理が走らなくても必ず false に戻る。
bool flash_busy() { return KERNEL::BOARD::time_us() < g_quiet_until_us; }

uint32_t register_ota(uintptr_t object_id, uintptr_t status_sink_obj_id,
                      uintptr_t ble_uart_obj_id) {
  g_ota_obj_id = object_id;
  g_status_sink_obj_id = status_sink_obj_id;
  g_ble_uart_obj_id = ble_uart_obj_id;
  const auto created = api(object_api::CREATE_OBJECT, g_ota_obj_id,
                           (uintptr_t)&ota_main, OBJECT_PRIVILEGED);
  const auto started = api(object_api::CALL_METHOD, g_ota_obj_id, 0, 0);
  if (created.error != 0 || started.error != 0 || started.value != 0) {
    BOARD::diag_printf("[OTA] FAILED: create=%lu call=%lu exports_failed=%lu\n",
                       (unsigned long)created.error,
                       (unsigned long)started.error,
                       (unsigned long)started.value);
    return 1;
  }
  BOARD::diag_printf("[OTA] registered (object %lu, stream %lu)\n",
                     (unsigned long)g_ota_obj_id, (unsigned long)g_out_id);
  return 0;
}

uint32_t start_ota(uintptr_t object_id) {
  const auto spawned =
      api(object_api::SPAWN, object_id, (uintptr_t)method::POLL, 0);
  if (spawned.error != 0) {
    BOARD::diag_printf("[OTA] could not spawn (%lu)\n",
                       (unsigned long)spawned.error);
    return 1;
  }
  BOARD::diag_printf("[OTA] thread %lu started\n",
                     (unsigned long)spawned.value);
  return 0;
}

} // namespace ota
} // namespace objects
} // namespace shizuku
