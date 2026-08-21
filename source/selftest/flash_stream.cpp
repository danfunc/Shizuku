// ===========================================================================
//  非特権オブジェクトが flash をストリーム越しに読み書きできるか (DESIGN §11.2 / §13)
// ===========================================================================
//  ★これが「制御はメソッド、データはストリーム」に分けた理由そのもの:
//    - **読み**は extent (XIP アドレス + 長さ) が流れる。XIP は region0 が
//      読み出しを全員に許しているので、非特権のまま**直接読める**。写さない
//    - **書き**は非特権ではフラッシュを焼けない。だからバイトをストリームへ流し、
//      特権側の書き手が汲んで焼く。流し込む側は 33ms を一度も待たない
//  ★確かめるのは「動いた」ではなく「**非特権のまま**動いた」。対象自身に
//    CONTROL を読ませて申告させる (§11.2.0 の教訓: 特権のまま走った計測を
//    「非特権で動いた」と読み違えた事故がある)。
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/objects/flash_fs.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/stream.hpp"

namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_WRITER = object_id::unpriv_writer;
constexpr uintptr_t OBJECT_READER = object_id::unpriv_reader;
constexpr uintptr_t OBJECT_SINK = object_id::unpriv_sink;
constexpr uint32_t PAYLOAD_BYTES = 300; // ページ 1 枚を跨がせる

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
uintptr_t unprivileged_writer(uintptr_t descriptor, uintptr_t, uintptr_t,
                              uintptr_t) {
  stream::handle<objects::flash_chunk> out((stream::descriptor *)descriptor);
  uint32_t sent = 0;
  while (sent < PAYLOAD_BYTES) {
    objects::flash_chunk chunk{};
    chunk.bytes = PAYLOAD_BYTES - sent < objects::FLASH_CHUNK_BYTES
                      ? PAYLOAD_BYTES - sent
                      : objects::FLASH_CHUNK_BYTES;
    for (uint32_t index = 0; index < chunk.bytes; ++index)
      chunk.data[index] = expected_byte(sent + index);
    if (!out.push(chunk))
      return 0xFFFFFFFFu; // 満杯 = 汲む側が居ない。ここでは起きてはならない
    sent += chunk.bytes;
  }
  return ARCH::control_register(); // 自分が非特権だったかを申告する
}

// ★非特権で走る読み手。**ストリームの実体は flash そのもの** — pop は XIP から
//   直に 1 レコード読む。中継の環はどこにも無いので、写す仕事も一貫性の心配も無い。
//   非特権のまま読めるのは、ディスクリプタが arena (書ける側) にあり、
//   base が XIP (読める側) にあるから。
uintptr_t unprivileged_reader(uintptr_t descriptor, uintptr_t, uintptr_t,
                              uintptr_t) {
  stream::handle<uint8_t> in((stream::descriptor *)descriptor);
  uint32_t sum = 0;
  uint8_t byte = 0;
  while (in.pop(&byte))
    sum += byte;
  // 上位に CONTROL、下位に和。1 語で両方持ち帰るのは、非特権が触れる置き場所が
  // 戻り値しか無いため。
  return (ARCH::control_register() << 24) | (sum & 0x00FFFFFFu);
}

// ★非特権で走る受け手。**DMA が運んできた**環から汲む。運んだのはカーネル側の
//   DMA で、途中に中継オブジェクトは 1 つも居ない。
uintptr_t unprivileged_sink(uintptr_t descriptor, uintptr_t, uintptr_t,
                            uintptr_t) {
  stream::handle<uint8_t> in((stream::descriptor *)descriptor);
  uint32_t sum = 0;
  uint8_t byte = 0;
  while (in.pop(&byte))
    sum += byte;
  return sum;
}

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

} // namespace

