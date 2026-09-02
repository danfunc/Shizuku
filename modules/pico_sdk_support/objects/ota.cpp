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

// ★新しい値は**末尾へ足すこと**。GET_STATE は列挙値をそのまま数値で外
//   (シェル) へ渡しており、途中に挿すと DONE/FAILED の番号がずれる。
//   Pico とシェルは同じ像に入っているので即死はしないが、片方だけ古いログや
//   ドキュメントを見ている人間が必ず騙される。
enum struct state : uint32_t {
  IDLE,
  HEADER,
  DATA,
  ZLEN,
  ZDATA,
  DONE,
  FAILED,
  // 以下 XNOR (チャンク単位の再送) 専用。
  CSEEK, // チャンクヘッダの magic を探している。**ラウンドの合間もここ**
  CHDR,  // 16B のチャンクヘッダを集めている
  CDATA, // チャンクのペイロードを集めている
};

constexpr uint32_t ZCHUNK_MAX = FLASH_SECTOR_SIZE + 256;

// ===========================================================================
//  XNOR — チャンク単位の再送 (HTTP の range 再取得と同じ発想)
// ===========================================================================
//  送り手は最後まで**一括で送り切り**、受け手 (ここ) は化けたチャンクを
//  記録するだけで転送を殺さない。送り終わったら送り手が「足りない seq」を
//  問い合わせ、その分だけ再送する。窓幅・順序・タイムアウト再送といった
//  パイプライン方式の状態機械が要らないのが利点。
//
//  ★★従来の XNOZ (`[u16 len][deflate]` の裸の連続) では**この方式が成立
//    しない**。len の 1 ビットが化けると以降のフレーム境界が全部ずれるので、
//    「化けた seq だけ再送」が実際には「最初の 1 個が化けたら以降全部」へ
//    退化する。だからチャンクごとに**自己同期できるヘッダ**を付ける:
//
//      [u32 'XNCK'][u16 seq][u16 len][u32 crc32(payload)][u16 rsv][u16 crc16(先頭14B)]
//
//    受け手は len を信用する前に crc16 でヘッダを検め、壊れていれば
//    1 バイトずつずらして magic を探し直す (CSEEK)。上乗せは 16B/4096B
//    = 0.39% (300KB で約 0.04 秒)。
//
//  ★seq == QUERY_SEQ (0xFFFF) かつ len == 0 のヘッダは「今どれが足りないか
//    教えろ」という問い合わせ。**チャンクと同じ枠に載せてある**ので、
//    CSEEK の探索も crc16 の保護もそのまま効き、BLE でも UART でも同じ
//    入口を通る (別経路・別パーサを作らずに済む)。
//
//  ★チャンク 1 個 = セクタ 1 個 (ZCHUNK == FLASH_SECTOR_SIZE == flash の
//    消去最小単位) が綺麗に一致するので、再送は「そのセクタを消して書き
//    直す」だけで済む。ここが噛み合っているのは設計の幸運なので、
//    ZCHUNK を変えるときは必ずこの前提を見直すこと。
//
//  ★★XNOZ の経路には**一切手を入れていない**。回復経路を残すため —
//    新形式にバグがあっても、古い送り手 (XNOZ) でこの板へ焼き直せる。

constexpr uint8_t CHUNK_MAGIC[4] = {'X', 'N', 'C', 'K'};
constexpr uint32_t CHUNK_HDR_BYTES = 16;
constexpr uint32_t QUERY_SEQ = 0xFFFFu;
// ★「今の転送を捨てて待ち受けに戻れ」。失敗して諦めた campaign のあと、
//   ota は CSEEK のまま最大 2 分居座る (ラウンドの合間を守るための長い
//   タイムアウト)。その間に次の転送を始めると、**XNOR ファイルヘッダが
//   チャンクデータとして食われて**先へ進まない。送り手が campaign の頭で
//   これを撃てるようにしておく。
constexpr uint32_t RESET_SEQ = 0xFFFEu;
// ステージング領域に入るチャンクの上限 (Pico 2 W = 4MB flash なら 509)。
constexpr uint32_t MAX_CHUNKS = STAGING_BYTES / FLASH_SECTOR_SIZE;
constexpr uint32_t BM_WORDS = (MAX_CHUNKS + 31) / 32;

