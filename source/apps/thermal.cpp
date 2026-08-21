// ===========================================================================
//  温度の履歴アプリ (設計の意図は shizuku/apps/thermal.hpp を参照)
// ===========================================================================
#include "shizuku/apps/thermal.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/peripherals.hpp"

namespace shizuku {
namespace apps {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

// 10Hz × 3000 = 5 分。1 標本 8 バイトなので 24KB — オブジェクト用 arena に収まる。
constexpr uint32_t PERIOD_US = 100000;
constexpr uint32_t CAPACITY = 3000;
constexpr uint32_t REPORT_EVERY = 300; // 30 秒ごとに揺らぎを報告する

struct call_result {
  uintptr_t error;
  uintptr_t value;
};

call_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
                uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

thermal_sample *g_ring = nullptr;
// ★書き込み番号は**巻き戻さない**。剰余で場所を出すので、番号そのものが
//   「何個目か」を表し、読み手はこれを見て追い越されたかどうかを判断できる。
volatile uint32_t g_written = 0;
volatile uint32_t g_late_max = 0;
volatile uint64_t g_late_sum = 0;
volatile int32_t g_latest = 0;

int32_t read_temperature() {
  const auto result =
      api(object_api::CALL_METHOD, objects::TEMPERATURE_OBJECT,
          (uintptr_t)objects::temperature_method::READ, 0);
  return (int32_t)result.value;
}

// 周期サンプラ。★絶対グリッドで待つ — 相対で待つとズレが積み上がって、
//   「遅れ」を測ること自体ができなくなる (揺らぎが見えなくなるのではなく、
//   遅れが周期そのものに化ける)。
uintptr_t sampler(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  api(object_api::DECLARE_NAME, (uintptr_t) "thermal");
  uint64_t next = BOARD::time_us() + PERIOD_US;
  uint32_t since_report = 0;

  while (true) {
    const int64_t remaining = (int64_t)(next - BOARD::time_us());
    if (remaining > 0)
      api(object_api::SLEEP_US, (uintptr_t)remaining);

    const uint64_t now = BOARD::time_us();
    const uint32_t late = now > next ? (uint32_t)(now - next) : 0;
    if (late > g_late_max)
      g_late_max = late;
    g_late_sum += late;

    const int32_t centi = read_temperature();
    g_latest = centi;
    // ★書くのは 1 スレッドだけなので、番号を最後に進めれば読み手は「番号が
    //   進んでいる = 中身も揃っている」と読める。逆順にすると、中身が揃う前の
    //   場所を読ませてしまう。
    const uint32_t slot = g_written % CAPACITY;
    g_ring[slot].at_ms = (uint32_t)(now / 1000);
    g_ring[slot].centi_celsius = centi;
    ARCH::store_release32((volatile uint32_t *)&g_written, g_written + 1);

    next += PERIOD_US;
    if (++since_report >= REPORT_EVERY) {
      since_report = 0;
      const uint32_t count = g_written;
      BOARD::diag_printf(
          "[THERMAL] %ld.%02ld C | %lu samples | late mean %lu us max %lu us "
          "(period %lu us)\n",
          (long)(centi / 100), (long)(centi < 0 ? -centi : centi) % 100,
          (unsigned long)count,
          (unsigned long)(count ? (uint32_t)(g_late_sum / count) : 0),
          (unsigned long)g_late_max, (unsigned long)PERIOD_US);
    }
  }
  return 0;
}

// 履歴を引く。★リングは古いものから必ず消えるので、**読んでいる最中に足元を
//   書き換えられる**。書き込み番号を前後で見比べ、追い越されていたらやり直す
//   (seqlock と同じ考え方)。やり直しは有界: 読む量より速く書かれ続けない限り必ず終わる。
uintptr_t history(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  thermal_query *request = (thermal_query *)argument;
  if (request == nullptr || request->into == nullptr || g_ring == nullptr)
    return 0;
  request->lost = 0;

  for (uint32_t attempt = 0; attempt < 8; ++attempt) {
    const uint32_t before = ARCH::load_acquire32((volatile uint32_t *)&g_written);
    uint32_t want = (request->seconds * 1000000u) / PERIOD_US;
    if (want > request->capacity)
      want = request->capacity;
    if (want > CAPACITY)
      want = CAPACITY;
    if (want > before)
      want = before;
    const uint32_t first = before - want; // 何個目から写すか

    for (uint32_t index = 0; index < want; ++index)
      request->into[index] = g_ring[(first + index) % CAPACITY];

    // ★写している間に、写した中で**一番古いもの**が上書きされていないか。
    //   書き込み番号が CAPACITY 以上進んでいたら、その古い側は既に別物。
    const uint32_t after = ARCH::load_acquire32((volatile uint32_t *)&g_written);
    if (after - first <= CAPACITY) {
      request->count = want;
      return want;
    }
    ++request->lost; // 追い越された。取り直す
  }
  request->count = 0;
  return 0;
}

uintptr_t stats(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  thermal_stats *request = (thermal_stats *)argument;
  if (request == nullptr)
    return 0;
  const uint32_t count = g_written;
  request->period_us = PERIOD_US;
  request->samples = count;
  request->late_mean_us = count ? (uint32_t)(g_late_sum / count) : 0;
  request->late_max_us = g_late_max;
  request->capacity = CAPACITY;
  request->held = count < CAPACITY ? count : CAPACITY;
  request->latest_centi = g_latest;
  return count;
}

// 履歴を定期的に引く読み手。★サンプラとは別スレッドなので、引いている最中に
//   書かれ得る = 追い越しの検出が実際に試される経路になる。
constexpr uint32_t READ_SECONDS = 60;
thermal_sample g_reader_buffer[READ_SECONDS * 1000000u / PERIOD_US];

uintptr_t reader(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  while (true) {
    api(object_api::SLEEP_US, 20000000u); // 20 秒ごと
    thermal_query query{};
    query.seconds = READ_SECONDS;
    query.into = g_reader_buffer;
    query.capacity = sizeof(g_reader_buffer) / sizeof(g_reader_buffer[0]);
    api(object_api::CALL_METHOD, THERMAL_OBJECT,
        (uintptr_t)thermal_method::HISTORY, (uintptr_t)&query);
    if (query.count == 0) {
      BOARD::diag_printf("[THERMAL] history query returned nothing\n");
      continue;
    }
    const thermal_sample &oldest = g_reader_buffer[0];
    const thermal_sample &newest = g_reader_buffer[query.count - 1];
    int32_t lowest = oldest.centi_celsius;
    int32_t highest = oldest.centi_celsius;
    for (uint32_t index = 1; index < query.count; ++index) {
      const int32_t value = g_reader_buffer[index].centi_celsius;
      if (value < lowest)
        lowest = value;
      if (value > highest)
        highest = value;
    }
    BOARD::diag_printf(
        "[THERMAL] last %lu s: %lu samples spanning %lu ms, %ld.%02ld..%ld.%02ld C"
        " (retries %lu)\n",
        (unsigned long)READ_SECONDS, (unsigned long)query.count,
        (unsigned long)(newest.at_ms - oldest.at_ms), (long)(lowest / 100),
        (long)(lowest % 100), (long)(highest / 100), (long)(highest % 100),
        (unsigned long)query.lost);
  }
  return 0;
}

uintptr_t thermal_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  uintptr_t failures = 0;
  failures += api(object_api::EXPORT_METHOD, (uintptr_t)thermal_method::HISTORY,
                  (uintptr_t)&history)
                  .error;
  failures += api(object_api::EXPORT_METHOD, (uintptr_t)thermal_method::STATS,
                  (uintptr_t)&stats)
                  .error;
  failures += api(object_api::EXPORT_METHOD, (uintptr_t)thermal_method::SAMPLER,
                  (uintptr_t)&sampler)
                  .error;
  failures += api(object_api::EXPORT_METHOD, (uintptr_t)thermal_method::READER,
                  (uintptr_t)&reader)
                  .error;
  return failures;
}

} // namespace