void flash_stream_ladder() {
  using namespace shizuku::objects;
  BOARD::diag_printf("[SELFTEST] flash stream ladder start\n");

  api(object_api::CREATE_OBJECT, OBJECT_WRITER, (uintptr_t)&unprivileged_writer,
      OBJECT_UNPRIVILEGED);
  api(object_api::CREATE_OBJECT, OBJECT_READER, (uintptr_t)&unprivileged_reader,
      OBJECT_UNPRIVILEGED);
  api(object_api::CREATE_OBJECT, OBJECT_SINK, (uintptr_t)&unprivileged_sink,
      OBJECT_UNPRIVILEGED);

  // ---- 書き: 非特権がバイトを流し、特権側が焼く ----------------------------
  flash_open opening{"unpriv.bin", 4096, 0, 0, 0, 0};
  const api_result opened =
      api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
          (uintptr_t)flash_fs_method::OPEN_WRITE, (uintptr_t)&opening);
  check("flash stream: opened for writing", opened.value != 0,
        (unsigned long)opened.value, 1);
  if (opened.value == 0)
    return;
  const api_result write_desc = api(object_api::STREAM_OPEN, opening.stream);
  // ★席を明け渡す: producer は非特権の書き手にする。FS は consumer のまま。
  const api_result wrote = api(object_api::CALL_METHOD, OBJECT_WRITER, 0,
                               write_desc.value);
  check("flash stream: the unprivileged writer ran non-privileged",
        (wrote.value & 1u) == 1u, (unsigned long)wrote.value, 1);
  const api_result closed =
      api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
          (uintptr_t)flash_fs_method::CLOSE_WRITE, 0);
  check("flash stream: the whole payload reached flash",
        closed.value == PAYLOAD_BYTES, (unsigned long)closed.value,
        (unsigned long)PAYLOAD_BYTES);

  // ---- 読み: 非特権が XIP から直接読む -------------------------------------
  flash_open reading{"unpriv.bin", 0, 1, 0, 0, 0};
  const api_result opened_read =
      api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
          (uintptr_t)flash_fs_method::OPEN_READ, (uintptr_t)&reading);
  check("flash stream: opened for reading", opened_read.value != 0,
        (unsigned long)opened_read.value, 1);
  // ★環を持たないので、レコード数はファイルの大きさそのもの。
  check("flash stream: the file itself is the stream",
        reading.records == PAYLOAD_BYTES, (unsigned long)reading.records,
        (unsigned long)PAYLOAD_BYTES);
  if (opened_read.value == 0)
    return;
  const api_result read_desc = api(object_api::STREAM_OPEN, reading.stream);
  const api_result got =
      api(object_api::CALL_METHOD, OBJECT_READER, 0, read_desc.value);
  uint32_t want = 0;
  for (uint32_t index = 0; index < PAYLOAD_BYTES; ++index)
    want += expected_byte(index);
  check("flash stream: the unprivileged reader ran non-privileged",
        ((got.value >> 24) & 1u) == 1u, (unsigned long)(got.value >> 24), 1);
  // ★★本命。非特権のまま XIP を直接読んで、書いた通りの中身が取れた。
  //   一致するということは、途中で誰も写していないということでもある。
  check("flash stream: the unprivileged reader saw the real bytes",
        (got.value & 0x00FFFFFFu) == (want & 0x00FFFFFFu),
        (unsigned long)(got.value & 0x00FFFFFFu), (unsigned long)want);

  // ---- 接続: flash から DMA で直に汲む ------------------------------------
  // ★これが「中継オブジェクトの pop/push が消える」ことの実証。src は XIP を
  //   実体とする読みストリームなので、**DMA はフラッシュから直接読んで**受け手の
  //   環へ書く。CPU は 1 バイトも写していない。
  {
    flash_open again{"unpriv.bin", 0, 1, 0, 0, 0};
    api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
        (uintptr_t)flash_fs_method::OPEN_READ, (uintptr_t)&again);
    // 受け側の環はオブジェクト arena に置く (非特権が汲める側)。
    using sink_ring = stream::storage<uint8_t, 128>;
    const api_result memory = api(object_api::MEMORY_ALLOCATE, sizeof(sink_ring));
    check("connect: room for the sink", memory.value != 0,
          (unsigned long)memory.value, 1);
    if (memory.value == 0)
      return;
    sink_ring *sink = (sink_ring *)memory.value;
    sink->init(stream::LOSSLESS);
    const api_result sink_id = api(object_api::STREAM_CREATE,
                                   (uintptr_t)&sink->desc);
    const api_result linked =
        api(object_api::STREAM_CONNECT, again.stream, sink_id.value);
    check("connect: the two streams were joined", linked.error == 0,
          (unsigned long)linked.error, 0);
    // ★繋いだ席は埋まる。手押しとの二重供給を機構で防いでいるか。
    const api_result intruder = api(object_api::STREAM_BIND, sink_id.value,
                                    (uintptr_t)stream::role::PRODUCER);
    check("connect: a hand-fed producer is refused on a joined stream",
          intruder.error == (uintptr_t)object_error::SEAT_TAKEN,
          (unsigned long)intruder.error,
          (unsigned long)object_error::SEAT_TAKEN);

    // ★汲むのは**非特権のオブジェクト**にやらせる。譲るたびにポンプが 1 歩進むので、
    //   「少し汲む → 譲る」を繰り返す。受け手が特権だと、非特権から使えることの
    //   証明にならない。
    uint32_t sum = 0;
    for (uint32_t spin = 0; spin < 20000; ++spin) {
      const api_result got =
          api(object_api::CALL_METHOD, OBJECT_SINK, 0, (uintptr_t)&sink->desc);
      sum += (uint32_t)got.value;
      api(object_api::YIELD); // ポンプに 1 歩進ませる
      if (sink->desc.rd >= PAYLOAD_BYTES)
        break;
    }
    const uint32_t drained = sink->desc.rd;
    uint32_t want = 0;
    for (uint32_t index = 0; index < PAYLOAD_BYTES; ++index)
      want += expected_byte(index);
    check("connect: every byte arrived through DMA", drained == PAYLOAD_BYTES,
          (unsigned long)drained, (unsigned long)PAYLOAD_BYTES);
    // ★★本命。CPU が 1 バイトも写さずに、flash の中身が受け手の環まで届いた。
    check("connect: the bytes are the ones in flash", sum == want,
          (unsigned long)sum, (unsigned long)want);
  }

  BOARD::diag_printf("[SELFTEST] flash stream ladder done\n");
}

} // namespace selftest
} // namespace shizuku
