#ifndef SHIZUKU_SELFTEST_HPP
#define SHIZUKU_SELFTEST_HPP
#include <cstdint>

// カーネルの自己テスト (DESIGN §16)。破っても静かに壊れる不変条件はここで守る。
// ★梯子式に組むこと: 1 段の最小プローブ → FP 活性 → N 段ネスト → 異常系。
//   いきなり多段を試すと、1 段目の不具合と往復の不具合を切り分けられない。
namespace shizuku {
namespace selftest {

// ブート活性化 (スレッド 0) の cookie。呼ばれた側が「誰に呼ばれたか」を突き合わせる
// ための既知値。
constexpr uintptr_t BOOT_COOKIE = 0x7E57E200;

// CALL / RETURN の梯子。結果は BOARD::diag_printf へ出す。
void call_ladder();

} // namespace selftest
} // namespace shizuku
#endif // SHIZUKU_SELFTEST_HPP
