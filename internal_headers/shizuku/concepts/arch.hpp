#ifndef SHIZUKU_CONCEPTS_ARCH_HPP
#define SHIZUKU_CONCEPTS_ARCH_HPP
#include <concepts>
#include <cstdint>
namespace shizuku {
namespace concepts {

// arch = ISA 依存層の要件 (docs/03_porting_policy.md D2, PORT §5.1)。
// ISA ごとに書き直すのはこの concept を満たすクラス 1 つ (+ 文脈退避 asm) だけ。
//
// ★意味の契約 (関数を「用意したが意味が違う」形の破れを防ぐため、ここに明記する):
// - context_t         : スレッド 1 本ぶんの退避文脈。文脈退避 asm と同一レイアウトで
//                       あること (実装側で static_assert によりオフセットを両縛りする)
// - exc_frame_bytes   : 例外エントリでハードウェアが積んだフレームの実サイズ [byte]。
//                       FP 拡張・ISA 固有の可変長を含めた「今この文脈の」値を返す
// - psp_after_return  : この文脈で例外復帰した「後」のスレッド SP。復帰時に SP へ
//                       追加調整が入る ISA (例: ARMv8-M の xPSR bit9 による +4) の
//                       吸収はここで行う。呼び出しフレーム幾何はこの値だけを信じる
//                       (I-4。見込みを誤ると退避域を踏み潰して無言ロックアップする)
// - current_priv      : 「今の実行が特権か」の自己申告。自己テスト必須項目
//                       (DESIGN §16「状態の主張は対象自身に申告させる」) の実装点
// - set_priv          : 文脈が次に復帰するときの特権状態を設定する (即時ではない)
// - stack_limit_set   : 文脈のスタック下限。ハード検出 (PSPLIM) が無い ISA は
//                       ソフト検査へ縮退してよいが「下回ったら必ず検出」は守ること
// - cas32 / store_release32 / load_acquire32 :
//                       マルチコア atomic の継ぎ目 (DESIGN §14.5.3)。同一ベンダでも
//                       チップで実現手段が真逆になるため、必ずここを経由する
// - timer_oneshot / timer_cancel / pend_context_switch :
//                       時限実行権 (GRANT) の期限回収に使うワンショットタイマと、
//                       最低優先度の遅延切替例外の起票。**GRANT 実装 (Phase 2) で
//                       requires に追加する** (未実装のうちは要件化しない)。
//   ★前提規約: スレッド切替は「最低優先度の遅延例外」でのみ起こすこと。
//     syscall 例外 > タイマ例外 > 切替例外 の優先度順を board/arch 初期化で保証する。
//     これが 1 コア内の相互排除を無償で与える (DESIGN §14.5.1)。移植時に静かに
//     失われやすいので、初期化後に自己テストで優先度を読み戻して検算すること。
template <typename ARCH>
concept arch_requires =
    requires(typename ARCH::context_t &context, const typename ARCH::context_t &const_context,
             volatile uint32_t *shared_word, uint32_t value, uintptr_t address,
             bool flag) {
      typename ARCH::context_t;
      typename ARCH::exception_frame_t;
      typename ARCH::method_t;
      { ARCH::exc_frame_bytes(const_context) } -> std::same_as<uint32_t>;
      { ARCH::psp_after_return(const_context) } -> std::same_as<uintptr_t>;
      { ARCH::current_priv() } -> std::same_as<bool>;
      { ARCH::set_priv(context, flag) };
      { ARCH::stack_limit_set(context, address) };
      { ARCH::cas32(shared_word, value, value) } -> std::same_as<bool>;
      { ARCH::store_release32(shared_word, value) };
      { ARCH::load_acquire32(shared_word) } -> std::same_as<uint32_t>;
    };

} // namespace concepts
} // namespace shizuku
#endif // SHIZUKU_CONCEPTS_ARCH_HPP
