#ifndef SHIZUKU_BOARDS_ESP32_C6_HPP
#define SHIZUKU_BOARDS_ESP32_C6_HPP

#include <cstdint>
#include "shizuku/concepts/board.hpp"

// ===========================================================================
//  ESP32-C6 (単核 SoC) 用 board 骨格 (未完成・fail-closed)
// ===========================================================================
//  ★現在の到達点:
//    - 単核 (CORE_COUNT = 1) の定義
//    - launch_core / park_other_cores 等の他コア操作は欺瞞的 no-op を禁止し、
//      panic / 未実装として宣言
//    - 実機ベクタ初期化・SYSTIMER・UART/USB 診断出力は未実装
//    - SHIZUKU_C6_SKELETON_PERMITTED が定義されていない本番ビルドでは
//      明示的にコンパイルを拒否する (fail-closed)。

#if !defined(SHIZUKU_C6_SKELETON_PERMITTED)
#error "ESP32-C6 board backend is incomplete (SYSTIMER, UART/USB, and interrupt matrix are under exploration). Build refused (fail-closed)."
#endif

namespace shizuku {
namespace boards {

class esp32_c6 {
public:
  static constexpr uint32_t CORE_COUNT = 1;

  static void init(uint32_t core);

  // 単核環境でのマルチコア操作: 欺瞞的 no-op 成功を置かず、
  // 呼ばれた場合は不変条件破れとして panic を発生させる契約とする。
  static void launch_core(void (*entry)());
  static void park_other_cores();
  static void resume_other_cores();

  static void diag_mute(bool quiet);
  static int dma_claim();
  static bool dma_busy(int channel);
  static void dma_release(int channel);

  static uint32_t core_num() { return 0; }
  static uint64_t time_us();
  static uint32_t cycles_per_us();
  static uintptr_t unprivileged_floor();

  static void diag_printf(const char *format, ...)
      __attribute__((format(printf, 1, 2)));

  [[noreturn]] static void panic(const char *message);
};

static_assert(shizuku::concepts::board_requires<esp32_c6>,
              "esp32_c6 must satisfy the board concept");

} // namespace boards
} // namespace shizuku

#endif // SHIZUKU_BOARDS_ESP32_C6_HPP
