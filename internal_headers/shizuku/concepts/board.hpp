#ifndef SHIZUKU_CONCEPTS_BOARD_HPP
#define SHIZUKU_CONCEPTS_BOARD_HPP
#include <concepts>
#include <cstdint>
namespace shizuku {
namespace concepts {

// board = ボード/SoC 依存層の要件 (docs/03_porting_policy.md D2, PORT §5)。
// コア識別・時刻・例外ハンドラの結線・診断出力・(将来) コア起動とクロック API。
//
// - init(core)     : 例外ハンドラ (svc/切替/タイマ) の登録と優先度設定。
//                    ★優先度は「syscall > タイマ > 切替 (最低)」を必ず守る
//                    (concepts/arch.hpp の前提規約を参照)。ベクタテーブルが全コア
//                    共有の場合、登録自体を 1 回に抑える責務も board が持つ
// - core_num()     : 自コア番号 (0 起点)
// - time_us()      : 単調増加のマイクロ秒時刻 (全コア共通基準)
// - diag_printf()  : 診断出力。**系が固まっても出る経路**であること (PORT §7)。
//                    カーネルと自己テストはここにしか出力しない
// - panic()        : カーネル自身の不変条件が壊れたときだけ呼ぶ (D12 / I-9)。
//                    オブジェクトの誤りでは決して呼ばない
// ★クロック変更はこの層の API として実装し、固定分周ハードコードの追従まで
//   board が面倒を見ること (PORT §2.3)。上位はクロックを直接触らない。
template <typename BOARD>
concept board_requires = requires(uint32_t core, const char *text) {
  { BOARD::init(core) };
  { BOARD::core_num() } -> std::same_as<uint32_t>;
  { BOARD::time_us() } -> std::same_as<uint64_t>;
  { BOARD::diag_printf(text) };
  { BOARD::panic(text) };
};

} // namespace concepts
} // namespace shizuku
#endif // SHIZUKU_CONCEPTS_BOARD_HPP
