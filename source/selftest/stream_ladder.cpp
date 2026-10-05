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

#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
extern "C" void shizuku_selftest_progress_mark(uint32_t stage, uint32_t a, uint32_t b);
#define SHIZUKU_PROG(stage, a, b) shizuku_selftest_progress_mark((stage), (a), (b))
#else
#define SHIZUKU_PROG(stage, a, b) ((void)0)
#endif

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
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  const api_result bound =
      api(object_api::STREAM_BIND, g_stream_id, (uintptr_t)stream::role::PRODUCER);
  SHIZUKU_PROG(50, (uint32_t)bound.error, (uint32_t)bound.value);
#else
  api(object_api::STREAM_BIND, g_stream_id, (uintptr_t)stream::role::PRODUCER);
#endif
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
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  const api_result bound =
      api(object_api::STREAM_BIND, g_stream_id, (uintptr_t)stream::role::CONSUMER);
  SHIZUKU_PROG(51, (uint32_t)bound.error, (uint32_t)bound.value);
#else
  api(object_api::STREAM_BIND, g_stream_id, (uintptr_t)stream::role::CONSUMER);
#endif
  auto in = g_ring.hdl();
  uint32_t expected = 1;
  uint32_t idle = 0;
  while (expected <= RECORDS && idle < 200000) {
    item record;
    shizuku_zero_record(&record);
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
    record_fail(name, got, want);
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

} // namespace

void stream_ladder() {
  BOARD::diag_printf("[SELFTEST] stream ladder start\n");
  SHIZUKU_PROG(1, 0, 0);
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
  SHIZUKU_PROG(10, (uint32_t)created.error, (uint32_t)(opened.value == (uintptr_t)&g_ring.desc));

#if !(defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0)
  api(object_api::CREATE_OBJECT, OBJECT_PRODUCER, (uintptr_t)&producer, 0);
  api(object_api::CREATE_OBJECT, OBJECT_CONSUMER, (uintptr_t)&consumer, 0);
  api(object_api::SPAWN, OBJECT_PRODUCER, METHOD_MAIN, 0);
  api(object_api::SPAWN, OBJECT_CONSUMER, METHOD_MAIN, 0);
#else
  const api_result created_producer =
      api(object_api::CREATE_OBJECT, OBJECT_PRODUCER, (uintptr_t)&producer, 0);
  SHIZUKU_PROG(20, (uint32_t)created_producer.error, (uint32_t)created_producer.value);
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  BOARD::diag_printf("[SELFTEST_PROG] CREATE_OBJECT producer: error=%lu value=%lu\n",
                      (unsigned long)created_producer.error,
                      (unsigned long)created_producer.value);
#endif
  const api_result created_consumer =
      api(object_api::CREATE_OBJECT, OBJECT_CONSUMER, (uintptr_t)&consumer, 0);
  SHIZUKU_PROG(21, (uint32_t)created_consumer.error, (uint32_t)created_consumer.value);
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  BOARD::diag_printf("[SELFTEST_PROG] CREATE_OBJECT consumer: error=%lu value=%lu\n",
                      (unsigned long)created_consumer.error,
                      (unsigned long)created_consumer.value);
#endif
  const api_result spawned_producer =
      api(object_api::SPAWN, OBJECT_PRODUCER, METHOD_MAIN, 0);
  SHIZUKU_PROG(22, (uint32_t)spawned_producer.error, (uint32_t)spawned_producer.value);
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  BOARD::diag_printf("[SELFTEST_PROG] SPAWN producer: error=%lu value=%lu\n",
                      (unsigned long)spawned_producer.error,
                      (unsigned long)spawned_producer.value);
#endif
  const api_result spawned_consumer =
      api(object_api::SPAWN, OBJECT_CONSUMER, METHOD_MAIN, 0);
  SHIZUKU_PROG(23, (uint32_t)spawned_consumer.error, (uint32_t)spawned_consumer.value);
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  BOARD::diag_printf("[SELFTEST_PROG] SPAWN consumer: error=%lu value=%lu\n",
                      (unsigned long)spawned_consumer.error,
                      (unsigned long)spawned_consumer.value);
#endif
#endif

  for (uint32_t guard = 0;
       guard < 400000 && (g_producer_done == 0 || g_consumer_done == 0); ++guard) {
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
    if ((guard % 10000) == 0) {
      SHIZUKU_PROG(30, guard, (g_pushed & 0xffffu) | ((g_popped & 0xffffu) << 16));
      BOARD::diag_printf(
          "[SELFTEST_PROG] guard=%lu pushed=%lu popped=%lu prod_done=%lu cons_done=%lu full_hits=%lu\n",
          (unsigned long)guard, (unsigned long)g_pushed, (unsigned long)g_popped,
          (unsigned long)g_producer_done, (unsigned long)g_consumer_done,
          (unsigned long)g_full_hits);
    }
#endif
    api(object_api::YIELD);
  }
  SHIZUKU_PROG(40, (uint32_t)g_pushed, (g_producer_done ? 1u : 0u) | (g_consumer_done ? 2u : 0u));
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  BOARD::diag_printf(
      "[SELFTEST_PROG] guard loop ended: pushed=%lu popped=%lu prod_done=%lu cons_done=%lu\n",
      (unsigned long)g_pushed, (unsigned long)g_popped,
      (unsigned long)g_producer_done, (unsigned long)g_consumer_done);
#endif

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
