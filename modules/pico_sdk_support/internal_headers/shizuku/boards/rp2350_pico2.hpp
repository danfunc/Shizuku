#ifndef SHIZUKU_BOARDS_RP2350_PICO2_HPP
#define SHIZUKU_BOARDS_RP2350_PICO2_HPP
#include <cstdint>
#include "pico/stdlib.h"
#include "shizuku/concepts/board.hpp"

namespace shizuku {
namespace boards {

// Raspberry Pi Pico 2 (RP2350, pico-sdk) の board バックエンド。
// 例外ハンドラの結線と優先度、コア識別、時刻を提供する。実装は board.cpp。
class rp2350_pico2 {
public:
  static constexpr uint32_t CORE_COUNT = 2;
  // 呼び出したコアの例外設定を行う。ベクタテーブル (RAM) は両コア共有なので
  // ハンドラ登録は core0 の 1 回だけ、優先度 (SHPR, banked) は各コアで設定する。
  // ★優先度規約: SVC 最優先 > (SysTick) > PendSV 最低 (concepts/arch.hpp 参照)。
  static void init(uint32_t core);
  static uint32_t core_num() { return (uint32_t)::get_core_num(); }
  static uint64_t time_us() { return ::time_us_64(); }
};

static_assert(shizuku::concepts::board_requires<rp2350_pico2>,
              "rp2350_pico2 does not satisfy the board concept");

} // namespace boards
} // namespace shizuku
#endif // SHIZUKU_BOARDS_RP2350_PICO2_HPP
