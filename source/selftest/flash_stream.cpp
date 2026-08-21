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

// ★非特権で走る読み手。extent を受け取り、**XIP から直接**読んで足し合わせる。
//   1 バイトも写していないことが、返る和が合うことで分かる。
uintptr_t unprivileged_reader(uintptr_t descriptor, uintptr_t, uintptr_t,
                              uintptr_t) {
  stream::handle<stream::extent> in((stream::descriptor *)descriptor);
  stream::extent piece{};
  if (!in.pop(&piece))
    return 0;
  uint32_t sum = 0;
  const uint8_t *bytes = (const uint8_t *)piece.address;
  for (uint32_t index = 0; index < piece.bytes; ++index)
    sum += bytes[index];
  // 上位に CONTROL、下位に和。1 語で両方持ち帰るのは、非特権が触れる置き場所が
  // 戻り値しか無いため。
  return (ARCH::control_register() << 24) | (sum & 0x00FFFFFFu);
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

  // ---- 書き: 非特権がバイトを流し、特権側が焼く ----------------------------
  flash_open opening{"unpriv.bin", 4096, 0};
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
  flash_open reading{"unpriv.bin", 0, 0};
  const api_result opened_read =
      api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
          (uintptr_t)flash_fs_method::OPEN_READ, (uintptr_t)&reading);
  check("flash stream: opened for reading", opened_read.value != 0,
        (unsigned long)opened_read.value, 1);
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

  BOARD::diag_printf("[SELFTEST] flash stream ladder done\n");
}

} // namespace selftest
} // namespace shizuku
