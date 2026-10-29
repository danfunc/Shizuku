#ifndef SHIZUKU_LED_BLINK_BLINK_LOGIC_HPP
#define SHIZUKU_LED_BLINK_BLINK_LOGIC_HPP

#include <cstdint>

// 点滅の時刻管理だけを置く。レジスタにも syscall にも触れないので、ホストでそのまま試験できる。
// 締切は絶対グリッド (next += period) で積む。1 回の遅れを次の周期へ持ち越さない。
// 差は符号付きで取るので、64bit 値の折り返しをまたいでも正しく測れる。
namespace shizuku::led_blink {

// 既定の点滅周期 [us] (0.25 s ごとに反転 = 2 Hz)。全ボード・selftest の点滅はこれを使う。
constexpr uint64_t DEFAULT_PERIOD_US = 250000;

struct schedule_t {
  uint64_t next_us; // 次に LED を反転させる時刻
  bool on;          // 今の LED の論理値
};

// 締切まで待つ時間 [us]。締切を過ぎていれば 0。
inline uint64_t wait_us(const schedule_t &s, uint64_t now_us) {
  const int64_t remaining = (int64_t)(s.next_us - now_us);
  return remaining > 0 ? (uint64_t)remaining : 0;
}

// 締切を過ぎた量 [us] (遅れ)。締切前なら 0。
inline uint64_t late_us(const schedule_t &s, uint64_t now_us) {
  const int64_t late = (int64_t)(now_us - s.next_us);
  return late > 0 ? (uint64_t)late : 0;
}

// 1 周期進める: LED を反転し、締切だけを period 後ろへ。
inline void advance(schedule_t &s, uint64_t period_us) {
  s.on = !s.on;
  s.next_us += period_us;
}

} // namespace shizuku::led_blink

#endif // SHIZUKU_LED_BLINK_BLINK_LOGIC_HPP
