// ===========================================================================
//  ストリームの自己テスト — SPSC が 2 コアで本当に成り立つか (DESIGN §13 / §16)
// ===========================================================================
//  ★1 コアなら「協調型だから実質直列」で通ってしまう。2 コアで**同時に**押し引き
//    させないと、公開の順序 (中身 → 番号) も、席の強制も、何も確かめたことにならない。
//  ★見るのは「動いた」ではなく「**1 個も落とさず、順序どおりに届いたか**」。
//    通し番号を載せて、受け取った側が期待値と突き合わせる。
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/stream.hpp"

namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_PRODUCER = object_id::stream_producer;
constexpr uintptr_t OBJECT_CONSUMER = object_id::stream_consumer;
constexpr uintptr_t METHOD_MAIN = 0;
constexpr uint32_t RECORDS = 4000;
constexpr uint32_t CAPACITY = 64; // ★わざと小さくして、押し戻しを必ず起こさせる

struct item {
  uint32_t sequence;
  uint32_t from_core;
};

// ★容量を小さくしてあるので、producer は必ず一度は満杯に出会う。出会わないと
//   「押し戻し」の経路が試されない (許可のテストだけでは証拠にならない)。
stream::storage<item, CAPACITY> g_ring;
uintptr_t g_stream_id = 0;
volatile uint32_t g_pushed = 0;
volatile uint32_t g_popped = 0;
volatile uint32_t g_out_of_order = 0;
volatile uint32_t g_lost = 0;
volatile uint32_t g_full_hits = 0;
volatile uint32_t g_cores = 0;
volatile uint32_t g_producer_done = 0;
volatile uint32_t g_consumer_done = 0;

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

uintptr_t producer(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  api(object_api::STREAM_BIND, g_stream_id, (uintptr_t)stream::role::PRODUCER);
  auto out = g_ring.hdl();
  for (uint32_t index = 1; index <= RECORDS; ++index) {
    item record{index, BOARD::core_num()};
    while (!out.push(record)) {
      ++g_full_hits; // 満杯 = 押し戻された。待つのではなく譲る
      api(object_api::YIELD);
    }
    g_cores |= 1u << BOARD::core_num();
    g_pushed = index;
  }
  g_producer_done = 1;
  return 0;
}

uintptr_t consumer(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  api(object_api::STREAM_BIND, g_stream_id, (uintptr_t)stream::role::CONSUMER);
  auto in = g_ring.hdl();
  uint32_t expected = 1;
  uint32_t idle = 0;
  while (expected <= RECORDS && idle < 200000) {
    item record{};
    uint32_t lost = 0;
    if (!in.pop(&record, &lost)) {
      g_lost += lost;
      ++idle;
      api(object_api::YIELD);
      continue;
    }
    idle = 0;
    g_lost += lost;
    // ★通し番号で突き合わせる。「増えている」ではなく「**次のものが来た**」を見る。
    if (record.sequence != expected)
      ++g_out_of_order;
    expected = record.sequence + 1;
    g_cores |= 1u << BOARD::core_num();
    g_popped = record.sequence;
  }
  g_consumer_done = 1;
  return 0;
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

void stream_ladder() {
  BOARD::diag_printf("[SELFTEST] stream ladder start\n");
  g_ring.init(stream::LOSSLESS);

  const api_result created =
      api(object_api::STREAM_CREATE, (uintptr_t)&g_ring.desc);
  check("stream: created", created.error == 0, (unsigned long)created.error, 0);
  g_stream_id = created.value;
  // 引き直せること (番号だけで discovery できる = 両端が互いの storage を
  // extern 参照しなくてよい)。
  const api_result opened = api(object_api::STREAM_OPEN, g_stream_id);
  check("stream: opened by id", opened.value == (uintptr_t)&g_ring.desc,
        (unsigned long)opened.value, (unsigned long)&g_ring.desc);

  api(object_api::CREATE_OBJECT, OBJECT_PRODUCER, (uintptr_t)&producer, 0);
  api(object_api::CREATE_OBJECT, OBJECT_CONSUMER, (uintptr_t)&consumer, 0);
  api(object_api::SPAWN, OBJECT_PRODUCER, METHOD_MAIN, 0);
  api(object_api::SPAWN, OBJECT_CONSUMER, METHOD_MAIN, 0);

  for (uint32_t guard = 0;
       guard < 400000 && (g_producer_done == 0 || g_consumer_done == 0); ++guard)
    api(object_api::YIELD);

  check("stream: the producer sent every record", g_pushed == RECORDS,
        (unsigned long)g_pushed, (unsigned long)RECORDS);
  check("stream: the consumer received every record", g_popped == RECORDS,
        (unsigned long)g_popped, (unsigned long)RECORDS);
  // ★本命。LOSSLESS なので 1 個も落ちてはいけないし、順序も入れ替わってはいけない。
  check("stream: nothing was lost", g_lost == 0, (unsigned long)g_lost, 0);
  check("stream: nothing arrived out of order", g_out_of_order == 0,
        (unsigned long)g_out_of_order, 0);
  // ★押し戻しが実際に起きたこと。起きていなければ、容量が足りていて
  //   「満杯のときどうなるか」を何も試していない。
  check("stream: back-pressure actually happened", g_full_hits > 0,
        (unsigned long)g_full_hits, 1);
  // ★席は 1 つ。二人目は断られる (規約ではなく機構で守れているか)。
  const api_result second =
      api(object_api::STREAM_BIND, g_stream_id, (uintptr_t)stream::role::PRODUCER);
  check("stream: the second producer is refused",
        second.error == (uintptr_t)object_error::SEAT_TAKEN,
        (unsigned long)second.error, (unsigned long)object_error::SEAT_TAKEN);

  BOARD::diag_printf(
      "[SELFTEST] stream ladder done (cores seen 0x%lx, back-pressure %lu times)\n",
      (unsigned long)g_cores, (unsigned long)g_full_hits);
}

} // namespace selftest
} // namespace shizuku