bool g_chunked = false;
uint32_t g_nchunks = 0;
uint32_t g_ok_bm[BM_WORDS]; // 検証を通って flash へ書けた seq
uint8_t g_chdr[CHUNK_HDR_BYTES];
uint32_t g_chdr_got = 0;
uint8_t g_cwin[4]; // CSEEK 用の magic 探索窓
uint32_t g_cwin_filled = 0;
uint32_t g_cseq = 0;
uint32_t g_clen = 0;
uint32_t g_ccrc = 0;
uint32_t g_cgot = 0;
uint32_t g_chunks_ok = 0;  // 診断用: 受理した数 (再送ぶんの重複は数えない)
uint32_t g_chunks_bad = 0; // 診断用: ヘッダ破損 + ペイロード破損の累計
uint32_t g_queries = 0;    // 診断用: 問い合わせを受けた回数 = ラウンド数

bool bm_get(const uint32_t *bm, uint32_t i) {
  return ((bm[i >> 5] >> (i & 31)) & 1u) != 0;
}
void bm_set(uint32_t *bm, uint32_t i) { bm[i >> 5] |= 1u << (i & 31); }

// CRC-16/CCITT-FALSE。★16 バイトのヘッダにしか掛けないので、表を持たず
//   ビットで回して十分 (テーブル 512B を積むほうがもったいない)。
uint16_t crc16_ccitt(const uint8_t *p, uint32_t n) {
  uint16_t crc = 0xFFFFu;
  for (uint32_t i = 0; i < n; ++i) {
    crc ^= (uint16_t)((uint16_t)p[i] << 8);
    for (uint32_t b = 0; b < 8; ++b)
      crc = (crc & 0x8000u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u)
                            : (uint16_t)(crc << 1);
  }
  return crc;
}

uint16_t read_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

state g_state = state::IDLE;
// ★reset_transfer() では触らない。DONE/FAILED から IDLE へ落ちる一瞬にしか
//   確定しない判定を、次の転送が始まるまで持ち越すための場所 (method::GET_LAST_OK)。
bool g_last_ok = false;
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
// ★チャンク再送 (XNOR) のラウンドの合間は 15 秒では短すぎる。OTW では中継を
//   いったん畳んで baud を 115200 へ戻し、母艦が NEED を読んでチャンクを
//   詰め直し、もう一度 ubridge を張り直す (2 回連続ブリッジの実用対処だけで
//   3 秒待つ)。ここで転送を捨てると**受領ビットマップごと消えて全再送**に
//   なり、再送機構そのものの意味が無くなる。
constexpr uint64_t CHUNKED_IDLE_TIMEOUT_US = 120000000ull; // 2分

// ★「ota が今 feed() の中にいる」。中継が baud を戻してよいかの判断に使う。
//   ストリームの available()==0 は「積んだ分は pop された」しか意味せず、
//   pop の中で走る flash 書き込みや完了行の送出までは保証しない
//   (GET_STATE を足したときと同じ話、method::GET_QUIESCENT を参照)。
volatile bool g_feeding = false;

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
  // ★ビットマップは XNOR ヘッダの受領時に消す。ここで消さないのは、
  //   reset_transfer() が「ラウンドの合間」には走らないことを前提に
  //   していないため — 走ってしまったら g_chunked が false になるので、
  //   次のラウンドは NEED n=0 (転送が走っていない) と正直に答える。
  g_chunked = false;
  g_nchunks = 0;
  g_cwin_filled = 0;
  g_chdr_got = 0;
}

