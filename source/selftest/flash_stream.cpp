// ===========================================================================
//  非特権オブジェクトが flash をストリーム越しに読み書きできるか (DESIGN §11.2 / §13)
// ===========================================================================
//  ★これが「制御はメソッド、データはストリーム」に分けた理由そのもの:
//    - **読み**は extent (XIP アドレス + 長さ) が流れる。写さない
//    - **書き**は非特権ではフラッシュを焼けない。だからバイトをストリームへ流し、
//      特権側の書き手が汲んで焼く。流し込む側は 33ms を一度も待たない
//  ★確かめるのは「動いた」ではなく「**非特権のまま**動いた」。対象自身に
//    CONTROL を読ませて申告させる (§11.2.0 の教訓: 特権のまま走った計測を
//    「非特権で動いた」と読み違えた事故がある)。
//  ★★2026-08-24 (Q8): region0 は**ファーム本体の範囲だけ**になった。flash_fs の
//    データは region0 の外なので、開いた本人が GRANT_REGION で自分の extent を
//    明示的に開示しないと、直接ポインタで読んでも落ちる (docs/03_porting_policy.md
//    Q8)。「開いたその extent だけ読める」ことを、正当な読みと拒否のテストの
//    両方で確かめる。
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
constexpr uintptr_t OBJECT_TRESPASSER = object_id::flash_trespasser;
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

// ★拒否のテスト (Q8)。GRANT_REGION された extent の**外**を直接ポインタで
//   読もうとしたら落ちること。触る先は「自分の extent のすぐ 1 バイト先」—
//   flash_fs のデータ領域の中ではあるが、この対象には開示していない場所。
//   (docs/selftest/unprivileged.cpp と同じ形: 別スレッドで走らせ、系は続ける)。
uintptr_t flash_trespasser(uintptr_t address, uintptr_t, uintptr_t, uintptr_t) {
  const volatile uint8_t *forbidden = (const volatile uint8_t *)address;
  return *forbidden; // ここで落ちるのが正しい
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
  api(object_api::CREATE_OBJECT, OBJECT_TRESPASSER,
      (uintptr_t)&flash_trespasser, OBJECT_UNPRIVILEGED);

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
  // ★Q8: region0 の外なので、読む本人 (OBJECT_READER) へ明示的に開示する。
  //   開示するのはこちら (特権で走っている選テストコード自身) — 「誰が誰に
  //   何を見せるか」を決めるのは特権行為 (call_method の呼び出し元ではなく、
  //   対象オブジェクトの性質として効く)。
  const api_result granted =
      api(object_api::GRANT_REGION, OBJECT_READER, reading.address,
          reading.address + reading.records);
  check("flash stream: granted the reader its own extent", granted.value == 1,
        (unsigned long)granted.value, 1);
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

  // ---- 拒否: 開示された extent の外を読もうとしたら落ちる (Q8) --------------
  {
    // ★同じファイルを、境界を確かめるために narrow に開き直す。開示するのは
    //   ちょうどこの extent だけ — grant の外なので、region の外 = 特権のみに
    //   落ちるはず。
    // ★★狙う先は extent の**ちょうど 1 バイト先ではない**。PMSAv8 は 32B 粒度
    //   なので、1 バイト外はハードウェアの丸め次第でたまたま同じブロックに
    //   収まり、落ちないことがある (実測で踏んだ — 境界ちょうどは粒度の限界で
    //   判定できない)。粒度の余地を確実に超える先 (1 ページ = 4096B 先、まだ
    //   flash_fs のデータ領域の中だが自分には開示されていない場所) を狙う。
    flash_open bounded{"unpriv.bin", 0, 1, 0, 0, 0};
    api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
        (uintptr_t)flash_fs_method::OPEN_READ, (uintptr_t)&bounded);
    const uintptr_t extent_end = bounded.address + bounded.records;
    const uintptr_t trespass_address = extent_end + 4096;
    api(object_api::GRANT_REGION, OBJECT_TRESPASSER, bounded.address,
        extent_end);

    const auto faults_before = kernel_instance.faults().count;
    // ★**別スレッドで**走らせる (unprivileged.cpp と同じ理由: 落ちるのは
    //   スレッド単位なので、呼び出しで試すと自分ごと止まってテストの続きが
    //   走れない)。
    const auto spawned = ARCH::syscall((uintptr_t)object_api::SPAWN,
                                       OBJECT_TRESPASSER, 0, trespass_address);
    for (uint32_t guard = 0;
         guard < 200 && kernel_instance.faults().count == faults_before;
         ++guard)
      ARCH::syscall((uintptr_t)object_api::YIELD);

    const auto &record = kernel_instance.faults();
    check("deny: reading past the granted extent faults",
          record.count == faults_before + 1, (unsigned long)record.count,
          (unsigned long)(faults_before + 1));
    check("deny: the offending thread was stopped",
          record.thread == spawned.value, (unsigned long)record.thread,
          (unsigned long)spawned.value);
    BOARD::diag_printf(
        "[SELFTEST] deny: flash extent [%08lx,%08lx) trespass at %08lx "
        "stopped thread %lu cfsr=%08lx\n",
        (unsigned long)bounded.address, (unsigned long)extent_end,
        (unsigned long)trespass_address, (unsigned long)record.thread,
        (unsigned long)record.status);

    // ---- 対照: 同じ extent の**中**なら読める (境界そのものを確かめる) ------
    const auto in_bounds_faults = kernel_instance.faults().count;
    const auto in_bounds = ARCH::syscall((uintptr_t)object_api::SPAWN,
                                         OBJECT_TRESPASSER, 0,
                                         extent_end - 1);
    for (uint32_t guard = 0; guard < 200; ++guard) {
      ARCH::syscall((uintptr_t)object_api::YIELD);
      if (kernel_instance.thread_state((uint32_t)in_bounds.value) ==
          KERNEL::THREAD::state_t::TERMINATED)
        break;
    }
    check("deny: the last byte inside the extent does not fault",
          kernel_instance.faults().count == in_bounds_faults,
          (unsigned long)kernel_instance.faults().count,
          (unsigned long)in_bounds_faults);
  }

  BOARD::diag_printf("[SELFTEST] flash stream ladder done\n");
}

} // namespace selftest
} // namespace shizuku
