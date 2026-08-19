#ifndef SHIZUKU_SELFTEST_HPP
#define SHIZUKU_SELFTEST_HPP
#include <cstdint>

// カーネルの自己テスト (DESIGN §16)。破っても静かに壊れる不変条件はここで守る。
// ★梯子式に組むこと: 1 段の最小プローブ → FP 活性 → N 段ネスト → 異常系。
//   いきなり多段を試すと、1 段目の不具合と往復の不具合を切り分けられない。
// ★テストはオブジェクトランドの API しか使わない (カーネルのプリミティブは
//   撃てない)。つまり検査するのは近道ではなく実経路そのものになる。
namespace shizuku {
namespace selftest {

// メソッド呼び出しの梯子。結果は BOARD::diag_printf へ出す。
void call_ladder();

} // namespace selftest
} // namespace shizuku
#endif // SHIZUKU_SELFTEST_HPP
