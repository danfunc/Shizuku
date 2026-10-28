// ===========================================================================
//  非特権オブジェクトが flash をストリーム越しに読み書きできるか (DESIGN §11.2 / §13)
// ===========================================================================
//  ★これが「制御は svc、データはストリーム」に分けた理由そのもの:
//    - **読み**は extent (XIP アドレス + 長さ) が流れる。写さない
//    - **書き**は非特権ではフラッシュを焼けない。だからバイトをストリームへ流し、
//      特権側の書き手が汲んで焼く。流し込む側は 33ms を一度も待たない
//  ★FlashFS を外したので、試す相手は flash オブジェクトではなく **flash_ops
//    (消去・書込み) とカーネルの stream** になった。目録・アドレス配布・書込み順序は
//    XNO 側 (flash_fs) の責務で、ここでは試さない。
//  ★確かめるのは「動いた」ではなく「**非特権のまま**動いた」。対象自身に
//    CONTROL を読ませて申告させる (§11.2.0 の教訓: 特権のまま走った計測を
//    「非特権で動いた」と読み違えた事故がある)。
//  ★★2026-08-24 (Q8): region0 は**ファーム本体の範囲だけ**。試す領域は flash_map の
//    FS 領域の末尾 1 セクタなので、開いた本人が GRANT_REGION で自分の extent を
//    明示的に開示しないと、直接ポインタで読んでも落ちる (docs/record/kernel_v2_record.typ
//    Q8)。「開いたその extent だけ読める」ことを、正当な読みと拒否のテストの
//    両方で確かめる。
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/method_entry.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/objects/flash_map.hpp"
#include "shizuku/objects/flash_ops.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/stream.hpp"
#include "shizuku/freestanding_mem.hpp"
#include "pico/error.h"

extern "C" void shizuku_flash_extent_read_pc();

#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
#include "pico.h"
extern "C" void shizuku_selftest_progress_mark(uint32_t, uint32_t, uint32_t);
namespace {
struct flash_stream_run_state_t { uint32_t magic, sequence; };
volatile flash_stream_run_state_t __uninitialized_ram(g_flash_stream_run_state);
uint32_t g_flash_stream_run_id;
constexpr uint32_t kFlashStreamRunMagic = 0x46535233u; // "FSR3"
}
#define FSTREAM_PROG(stage, event, value) \
  shizuku_selftest_progress_mark((stage), \
      ((g_flash_stream_run_id & 0x00ffffffu) << 8) | ((event) & 0xffu), \
      (uint32_t)(value))
#define FSTREAM_EVENT(event, value) \
  FSTREAM_PROG(610u + (((event) - 1u) % 6u), (event), (value))
#else
#define FSTREAM_PROG(stage, event, value) ((void)0)
#define FSTREAM_EVENT(event, value) ((void)0)
#endif
namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_WRITER = object_id::unpriv_writer;
constexpr uintptr_t OBJECT_READER = object_id::unpriv_reader;
constexpr uintptr_t OBJECT_SINK = object_id::unpriv_sink;
constexpr uintptr_t OBJECT_TRESPASSER = object_id::flash_trespasser;
#if defined(SHIZUKU_RP2040)
// RP2040 PMSAv6 cannot represent an exact 300-byte window. The scratch sector
// starts at a 4 KiB boundary, so 512 bytes is an exact MPU region for this test.
constexpr uint32_t PAYLOAD_BYTES = 512;
#else
constexpr uint32_t PAYLOAD_BYTES = 300; // ページ 1 枚を跨がせる
#endif

// ★試す場所は flash_map の FS 領域の末尾 1 セクタ。FS 領域は XNO の flash_fs が
//   先頭から詰めて使うので、末尾のほうが触り合いにくい。
constexpr uint32_t SCRATCH_OFFSET =
    objects::flash_map::FS_OFFSET + objects::flash_map::FS_BYTES - FLASH_SECTOR_SIZE;
