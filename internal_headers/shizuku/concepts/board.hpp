#ifndef SHIZUKU_CONCEPTS_BOARD_HPP
#define SHIZUKU_CONCEPTS_BOARD_HPP
#include <concepts>
#include <cstdint>
namespace shizuku {
namespace concepts {

// board = ボード/SoC 依存層の要件 (docs/03_porting_policy.md D2, PORT §5)。
// コア識別・時刻・例外ハンドラの結線・(将来) コア起動・クロック API を持つ。
//
// - init(core)   : 例外ハンドラ (svc/切替/タイマ) の登録と優先度設定。
//                  ★優先度は「syscall > タイマ > 切替 (最低)」を必ず守る
//                  (concepts/arch.hpp の前提規約を参照)。ベクタテーブルが全コア
//                  共有の場合、登録自体は 1 回に抑える責務も board が持つ
// - core_num()   : 自コア番号 (0 起点)
// - time_us()    : 単調増加のマイクロ秒時刻 (全コア共通基準)
// ★クロック変更はこの層の API として実装し、固定分周ハードコードの追従まで
//   board が面倒を見ること (PORT §2.3)。上位はクロックを直接触らない。
template <typename BOARD>
concept board_requires = requires(uint32_t core) {
  { BOARD::init(core) };
  { BOARD::core_num() } -> std::same_as<uint32_t>;
  { BOARD::time_us() } -> std::same_as<uint64_t>;
};

} // namespace concepts
} // namespace shizuku
#endif // SHIZUKU_CONCEPTS_BOARD_HPP
