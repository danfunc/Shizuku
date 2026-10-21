// boards/esp32_c6/heartbeat_logic.hpp の判定だけを試験する (レジスタには触れない)。
#include <cstdint>
#include <cstdio>
#include "boards/esp32_c6/heartbeat_logic.hpp"

namespace hb = shizuku::boards::esp32_c6_heartbeat;

#define CHECK(expr)                                                  \
  do {                                                               \
    if (!(expr)) {                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
      return 1;                                                      \
    }                                                                \
  } while (0)

int main() {
  // 16 MHz SYSTIMER で 0.25 s。
  CHECK(hb::PERIOD_TICKS == 4'000'000u);

  // 周期未満では反転しない (一歩手前 3'999'999 tick)。
  hb::state_t s{0, false};
  CHECK(!hb::step(s, 3'999'999u, true));
  CHECK(s.led_on == false);
  CHECK(s.last_toggle == 0u);

  // ちょうど周期で反転し、時刻を記録する。
  CHECK(hb::step(s, 4'000'000u, true));
  CHECK(s.led_on == true);
  CHECK(s.last_toggle == 4'000'000u);

  // 8 周期ぶん呼ぶと毎回反転し、点灯状態は奇数回目で on になる。
  hb::state_t seq{0, false};
  for (uint64_t k = 1; k <= 8; ++k) {
    CHECK(hb::step(seq, k * hb::PERIOD_TICKS, true));
    CHECK(seq.led_on == (k % 2 == 1));
  }

  // 無効化されているときは時刻が十分過ぎても何もしない (状態も変えない)。
  hb::state_t off{0, false};
  CHECK(!hb::step(off, 10 * hb::PERIOD_TICKS, false));
  CHECK(off.led_on == false);
  CHECK(off.last_toggle == 0u);

  // 64bit の折り返しをまたいでも、差は 0.25 s で正しく測れる。
  // last = 2^64 - 11, now = 3'999'989 (折り返し後) → now - last = 4'000'000。
  hb::state_t wrap{UINT64_MAX - 10u, false};
  CHECK(!hb::step(wrap, 3'999'988u, true));
  CHECK(hb::step(wrap, 3'999'989u, true));
  CHECK(wrap.led_on == true);

  std::puts("PASS: heartbeat_logic period, toggle sequence, disabled no-op, wrap-safe difference");
  return 0;
}