constexpr uintptr_t SCRATCH_ADDRESS = XIP_BASE + SCRATCH_OFFSET;
static_assert(objects::flash_map::FS_BYTES >= FLASH_SECTOR_SIZE,
              "flash_map FS region must hold at least one sector for the selftest");
static_assert(SCRATCH_OFFSET % FLASH_SECTOR_SIZE == 0,
              "selftest scratch sector must be sector aligned");

// 書きのストリームが運ぶ 1 片。旧 FlashFS の flash_chunk と同じ形。
constexpr uint32_t CHUNK_BYTES = 64;
struct flash_chunk {
  uint32_t bytes; // 実際に詰まっている量 (最後の 1 片は半端になる)
  uint8_t data[CHUNK_BYTES];
};

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

uint8_t expected_byte(uint32_t index) { return (uint8_t)(index * 7u + 11u); }

// ★非特権で走る書き手。触れるのは**引数で渡されたディスクリプタと自分のスタック
//   だけ**。グローバルにも標準ライブラリにも触らない (触れば落ちる)。
//   戻り値の下位に CONTROL を載せて、非特権だったことを自分で申告させる。
uintptr_t unprivileged_writer(uintptr_t stream_id, uintptr_t, uintptr_t,
                              uintptr_t) {
  stream::handle<flash_chunk> out(stream_id);
  if (api(object_api::STREAM_BIND, stream_id,
          (uintptr_t)stream::role::PRODUCER).error != 0)
    return 0xFFFFFFFFu;
  uint32_t sent = 0;
  while (sent < PAYLOAD_BYTES) {
    flash_chunk chunk;
    shizuku_zero_record(&chunk); // `{}` は暗黙に memset を呼ぶ (非特権では落ちる)
    chunk.bytes = PAYLOAD_BYTES - sent < CHUNK_BYTES
                      ? PAYLOAD_BYTES - sent
                      : CHUNK_BYTES;
    for (uint32_t index = 0; index < chunk.bytes; ++index)
      chunk.data[index] = expected_byte(sent + index);
    if (!out.push(chunk))
      return 0xFFFFFFFFu; // 満杯 = 汲む側が居ない。ここでは起きてはならない
    sent += chunk.bytes;
  }
  return ARCH::control_register(); // 自分が非特権だったかを申告する
}

// Ring transports one extent record. The consumer reads the addressed XIP bytes
// under its independently granted MPU region.
uintptr_t unprivileged_reader(uintptr_t stream_id, uintptr_t, uintptr_t,
                              uintptr_t) {
  stream::handle<stream::extent> in(stream_id);
  if (api(object_api::STREAM_BIND, stream_id,
          (uintptr_t)stream::role::CONSUMER).error != 0)
    return 0xFFFFFFFFu;
  uint32_t sum = 0;
  stream::extent extent;
  shizuku_zero_record(&extent);
  if (in.pop(&extent)) {
    const volatile uint8_t *bytes = (const volatile uint8_t *)extent.address;
    for (uint32_t i = 0; i < extent.bytes; ++i) sum += bytes[i];
  }
  // 上位に CONTROL、下位に和。1 語で両方持ち帰るのは、非特権が触れる置き場所が
  // 戻り値しか無いため。
  return (ARCH::control_register() << 24) | (sum & 0x00FFFFFFu);
}

// ★非特権で走る受け手。**DMA が運んできた**環から汲む。運んだのはカーネル側の
//   DMA で、途中に中継オブジェクトは 1 つも居ない。
uintptr_t unprivileged_sink(uintptr_t stream_id, uintptr_t, uintptr_t,
                            uintptr_t) {
  stream::handle<stream::extent> in(stream_id);
  if (api(object_api::STREAM_BIND, stream_id,
          (uintptr_t)stream::role::CONSUMER).error != 0)
    return 0xFFFFFFFFu;
  uint32_t sum = 0;
  stream::extent extent;
  shizuku_zero_record(&extent);
  if (in.pop(&extent)) {
    const volatile uint8_t *bytes = (const volatile uint8_t *)extent.address;
    for (uint32_t i = 0; i < extent.bytes; ++i) sum += bytes[i];
  }
  return sum;
}

