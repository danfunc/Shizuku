#ifndef SHIZUKU_BOARDS_ESP32_C6_HEARTBEAT_LOGIC_HPP
#define SHIZUKU_BOARDS_ESP32_C6_HEARTBEAT_LOGIC_HPP

#include <cstdint>
#include "chips/esp32_c6/systimer.hpp"

// ユーザー LED の点滅判定だけを置く。レジスタには触れないので、ホストでそのまま試験できる。
namespace shizuku::boards::esp32_c6_heartbeat {

// 0.25 s ごとに反転 (2 Hz 点滅)。SYSTIMER tick 数は chips 側の TICKS_PER_US に揃える。
constexpr uint64_t PERIOD_TICKS = shizuku::ports::c6::TICKS_PER_US * 250'000u;

struct state_t {
  uint64_t last_toggle;
  bool led_on;
};

// 前回の反転から PERIOD_TICKS 以上経っていれば反転して true。無効なら何もしない。
// 差は符号なしで取るので、64bit 値の折り返しがあっても正しく測れる。
inline bool step(state_t &state, uint64_t now, bool enabled) {
  if (!enabled) return false;
  if (now - state.last_toggle < PERIOD_TICKS) return false;
  state.last_toggle = now;
  state.led_on = !state.led_on;
  return true;
}

} // namespace shizuku::boards::esp32_c6_heartbeat

#endif // SHIZUKU_BOARDS_ESP32_C6_HEARTBEAT_LOGIC_HPP