uint32_t start_thermal() {
  // ★記憶はオブジェクトランドから借りる。静的配列にしないのは、非特権で走れる
  //   オブジェクトになったときに静的領域が見えなくなるため (DESIGN §11.2.2)。
  const auto memory = api(object_api::MEMORY_ALLOCATE,
                          sizeof(thermal_sample) * CAPACITY);
  if (memory.value == 0) {
    BOARD::diag_printf("[THERMAL] no room for %lu samples\n",
                       (unsigned long)CAPACITY);
    return 1;
  }
  g_ring = (thermal_sample *)memory.value;

  const auto created = api(object_api::CREATE_OBJECT, THERMAL_OBJECT,
                           (uintptr_t)&thermal_main, 0);
  const auto started = api(object_api::CALL_METHOD, THERMAL_OBJECT, 0, 0);
  if (created.error != 0 || started.error != 0 || started.value != 0) {
    BOARD::diag_printf("[THERMAL] FAILED: create=%lu call=%lu exports=%lu\n",
                       (unsigned long)created.error,
                       (unsigned long)started.error,
                       (unsigned long)started.value);
    return 1;
  }
  // ★サンプラを別スレッドで走らせる。**method 0 (main) を起こしてはいけない** —
  //   main は export して戻るだけなので、スレッドが即終わる。最初これを間違えて
  //   「起動したのに 100 秒待っても 1 行も出ない」になった。起こすのは SAMPLER。
  const auto spawned = api(object_api::SPAWN, THERMAL_OBJECT,
                           (uintptr_t)thermal_method::SAMPLER, 0);
  if (spawned.error != 0) {
    BOARD::diag_printf("[THERMAL] could not spawn the sampler (%lu)\n",
                       (unsigned long)spawned.error);
    return 1;
  }
  api(object_api::SPAWN, THERMAL_OBJECT, (uintptr_t)thermal_method::READER, 0);
  BOARD::diag_printf(
      "[THERMAL] sampling every %lu us into %lu slots (%lu s of history), "
      "thread %lu\n",
      (unsigned long)PERIOD_US, (unsigned long)CAPACITY,
      (unsigned long)((uint64_t)CAPACITY * PERIOD_US / 1000000u),
      (unsigned long)spawned.value);
  return 0;
}

} // namespace apps
} // namespace shizuku
