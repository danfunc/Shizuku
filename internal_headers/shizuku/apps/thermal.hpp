#ifndef SHIZUKU_APPS_THERMAL_HPP
#define SHIZUKU_APPS_THERMAL_HPP
#include <cstdint>
#include "shizuku/object_ids.hpp"

// ===========================================================================
//  温度の履歴アプリ — 直近数分ぶんを一定周期で取り、後から引けるようにする
// ===========================================================================
//  ★これは「オブジェクトとして普通に書けるか」の実例でもある。特権は要らず
//    (温度を読むのはペリフェラルオブジェクトの仕事)、記憶はオブジェクトランドから
//    借り (MEMORY_ALLOCATE)、周期は SLEEP_US で作る。カーネルには何も足していない。
//
//  ★リングバッファの厄介さ (2026-08-21 ユーザー指摘:「リングバッファで使う時この
//    特性は邪魔なんだよね」) に正面から答える場所でもある。古い記録は必ず消えるので、
//    **読んでいる最中に足元を書き換えられる**。ここでは書き込み番号を読み取りの
//    前後で見比べ、追い越されていたらやり直す (seqlock と同じ考え方)。
//    「アプリ層でなんとかする」形にしないのは、これを間違えると**静かに壊れた値**が
//    出てくるため — 静かに壊れるものは、持ち主の側で塞ぐ。
namespace shizuku {
namespace apps {

constexpr uintptr_t THERMAL_OBJECT = object_id::thermal;

enum struct thermal_method : uintptr_t {
  MAIN = 0,
  HISTORY = 1, // a0 = thermal_query*
  STATS = 2,   // a0 = thermal_stats*
  // ★周期サンプラ。**別スレッドとして起こすためのメソッド**で、呼び出しては
  //   ならない (戻らないので、呼んだスレッドがそのままサンプラになる)。
  SAMPLER = 3,
  // ★履歴を定期的に引いて印字する読み手。**サンプラとは別スレッド**として
  //   起こす — 同じスレッドから引いたのでは、リングの追い越しが起こり得ないので
  //   検出の仕組みを何も確かめられない。
  READER = 4,
};

// 1 標本。★時刻も一緒に持つ。周期がずれたときに「いつの値か」が分からないと、
//   揺らぎを測ることも、後から読む側が間隔を仮定することもできなくなる。
struct thermal_sample {
  uint32_t at_ms;      // 起動からの経過 [ms]
  int32_t centi_celsius; // 摂氏の 1/100
};

struct thermal_query {
  uint32_t seconds;      // [in] さかのぼる秒数
  thermal_sample *into;  // [in] 呼び出し側の受け皿
  uint32_t capacity;     // [in] 受け皿の個数
  uint32_t count;        // [out] 実際に書いた個数 (古い順)
  uint32_t lost;         // [out] 追い越されてやり直した回数
};

// 周期の揺らぎ。★「動いている」ではなく「どれだけ外したか」を出す。
struct thermal_stats {
  uint32_t period_us;
  uint32_t samples;
  uint32_t late_mean_us;
  uint32_t late_max_us;
  uint32_t capacity;    // 何個ぶん覚えていられるか
  uint32_t held;        // 今持っている個数
  int32_t latest_centi; // 最新の温度
};

// 生成してサンプラを起こす。0 = 成功。
uint32_t start_thermal();

} // namespace apps
} // namespace shizuku
#endif // SHIZUKU_APPS_THERMAL_HPP