// ★拒否のテスト (Q8)。GRANT_REGION された extent の**外**を直接ポインタで
//   読もうとしたら落ちること。触る先は extent の直後 (MPU 粒度を越えた XIP 範囲 —
//   試す領域の隣にあり、この対象には開示していない場所)。
//   (docs/selftest/unprivileged.cpp と同じ形: 別スレッドで走らせ、系は続ける)。
[[gnu::noinline]] uintptr_t flash_trespasser(uintptr_t address, uintptr_t, uintptr_t, uintptr_t) {
  uintptr_t value;
  asm volatile(".global shizuku_flash_extent_read_pc\n"
               "shizuku_flash_extent_read_pc:\n"
               "ldrb %0, [%1]" : "=l"(value) : "l"(address) : "memory");
  return value;
}

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    record_fail(name, got, want);
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

// 自分が持ち主の extent stream を作り、extent を 1 つ積む。席は producer を自分が
//   取る (読み手は後から consumer に座る)。積めた件数は *pushed に入る。
api_result open_extent(uintptr_t address, uint32_t bytes, uint32_t *pushed) {
  const api_result created = api(object_api::STREAM_CREATE, sizeof(stream::extent),
                                 1, stream::LOSSLESS);
  if (created.error != 0) return created;
  const api_result bound = api(object_api::STREAM_BIND, created.value,
                               (uintptr_t)stream::role::PRODUCER);
  if (bound.error != 0) return bound;
  stream::extent extent{};
  extent.address = address;
  extent.bytes = bytes;
  *pushed = stream::handle<stream::extent>(created.value).push_n(&extent, 1);
  return created;
}

// 1 ページ焼く。flash_ops はページ整列の長さしか受けない。
int program_page(uint32_t offset, const uint8_t *page) {
  return objects::flash_ops::program(offset, page, FLASH_PAGE_SIZE);
}

struct drain_result {
  uint32_t received; // 汲んだバイト数
  int rc;            // 最初に失敗した flash_ops の戻り値 (PICO_OK = 全部焼けた)
};

// 書きの席 (consumer) に座って汲み、ページ単位で焼く。半端な最後のページは 0xFF で
//   埋める (0xFF はビットを落とさないので後続に無害 — 旧 FlashFS の close と同じ)。
drain_result drain_writes(uintptr_t stream_id, uint32_t offset) {
  alignas(4) static uint8_t page[FLASH_PAGE_SIZE];
  drain_result result{0, PICO_OK};
  uint32_t staged = 0;
  uint32_t written = 0;
  auto in = stream::handle<flash_chunk>(stream_id);
  flash_chunk chunk{};
  while (in.pop(&chunk)) {
    result.received += chunk.bytes;
    for (uint32_t index = 0; index < chunk.bytes; ++index) {
      page[staged++] = chunk.data[index];
      if (staged == FLASH_PAGE_SIZE) {
        if (result.rc == PICO_OK)
          result.rc = program_page(offset + written, page);
        written += FLASH_PAGE_SIZE;
        staged = 0;
      }
    }
  }
  if (staged != 0) {
    for (uint32_t at = staged; at < FLASH_PAGE_SIZE; ++at)
      page[at] = 0xFF;
    if (result.rc == PICO_OK)
      result.rc = program_page(offset + written, page);
  }
  return result;
}

} // namespace