// ★計測用の足跡。**kprintf は USB CDC へ出るので BLE には一切載らない** ——
//   だから OTA 中に使っても干渉を増やさない。しかも USB の列挙は割り込みだけで
//   回るので、スレッドが全部止まっても**最後の行は残る**。落ちる瞬間を掴む
//   唯一の窓なので、ここだけは残しておくこと。
#define OTA_TRACE(...) KERNEL::BOARD::diag_printf(__VA_ARGS__)

bool ensure_erased(uint32_t needed_bytes) {
  if (needed_bytes > STAGING_BYTES)
    return false;
  // ★★64KB ブロックを 1 回の flash_safe_execute でまとめて消していたら
  //   1 回あたり約 108ms 止まっていた (2026-09-02 実測、以前のメモに
  //   「対策済み」と書かれていた値は単位が us と誤記されていた可能性が高い —
  //   同じ 108ms/block の値と一致する)。flush_sector() が書き込みを 256B
  //   ページへ分割して直したのと同じ理由で、こちらも消去の最小単位
  //   (FLASH_SECTOR_SIZE = 4KB、これより小さくは消せない) へ割り、
  //   合間で必ず YIELD する。窓を縮めるだけでなく実際に譲ることが要点
  //   (flush_sector() と同じ)。
  while (g_erased < needed_bytes) {
    uint32_t block_bytes = FLASH_SECTOR_SIZE;
    if (g_erased + block_bytes > STAGING_BYTES)
      block_bytes = STAGING_BYTES - g_erased;
    erase_op op{STAGING_OFFSET + g_erased, block_bytes};
    OTA_TRACE("[OTA] erase>  off=%lu bytes=%lu rx=%lu\n",
              (unsigned long)(STAGING_OFFSET + g_erased),
              (unsigned long)block_bytes, (unsigned long)g_received);
    const uint64_t t0 = BOARD::time_us();
    int res;
    {
      flash_quiet quiet;
      res = ::flash_safe_execute(erase_range, &op, UINT32_MAX);
    }
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
    // ★flush_sector() のページループと同じ規律: ロックの外で必ず譲る。
    api(object_api::YIELD);
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

// ---- XNOR: 1 チャンク = 1 セクタを消して書く ------------------------------
// ★★再送では**必ず消してから**書く。一度書いた flash は消さずに書き直せない。
//   毎回消すので「初回か再送か」を覚えておく必要がなく、状態が 1 つ減る。
//   代償は初回も必ず消去が入ることだが、ensure_erased() も結局全域を消して
//   いたので**総消去量は変わらない** (2026-09-02 実測の erase 4284ms /109 blk
//   と同じ仕事量)。
// ★消去は 4KB 単位、書き込みは 256B ページ単位に割り、合間で必ず YIELD する。
//   窓を縮めるだけでは足りず、実際に譲って CYW43 を回さないと取り残しは
//   解消しない ([[long-irq-off-wedges-cyw43]]、flush_sector() と同じ規律)。
// ★g_sector を作業バッファとして借りる。XNOZ 経路と同時には動かない
//   (magic が排他) が、**借り物であることは意識しておくこと**。
bool write_sector(uint32_t seq, const uint8_t *data, uint32_t len) {
  const uint32_t at = seq * FLASH_SECTOR_SIZE;
  if (len > FLASH_SECTOR_SIZE || at + FLASH_SECTOR_SIZE > STAGING_BYTES)
    return false;

  {
    erase_op op{STAGING_OFFSET + at, FLASH_SECTOR_SIZE};
    const uint64_t t0 = BOARD::time_us();
    int res;
    {
      flash_quiet quiet;
      res = ::flash_safe_execute(erase_range, &op, UINT32_MAX);
    }
    g_erase_us += BOARD::time_us() - t0;
    ++g_erase_count;
    if (res != PICO_OK) {
      say("erase failed\n");
      return false;
    }
    api(object_api::YIELD);
  }

  memcpy(g_sector, data, len);
  const uint32_t write_bytes =
      (len + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
  if (write_bytes > len)
    memset(g_sector + len, 0xFF, write_bytes - len);

  const uint64_t t0 = BOARD::time_us();
  int res = PICO_OK;
  for (uint32_t off = 0; off < write_bytes; off += FLASH_PAGE_SIZE) {
    program_op op{STAGING_OFFSET + at + off, g_sector + off, FLASH_PAGE_SIZE};
    {
      flash_quiet quiet;
      res = ::flash_safe_execute(program_range, &op, UINT32_MAX);
    }
    if (res != PICO_OK)
      break;
    api(object_api::YIELD); // ★ここで譲らないとページに割った意味が無い
  }
  g_program_us += BOARD::time_us() - t0;
  if (res != PICO_OK) {
    say("program failed\n");
    return false;
  }
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

// ---- XNOR: 足りないチャンクを報告する (= ラウンドの区切り) ----------------
// ★★ここが「送り手が次に何をすべきか」を決める唯一の出口。出力の形:
//     NEED n=<足りない数> (of <総数>, ok=.. bad=.. round=..)
//     NEEDSEQ 3,17,42            ← n>0 のとき、必要なだけ複数行
//     NEEDEND
//   n==0 のときは NEEDSEQ を出さずに、そのまま**flash の読み返し CRC**まで
//   走らせて done: / crc MISMATCH: を出す。
// ★★判定に到着順のストリーミング CRC を使わない。順不同の再送と両立しない
//   のが直接の理由だが、**flash の実体を検べるほうが本質的に強い** —
//   「RAM を通ったバイト列は正しかった」ではなく「焼けているものが正しい」を
//   言えるので、commit 前の readback (begin_commit) と同じ土俵に乗る。
uint32_t report_missing() {
  char line[200];
  if (!g_chunked || g_nchunks == 0) {
    // ★★"NEED n=0" を使わないこと。あれは「全チャンク揃った」を意味する
    //   ので、**転送が走っていないという失敗**を成功として読ませてしまう。
    //   実際に踏んだ (2026-09-02): 転送が FAILED で畳まれた後の問い合わせに
    //   この行を返し、母艦が「全チャンク受領」と報告して commit へ進んだ。
    //   ★機械が読む行に日本語を混ぜないこと。XIAO の行組み立ては
    //     32..=126 しか通さないので、**日本語は途中で黙って消える**。
    //     消えた結果 "NEED n=0 ()" になり、n=0 として解釈された。
    say("NEEDIDLE no transfer in progress\n");
    say("NEEDEND\n");
    return 0;
  }
  ++g_queries;

  uint32_t missing = 0;
  for (uint32_t i = 0; i < g_nchunks; ++i)
    if (!bm_get(g_ok_bm, i))
      ++missing;

  // ★短く保つこと (上の FIFO の話は見出し行にも同じく効く。45 文字の 1 行
  //   ですら 115200 で 3.9ms かかり、相手の 2.8ms を超える)。
  snprintf(line, sizeof(line), "NEED n=%lu of=%lu ok=%lu bad=%lu r=%lu\n",
           (unsigned long)missing, (unsigned long)g_nchunks,
           (unsigned long)g_chunks_ok, (unsigned long)g_chunks_bad,
           (unsigned long)g_queries);
  say(line);
  api(object_api::SLEEP_US, 30000);

  if (missing == 0) {
    const uint32_t got = staged_crc(g_total);
    const uint64_t total_us = BOARD::time_us() - g_start_us;
    const uint32_t total_ms = (uint32_t)(total_us / 1000ull);
    const uint32_t erase_ms = (uint32_t)(g_erase_us / 1000ull);
    const uint32_t prog_ms = (uint32_t)(g_program_us / 1000ull);
    const uint32_t infl_ms = (uint32_t)(g_inflate_us / 1000ull);
    const int32_t link_ms = (int32_t)total_ms - (int32_t)erase_ms -
                            (int32_t)prog_ms - (int32_t)infl_ms;
    if (got != g_expect_crc) {
      snprintf(line, sizeof(line),
               "crc MISMATCH: got=%08lx want=%08lx (%lu bytes, total %lums)\n",
               (unsigned long)got, (unsigned long)g_expect_crc,
               (unsigned long)g_total, (unsigned long)total_ms);
      say(line);
      // ★全チャンクが個別の crc32 を通ったのにここで外れる = 化けではなく
      //   flash へ書けていない (program の取りこぼし等)。再送しても直らない
      //   ので、素直に失敗として畳む。
      say("NEEDEND\n");
      g_state = state::FAILED;
      return 0;
    }
    snprintf(line, sizeof(line),
             "done: %lu bytes crc=%08lx OK (staged at 0x%lx)\n",
             (unsigned long)g_total, (unsigned long)got,
             (unsigned long)STAGING_OFFSET);
    say(line);
    snprintf(line, sizeof(line),
             "time: %lums = erase %lu (%lu blk) + program %lu + inflate %lu + "
             "link %ld\n",
             (unsigned long)total_ms, (unsigned long)erase_ms,
             (unsigned long)g_erase_count, (unsigned long)prog_ms,
             (unsigned long)infl_ms, (long)link_ms);
    say(line);
    // ★終端は成否によらず必ず出す。母艦は「NEEDEND が見えたか」だけで
    //   「返事が全部届いた」を判定する — 経路ごとに終端が違うと、一覧が
    //   途中で切れたのか正常に終わったのかを区別できない。
    say("NEEDEND\n");
    g_state = state::DONE;
    return 0;
  }

  // 一覧は**範囲**で書く ("40-59,98-108")。ビット化けは連続したチャンクを
  // まとめて落とすので、範囲にすると劇的に短くなる。
  // ★★短くするのは見た目のためではない。**XIAO の UART RX は FIFO 32B の
  //   ポーリングで、115200 では 2.8ms で溢れる** (Pico 側が DMA 化して解決した
  //   のと同じ問題が、XIAO 側には残っている)。長い行を続けて流すと途中が
  //   落ちて一覧が欠け、送り手は足りない seq を知らないまま再送するので
  //   **永久に収束しない**。2026-09-02 実機で踏んだ: n=70 と言いながら
  //   11 個しか届かず、再送しても ok が increase しなかった。
  // ★1 行ごとに実際に**寝る**こと。YIELD は他に走るものが無ければすぐ戻る
  //   ので、相手が汲む時間を作れない。
  constexpr uint32_t LINE_GAP_US = 30000;
  uint32_t at = 0;
  while (at < g_nchunks) {
    int n = snprintf(line, sizeof(line), "NEEDSEQ");
    bool any = false;
    // 1 行あたりの範囲数を絞る (相手の FIFO に合わせて短く保つ)。
    for (uint32_t ranges = 0; ranges < 8 && at < g_nchunks; ++ranges) {
      while (at < g_nchunks && bm_get(g_ok_bm, at))
        ++at;
      if (at >= g_nchunks)
        break;
      const uint32_t start = at;
      while (at < g_nchunks && !bm_get(g_ok_bm, at))
        ++at;
      const uint32_t last = at - 1;
      int add;
      if (last == start)
        add = snprintf(line + n, sizeof(line) - (size_t)n, "%s%lu",
                       any ? "," : " ", (unsigned long)start);
      else
        add = snprintf(line + n, sizeof(line) - (size_t)n, "%s%lu-%lu",
                       any ? "," : " ", (unsigned long)start,
                       (unsigned long)last);
      if (add <= 0 || n + add >= (int)sizeof(line) - 2) {
        at = start; // この行には入らない。次の行へ回す
        break;
      }
      n += add;
      any = true;
    }
    if (!any)
      break;
    snprintf(line + n, sizeof(line) - (size_t)n, "\n");
    say(line);
    api(object_api::SLEEP_US, LINE_GAP_US);
  }
  api(object_api::SLEEP_US, LINE_GAP_US);
  say("NEEDEND\n");
  return missing;
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
  // ★待ち受け (IDLE) の最中にチャンク枠が来た。制御フレーム (問い合わせ /
  //   リセット) を**転送が走っていないときにも撃てる**ようにするための受け口。
  //   12B だけ先に食ってしまっているので、そのまま 16B ヘッダの途中として
  //   引き継ぐ (残り 4B は続けて流れてくる)。これが無いと、送り手は
  //   「相手が IDLE か CSEEK か」を知らないと制御フレームを撃てなくなる。
  if (hdr[0] == 'X' && hdr[1] == 'N' && hdr[2] == 'C' && hdr[3] == 'K') {
    memcpy(g_chdr, hdr, 12);
    g_chdr_got = 12;
    g_cwin_filled = 0;
    g_state = state::CHDR;
    return;
  }
  if (hdr[0] == 'X' && hdr[1] == 'N' && hdr[2] == 'O' && hdr[3] == 'R') {
    g_compressed = true;
    g_chunked = true;
    g_nchunks = (g_total + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
    if (g_total == 0 || g_nchunks > MAX_CHUNKS) {
      say("size out of range\n");
      g_state = state::FAILED;
      return;
    }
    memset(g_ok_bm, 0, sizeof(g_ok_bm));
    g_chunks_ok = 0;
    g_chunks_bad = 0;
    g_queries = 0;
    g_cwin_filled = 0;
    g_chdr_got = 0;
    g_state = state::CSEEK;
    // ★ここで全域を消さない。消去はチャンクを書く直前へ移した
    //   (write_sector)。先に全部消すと、実際には送られてこない末尾まで
    //   消すことになるうえ、再送のたびに「消し済みで未書き込み」のセクタを
    //   覚える羽目になる。毎回消せばその記憶が要らない。
    say("ready (chunked)\n");
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

// ---- XNOR: チャンク列を読む -----------------------------------------------
void accept_chunk() {
  // 期待する展開後の長さ。最後のチャンクだけ端数になる。
  const uint32_t at = g_cseq * FLASH_SECTOR_SIZE;
  const uint32_t want_raw =
      (g_total - at) < FLASH_SECTOR_SIZE ? (g_total - at) : FLASH_SECTOR_SIZE;

  const uint64_t t0 = BOARD::time_us();
  const int32_t raw_bytes =
      tiny_inflate::run(g_inflate_state, g_zbuf, g_clen, g_raw, sizeof(g_raw));
  g_inflate_us += BOARD::time_us() - t0;

  if (raw_bytes <= 0 || (uint32_t)raw_bytes != want_raw) {
    // ★crc32 を通ったのに展開できない / 長さが合わない = ビット化けではなく
    //   送り手と受け手の作りが食い違っている。再送しても直らないが、
    //   **ここでも転送は殺さない** — 欠損として記録して NEED に出し、送り手の
    //   上限回数で諦めさせる。転送を殺すと「何が起きたか」を問い合わせる口
    //   ごと消えてしまい、原因が分からないまま終わるため。
    ++g_chunks_bad;
    return;
  }
  if (!write_sector(g_cseq, g_raw, (uint32_t)raw_bytes)) {
    g_state = state::FAILED; // flash が焼けないのは本物の失敗
    return;
  }
  if (!bm_get(g_ok_bm, g_cseq)) {
    bm_set(g_ok_bm, g_cseq);
    ++g_chunks_ok;
  }
}

void process_chunked_stream(const uint8_t *p, uint32_t len) {
  while (len > 0 && g_state != state::FAILED && g_state != state::DONE) {
    if (g_state == state::CSEEK) {
      // magic を 1 バイトずつずらして探す。★ヘッダが化けたら len を信用でき
      //   ないので、次のチャンクの頭は「探す」以外に見つけようがない。
      g_cwin[0] = g_cwin[1];
      g_cwin[1] = g_cwin[2];
      g_cwin[2] = g_cwin[3];
      g_cwin[3] = *p++;
      --len;
      // ★★窓が満ちた**その回に**照合すること。「満ちるまで continue」に
      //   すると 4 バイト目を入れた回を飛ばしてしまい、その窓は次のバイトで
      //   先頭が押し出されて二度と一致しない。結果、**チャンクを 1 個おきに
      //   取り逃がす** (無傷の入力でも毎ラウンド半分しか受理されず、ラウンド
      //   ごとに欠損が半減していくだけで収束しない)。ホスト側に受信部を
      //   写して無傷入力を流す試験で捕まえた。
      if (g_cwin_filled < 4)
        ++g_cwin_filled;
      if (g_cwin_filled < 4)
        continue;
      if (g_cwin[0] != CHUNK_MAGIC[0] || g_cwin[1] != CHUNK_MAGIC[1] ||
          g_cwin[2] != CHUNK_MAGIC[2] || g_cwin[3] != CHUNK_MAGIC[3])
        continue;
      memcpy(g_chdr, CHUNK_MAGIC, 4);
      g_chdr_got = 4;
      g_state = state::CHDR;
      continue;
    }

    if (g_state == state::CHDR) {
      while (len > 0 && g_chdr_got < CHUNK_HDR_BYTES) {
        g_chdr[g_chdr_got++] = *p++;
        --len;
      }
      if (g_chdr_got < CHUNK_HDR_BYTES)
        return;
      g_cseq = read_le16(g_chdr + 4);
      g_clen = read_le16(g_chdr + 6);
      g_ccrc = read_le32(g_chdr + 8);
      const bool hdr_ok =
          crc16_ccitt(g_chdr, CHUNK_HDR_BYTES - 2) == read_le16(g_chdr + 14);

      g_cwin_filled = 0;
      g_chdr_got = 0;

      // 問い合わせ (seq=QUERY_SEQ, len=0)。★チャンクと同じ枠に載せてあるので、
      //   CSEEK の探索も crc16 の保護もそのまま効く。
      if (hdr_ok && g_cseq == QUERY_SEQ && g_clen == 0) {
        report_missing();
        if (g_state != state::FAILED && g_state != state::DONE)
          g_state = state::CSEEK;
        continue;
      }
      if (hdr_ok && g_cseq == RESET_SEQ && g_clen == 0) {
        say("reset\n");
        reset_transfer(); // g_state は IDLE へ戻る
        // ★★このフレームの**残りは捨てる**。feed() の header 段はもう
        //   通り過ぎているので、ここから先を続けて読ませる道が無い。
        //   したがって送り手は **RESET を書き込み単位の最後に置くこと**
        //   (BLE なら 16B を単独で write する)。UART の中継のように
        //   バイト列が連続する経路では、代わりに**中継の外**からシェルの
        //   OTARESET を使うこと (そちらはそもそもこの経路を通らない)。
        return;
      }
      if (!hdr_ok || g_cseq >= g_nchunks || g_clen == 0 ||
          g_clen > sizeof(g_zbuf)) {
        // ヘッダが壊れている、またはデータ中に偶然並んだ 'XNCK'。
        // ★探索をやり直すだけ。**16 バイト食ってしまった分、本物のヘッダを
        //   飛び越える可能性がある**が、その場合はそのチャンクが NEED に出て
        //   次のラウンドで拾える。取りこぼしても壊れないほうを選ぶ。
        ++g_chunks_bad;
        g_state = state::CSEEK;
        continue;
      }
      g_cgot = 0;
      g_state = state::CDATA;
      continue;
    }

    if (g_state == state::CDATA) {
      const uint32_t need = g_clen - g_cgot;
      const uint32_t take = len < need ? len : need;
      memcpy(g_zbuf + g_cgot, p, take);
      g_cgot += take;
      p += take;
      len -= take;
      if (g_cgot < g_clen)
        return;
      const uint32_t crc =
          crc32_update(0xFFFFFFFFu, g_zbuf, g_clen) ^ 0xFFFFFFFFu;
      if (crc != g_ccrc)
        ++g_chunks_bad; // 化けた。記録だけして先へ進む (NEED で拾う)
      else
        accept_chunk();
      g_cwin_filled = 0;
      g_chdr_got = 0;
      if (g_state != state::FAILED && g_state != state::DONE)
        g_state = state::CSEEK;
      continue;
    }
    break;
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
  } else if (g_state == state::CSEEK || g_state == state::CHDR ||
             g_state == state::CDATA) {
    process_chunked_stream(data, len);
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

uintptr_t method_get_state(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return (uintptr_t)g_state;
}

uintptr_t method_get_last_ok(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // ★チャンク再送のラウンドの合間は「像が完成したか」がまだ決まっていない。
  //   ACK/NAK は XIAO にとって「Pico はもう喋らない」を告げる同期バイトで
  //   あって合否ではない (合否は NEED/done の行を母艦が読む) ので、ここでは
  //   「ハード故障を起こしていない」を返す。素直に g_last_ok を返すと、
  //   正常な多ラウンド転送でも毎ラウンド NAK になり、XIAO の状態 LED が
  //   赤く光って**転送が失敗しているように見える**。
  if (g_chunked && g_state == state::CSEEK)
    return 1;
  return (uintptr_t)g_last_ok;
}

uintptr_t method_get_missing(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return (uintptr_t)report_missing();
}

uintptr_t method_reset(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  say("reset\n");
  reset_transfer();
  return 0;
}

// ★「ota は何も抱えていない」= メッセージの途中でもなく、feed() の最中でも
//   ない。GET_STATE==IDLE を使わないのは、チャンク再送のラウンドの合間の
//   ota が IDLE ではなく CSEEK (次のチャンクを待っている) で待機しているため。
//   IDLE を待つと永久に来ず、中継が baud を戻せない。
uintptr_t method_get_quiescent(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  if (g_feeding)
    return 0;
  return (g_state == state::IDLE || g_state == state::CSEEK) ? 1 : 0;
}

uintptr_t poll_loop(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  while (true) {
    api(object_api::YIELD);
    if (!g_in.valid())
      continue;

    frame_t f{};
    uint32_t lost = 0;
    g_feeding = true;
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
    g_feeding = false;
    const uint64_t idle_limit =
        g_chunked ? CHUNKED_IDLE_TIMEOUT_US : IDLE_TIMEOUT_US;
    if (g_state != state::IDLE && g_state != state::DONE &&
        g_state != state::FAILED &&
        BOARD::time_us() - g_last_byte_us > idle_limit) {
      char line[96];
      if (snprintf(line, sizeof(line),
                   "timed out at %lu / %lu bytes — 捨てて待ち受けに戻る\n",
                   (unsigned long)g_received, (unsigned long)g_total) > 0)
        say(line);
      reset_transfer();
    }

    // ★チャンク再送 (XNOR) ではこの定期診断行を出さない。中継の終わりで
    //   シェルの UBRIDGE_LOST / UBRIDGE_DONE / ACK と UART0 上で衝突し、
    //   **ACK が化けて XIAO が 2 秒のアイドル待ちに落ちる** (2026-09-02 実機)。
    //   欲しい情報 (受理数・破損数・ラウンド数) は report_missing() の
    //   NEED 行がより正確に出すので、失うものが無い。XNOZ 経路は従来どおり
    //   — あちらは inflate failed を追うときの数少ない足跡なので残す。
    if (!g_chunked && g_writes != g_last_write_report) {
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
      // ★DONE/FAILED から IDLE へ落ちるのはここだけ (ota 自身のスレッド)
      //   なので、確定判定として競合なく残せる。GET_STATE は reset_transfer()
      //   の後は 0 (IDLE) に戻ってしまい、外から見えるのは一瞬だけ。
      g_last_ok = (g_state == state::DONE);
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
  failures += export_method(method::GET_STATE, (uintptr_t)&method_get_state);
  failures +=
      export_method(method::GET_LAST_OK, (uintptr_t)&method_get_last_ok);
  failures +=
      export_method(method::GET_MISSING, (uintptr_t)&method_get_missing);
  failures +=
      export_method(method::GET_QUIESCENT, (uintptr_t)&method_get_quiescent);
  failures += export_method(method::RESET, (uintptr_t)&method_reset);
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
