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

// 梯子の通算結果。★起動時の出力はホストが繋ぐ前に流れて消えることがあるので
// (PORT §7 の罠)、**いつ繋いでも結果が分かる**ように生存表示へ載せる。
extern uint32_t passed;
extern uint32_t failed;

// メソッド呼び出しの梯子。結果は BOARD::diag_printf へ出す。
void call_ladder();

// スレッドと実行権の梯子: 1 本起こす → 譲り合う → 時限つきで貸す → 返さない相手を
// 取り上げる。**取り上げが効くこと**が「1 つの暴走が全系を凍らせない」の証拠になる。
void thread_ladder();

// 記憶の貸し借りが定数時間であることの実測。★空きの散らばり方を変えても費用が
// 変わらないことを見る — 「O(1) にした」は主張であって、証拠はこの差の無さのほう。
void memory_ladder();

// 非特権オブジェクトが本当に非特権で走っているかを、対象自身に申告させて確かめる。
void unprivileged_probe();
// プローブが自分で読んで持ち帰った CONTROL。定期報告に載せて、いつ繋いでも
// 「本当に非特権で走ったのか」を確かめられるようにする。
extern uintptr_t unprivileged_control;
extern uintptr_t privileged_control;

// 負荷試験: ランダムな長さの仕事をするスレッドを何本か走らせ、周期スレッドが
// 締切をどれだけ外すか (揺らぎ) を測り続ける。目視ではなく数字で見る。
void stress_launch();

} // namespace selftest
} // namespace shizuku
#endif // SHIZUKU_SELFTEST_HPP