void flash_stream_ladder() {
  BOARD::diag_printf("[SELFTEST] flash stream ladder start\n");
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  if (g_flash_stream_run_state.magic != kFlashStreamRunMagic) {
    g_flash_stream_run_state.magic = kFlashStreamRunMagic;
    g_flash_stream_run_state.sequence = 0;
  }
  g_flash_stream_run_id =
      (g_flash_stream_run_state.sequence + 1u) & 0x00ffffffu;
  g_flash_stream_run_state.sequence = g_flash_stream_run_id;
#endif
  FSTREAM_EVENT(1, 0);

  api(object_api::CREATE_OBJECT, OBJECT_WRITER, method_entry<ARCH, &unprivileged_writer>(),
      OBJECT_UNPRIVILEGED);
  api(object_api::CREATE_OBJECT, OBJECT_READER, method_entry<ARCH, &unprivileged_reader>(),
      OBJECT_UNPRIVILEGED);
  api(object_api::CREATE_OBJECT, OBJECT_SINK, method_entry<ARCH, &unprivileged_sink>(),
      OBJECT_UNPRIVILEGED);
  api(object_api::CREATE_OBJECT, OBJECT_TRESPASSER,
      method_entry<ARCH, &flash_trespasser>(), OBJECT_UNPRIVILEGED);
  FSTREAM_EVENT(2, 0);

  // ---- 書き: 非特権がバイトを流し、特権側が焼く ----------------------------
  // ★流し先の席 (consumer) は、焼く側である自分 (特権) が取る。
  const api_result write_ring =
      api(object_api::STREAM_CREATE, sizeof(flash_chunk), 32, stream::LOSSLESS);
  check("flash stream: kernel allocated the write ring", write_ring.error == 0,
        (unsigned long)write_ring.error, 0);
  if (write_ring.error != 0)
    return;
  const api_result write_seat = api(object_api::STREAM_BIND, write_ring.value,
                                    (uintptr_t)stream::role::CONSUMER);
  if (write_seat.error != 0)
    return;
  const api_result write_desc = api(object_api::STREAM_OPEN, write_ring.value);
  FSTREAM_EVENT(3, PAYLOAD_BYTES);
  // ★席を明け渡す: producer は非特権の書き手にする。consumer は特権側のまま。
  FSTREAM_EVENT(4, write_desc.value);
  const api_result wrote = api(object_api::CALL_METHOD, OBJECT_WRITER, 0,
                               write_desc.value);
  FSTREAM_EVENT(5, wrote.value);
  check("flash stream: the unprivileged writer ran non-privileged",
        (wrote.value & 1u) == 1u, (unsigned long)wrote.value, 1);

  // ★汲む前に書き先を消しておく。flash_ops は整列した範囲しか受けない。
  objects::flash_ops::timing erase_time{};
  const int erased =
      objects::flash_ops::erase(SCRATCH_OFFSET, FLASH_SECTOR_SIZE, &erase_time);
  check("flash ops: erased the scratch sector", erased == PICO_OK,
        (unsigned long)(uint32_t)erased, (unsigned long)PICO_OK);
  const drain_result drained = drain_writes(write_ring.value, SCRATCH_OFFSET);
  check("flash ops: programmed every page", drained.rc == PICO_OK,
        (unsigned long)(uint32_t)drained.rc, (unsigned long)PICO_OK);
  check("flash stream: the whole payload reached flash",
        drained.received == PAYLOAD_BYTES, (unsigned long)drained.received,
        (unsigned long)PAYLOAD_BYTES);
  FSTREAM_EVENT(6, drained.received);
  const api_result write_closed =
      api(object_api::STREAM_DESTROY, write_ring.value);
  check("flash stream: the write ring is released", write_closed.error == 0,
        (unsigned long)write_closed.error, 0);

  // ---- 読み: extent を stream に積み、非特権 consumer が XIP を読む ----------
  uint32_t read_pushed = 0;
  const api_result reading =
      open_extent(SCRATCH_ADDRESS, PAYLOAD_BYTES, &read_pushed);
  FSTREAM_EVENT(7, 0);
  FSTREAM_EVENT(8, reading.value);
  check("flash stream: opened the extent ring", reading.error == 0,
        (unsigned long)reading.error, 0);
  // Ring contains one extent record; file bytes remain at their XIP address.
  check("flash stream: one extent describes the payload", read_pushed == 1,
        (unsigned long)read_pushed, 1);
#if defined(SHIZUKU_RP2040)
  check("flash stream: RP2040 extent starts on a 4 KiB boundary",
        (SCRATCH_ADDRESS & 0xFFFu) == 0,
        (unsigned long)(SCRATCH_ADDRESS & 0xFFFu), 0);
#endif
  if (reading.error != 0 || read_pushed != 1)
    return;
  // ★Q8: region0 の外なので、読む本人 (OBJECT_READER) へ明示的に開示する。
  //   開示するのはこちら (特権で走っている選テストコード自身) — 「誰が誰に
  //   何を見せるか」を決めるのは特権行為 (call_method の呼び出し元ではなく、
  //   対象オブジェクトの性質として効く)。
  FSTREAM_EVENT(9, SCRATCH_ADDRESS);
  const api_result granted = api(object_api::GRANT_REGION, OBJECT_READER,
                                 SCRATCH_ADDRESS,
                                 SCRATCH_ADDRESS + PAYLOAD_BYTES);
  FSTREAM_EVENT(10, granted.value);
  check("flash stream: granted the reader its own extent", granted.value == 1,
        (unsigned long)granted.value, 1);
#if defined(SHIZUKU_RP2040)
  // A 300-byte grant is impossible here. Rejecting it must leave the valid
  // 512-byte grant above intact.
  FSTREAM_EVENT(11, SCRATCH_ADDRESS + 300);
  const api_result narrow_grant =
      api(object_api::GRANT_REGION, OBJECT_READER, SCRATCH_ADDRESS,
          SCRATCH_ADDRESS + 300);
  FSTREAM_EVENT(12, narrow_grant.error);
  check("flash stream: RP2040 rejects an unrepresentable 300-byte grant",
        narrow_grant.error == (uintptr_t)object_error::BAD_MEMORY &&
            narrow_grant.value == 0,
        (unsigned long)narrow_grant.error,
        (unsigned long)object_error::BAD_MEMORY);
#endif
  FSTREAM_EVENT(13, reading.value);
  const api_result read_desc = api(object_api::STREAM_OPEN, reading.value);
  FSTREAM_EVENT(14, read_desc.value);
  FSTREAM_EVENT(15, read_desc.value);
  const api_result got =
      api(object_api::CALL_METHOD, OBJECT_READER, 0, read_desc.value);
  FSTREAM_EVENT(16, got.value);
  uint32_t want = 0;
  for (uint32_t index = 0; index < PAYLOAD_BYTES; ++index)
    want += expected_byte(index);
  check("flash stream: reader call succeeded", got.error == 0,
        (unsigned long)got.error, 0);
  check("flash stream: the unprivileged reader ran non-privileged",
        ((got.value >> 24) & 1u) == 1u, (unsigned long)(got.value >> 24), 1);
  // ★★本命。非特権のまま XIP を直接読んで、書いた通りの中身が取れた。
  //   一致するということは、途中で誰も写していないということでもある。
  check("flash stream: the unprivileged reader saw the real bytes",
        (got.value & 0x00FFFFFFu) == (want & 0x00FFFFFFu),
        (unsigned long)(got.value & 0x00FFFFFFu), (unsigned long)want);
  const api_result closed_read = api(object_api::STREAM_DESTROY, reading.value);
  FSTREAM_EVENT(17, reading.value);
  check("flash stream: destroying the drained extent ring succeeds",
        closed_read.error == 0, (unsigned long)closed_read.error, 0);
  FSTREAM_EVENT(18, closed_read.error);

  // ---- 接続: flash から DMA で直に汲む ------------------------------------
  // DMA transports the extent record. The sink dereferences the extent under
  // its own explicitly granted MPU region.
  {
    uint32_t again_pushed = 0;
    FSTREAM_EVENT(19, 0);
    const api_result again =
        open_extent(SCRATCH_ADDRESS, PAYLOAD_BYTES, &again_pushed);
    FSTREAM_EVENT(20, again.value);
    FSTREAM_EVENT(21, 0);
    const api_result sink_id = api(object_api::STREAM_CREATE,
                                   sizeof(stream::extent), 1,
                                   stream::LOSSLESS);
    FSTREAM_EVENT(22, sink_id.value);
    check("connect: kernel allocated the sink ring", sink_id.error == 0,
          (unsigned long)sink_id.error, 0);
    if (sink_id.error != 0) return;
    if (again.error != 0 || again_pushed != 1) return;
    FSTREAM_EVENT(23, SCRATCH_ADDRESS);
    const api_result sink_grant =
        api(object_api::GRANT_REGION, OBJECT_SINK, SCRATCH_ADDRESS,
            SCRATCH_ADDRESS + PAYLOAD_BYTES);
    FSTREAM_EVENT(24, sink_grant.value);
    check("connect: granted the sink its own extent",
          sink_grant.error == 0 && sink_grant.value == 1,
          (unsigned long)sink_grant.error, 0);
    FSTREAM_EVENT(25, again.value);
    const api_result linked =
        api(object_api::STREAM_CONNECT, again.value, sink_id.value);
    FSTREAM_EVENT(26, linked.value);
    check("connect: the two streams were joined", linked.error == 0,
          (unsigned long)linked.error, 0);
    // ★繋いだ席は埋まる。手押しとの二重供給を機構で防いでいるか。
    FSTREAM_EVENT(27, sink_id.value);
    const api_result intruder = api(object_api::STREAM_BIND, sink_id.value,
                                    (uintptr_t)stream::role::PRODUCER);
    FSTREAM_EVENT(28, intruder.error);
    check("connect: a hand-fed producer is refused on a joined stream",
          intruder.error == (uintptr_t)object_error::SEAT_TAKEN,
          (unsigned long)intruder.error,
          (unsigned long)object_error::SEAT_TAKEN);

    FSTREAM_EVENT(29, again.value);
    const api_result busy_destroy = api(object_api::STREAM_DESTROY, again.value);
    FSTREAM_EVENT(30, busy_destroy.error);
    check("connect: connected source cannot be destroyed",
          busy_destroy.error == (uintptr_t)object_error::STREAM_BUSY,
          (unsigned long)busy_destroy.error,
          (unsigned long)object_error::STREAM_BUSY);

    // ★汲むのは**非特権のオブジェクト**にやらせる。譲るたびにポンプが 1 歩進むので、
    //   「少し汲む → 譲る」を繰り返す。受け手が特権だと、非特権から使えることの
    //   証明にならない。
    uint32_t sum = 0;
    bool sink_call_success_checked = false;
    bool sink_call_failure_reported = false;
    bool sink_call_progress_marked = false;
    FSTREAM_EVENT(31, sink_id.value);
    for (uint32_t spin = 0; spin < 20000; ++spin) {
      const api_result got = api(object_api::CALL_METHOD, OBJECT_SINK, 0,
                                  sink_id.value);
      if (!sink_call_progress_marked) {
        FSTREAM_EVENT(32, got.error);
        sink_call_progress_marked = true;
      }
      if (got.error != 0) {
        if (!sink_call_failure_reported) {
          check("connect: sink call succeeded", false,
                (unsigned long)got.error, 0);
          sink_call_failure_reported = true;
        }
        break;
      }
      if (!sink_call_success_checked) {
        check("connect: sink call succeeded", true,
              (unsigned long)got.error, 0);
        sink_call_success_checked = true;
      }
      if (got.value != 0) sum = (uint32_t)got.value;
      api(object_api::YIELD); // ポンプに 1 歩進ませる
      if (sum != 0)
        break;
    }
    FSTREAM_EVENT(33, sum);
    const uint32_t drained_extent = sum != 0 ? 1 : 0;
    uint32_t want = 0;
    for (uint32_t index = 0; index < PAYLOAD_BYTES; ++index)
      want += expected_byte(index);
    check("connect: extent arrived through DMA", drained_extent == 1,
          (unsigned long)drained_extent, 1);
    // ★★本命。CPU が 1 バイトも写さずに、flash の中身が受け手の環まで届いた。
    check("connect: the bytes are the ones in flash", sum == want,
          (unsigned long)sum, (unsigned long)want);
    FSTREAM_EVENT(34, linked.value);
    const api_result disconnected =
        api(object_api::STREAM_DISCONNECT, linked.value);
    FSTREAM_EVENT(35, disconnected.error);
    for (uint32_t spin = 0; spin < 1000; ++spin) api(object_api::YIELD);
    FSTREAM_EVENT(36, again.value);
    const api_result source_closed =
        api(object_api::STREAM_DESTROY, again.value);
    FSTREAM_EVENT(37, source_closed.error);
    check("connect: source destroy succeeds after disconnect completion",
          source_closed.error == 0, (unsigned long)source_closed.error, 0);
    FSTREAM_EVENT(38, sink_id.value);
    const api_result sink_closed = api(object_api::STREAM_DESTROY, sink_id.value);
    FSTREAM_EVENT(39, sink_closed.error);
    check("connect: owner destroys disconnected sink",
          sink_closed.error == 0, (unsigned long)sink_closed.error, 0);
  }

  // ---- 拒否: 開示された extent の外を読もうとしたら落ちる (Q8) --------------
  {
    // ★同じ領域を、境界を確かめるために narrow な extent として開き直す。開示するのは
    //   ちょうどこの extent だけ — grant の外なので、region の外 = 特権のみに
    //   落ちるはず。
    // ★★境界のすぐ外は MPU の領域粒度次第で同じ許可範囲に入り得るため、
    //   十分離れた先 (1 ページ = 4096B 先、試す領域の隣で自分には開示されていない場所)
    //   を狙う。
    uint32_t bounded_pushed = 0;
    FSTREAM_EVENT(40, 0);
    const api_result bounded =
        open_extent(SCRATCH_ADDRESS, PAYLOAD_BYTES, &bounded_pushed);
    FSTREAM_EVENT(41, bounded.value);
    const uintptr_t extent_end = SCRATCH_ADDRESS + PAYLOAD_BYTES;
    const uintptr_t trespass_address = extent_end + 4096;
    FSTREAM_EVENT(42, SCRATCH_ADDRESS);
    const api_result trespass_grant =
        api(object_api::GRANT_REGION, OBJECT_TRESPASSER, SCRATCH_ADDRESS,
            extent_end);
    FSTREAM_EVENT(43, trespass_grant.value);
    check("deny: granted the trespasser its bounded extent",
          trespass_grant.error == 0 && trespass_grant.value == 1,
          (unsigned long)trespass_grant.error, 0);

    const auto faults_before = kernel_instance.faults().count;
    // ★**別スレッドで**走らせる (unprivileged.cpp と同じ理由: 落ちるのはスレッド単位なので、
    //   呼び出しで試すと自分ごと止まってテストの続きが走れない)。
    FSTREAM_EVENT(44, trespass_address);
    const auto spawned = ARCH::syscall((uintptr_t)object_api::SPAWN,
                                       OBJECT_TRESPASSER, 0, trespass_address);
    FSTREAM_EVENT(45, spawned.value);
    const bool spawn_succeeded = spawned.error == 0 && spawned.value != 0;
    check("deny: trespasser thread spawned", spawn_succeeded,
          (unsigned long)spawned.error, 0);
    if (spawn_succeeded) {
      for (uint32_t guard =
               0; guard < 200 && kernel_instance.faults().count == faults_before;
           ++guard)
        ARCH::syscall((uintptr_t)object_api::YIELD);
    }

    const auto &record = kernel_instance.faults();
    FSTREAM_EVENT(46, record.count);
    const bool new_fault = spawn_succeeded &&
                           record.count == faults_before + 1;
    bool expected_fault = new_fault;
    if constexpr (ARCH::IS_ARMV8M) {
      expected_fault = expected_fault && (record.status & 0x82u) == 0x82u &&
          record.address == trespass_address &&
          (record.pc & ~uintptr_t(1)) ==
              (reinterpret_cast<uintptr_t>(&shizuku_flash_extent_read_pc) & ~uintptr_t(1));
    }
    check("deny: reading past the granted extent faults",
          expected_fault,
          (unsigned long)(ARCH::IS_ARMV8M ? record.address : record.count),
          (unsigned long)(ARCH::IS_ARMV8M ? trespass_address : faults_before + 1));
    check("deny: the offending thread was stopped",
          new_fault && record.thread == spawned.value,
          (unsigned long)record.thread,
          (unsigned long)spawned.value);
    BOARD::diag_printf(
        "[SELFTEST] deny: flash extent [%08lx,%08lx) trespass at %08lx "
        "stopped thread %lu cfsr=%08lx actual=%08lx pc=%08lx\n",
        (unsigned long)SCRATCH_ADDRESS, (unsigned long)extent_end,
        (unsigned long)trespass_address, (unsigned long)record.thread,
        (unsigned long)record.status, (unsigned long)record.address,
        (unsigned long)record.pc);

    // ---- 対照: 同じ extent の**中**なら読める (境界そのものを確かめる) ------
    const auto in_bounds_faults = kernel_instance.faults().count;
    FSTREAM_EVENT(47, extent_end - 1);
    const auto in_bounds = ARCH::syscall((uintptr_t)object_api::SPAWN,
                                         OBJECT_TRESPASSER, 0,
                                         extent_end - 1);
    FSTREAM_EVENT(48, in_bounds.value);
    const bool in_bounds_spawned =
        in_bounds.error == 0 && in_bounds.value != 0;
    check("deny: in-bounds probe thread spawned", in_bounds_spawned,
          (unsigned long)in_bounds.error, 0);
    bool in_bounds_terminated = false;
    if (in_bounds_spawned) {
      for (uint32_t guard = 0; guard < 200; ++guard) {
        ARCH::syscall((uintptr_t)object_api::YIELD);
        if (kernel_instance.thread_state((uint32_t)in_bounds.value) ==
            KERNEL::THREAD::state_t::TERMINATED) {
          in_bounds_terminated = true;
          break;
        }
      }
    }
    FSTREAM_EVENT(49, in_bounds_terminated ? in_bounds.value : 0);
    // 判定は 3 つの AND (spawn・TERMINATED・fault 数不変) で、check の got/want は
    // 最後の 1 つしか見せない。落ちたときに残りの 2 つを見えるようにする (判定は弱めない)。
    if (!(in_bounds_spawned && in_bounds_terminated)) {
      BOARD::diag_printf("[SELFTEST] deny: in-bounds thread %lu state=%lu spawned=%u "
                         "terminated=%u faults=%lu->%lu\n",
                         (unsigned long)in_bounds.value,
                         (unsigned long)kernel_instance.thread_state((uint32_t)in_bounds.value),
                         (unsigned)in_bounds_spawned, (unsigned)in_bounds_terminated,
                         (unsigned long)in_bounds_faults,
                         (unsigned long)kernel_instance.faults().count);
    }
    check("deny: the last byte inside the extent completes without fault",
          in_bounds_spawned && in_bounds_terminated &&
              kernel_instance.faults().count == in_bounds_faults,
          (unsigned long)kernel_instance.faults().count,
          (unsigned long)in_bounds_faults);
    FSTREAM_EVENT(50, bounded.value);
    const api_result bounded_closed = api(object_api::STREAM_DESTROY, bounded.value);
    FSTREAM_EVENT(51, bounded_closed.error);
  }

  FSTREAM_PROG(62, 52, 0);
  BOARD::diag_printf("[SELFTEST] flash stream ladder done\n");
}

} // namespace selftest
} // namespace shizuku
