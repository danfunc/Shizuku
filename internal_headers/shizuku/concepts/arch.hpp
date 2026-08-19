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
// - exception_frame_t : 例外エントリでハードウェアが積むフレーム。中身の並びは ISA の
//                       都合なので、カーネルは下の arg/set_* 経由でしか触らない
// - exc_frame_bytes   : この文脈が積んでいる例外フレームの実サイズ [byte]。
//                       FP 拡張・可変長を含めた「今の」値を返す
// - psp_after_return  : この文脈で例外復帰した「後」のスレッド SP。復帰時に SP へ
//                       追加調整が入る ISA (例: ARMv8-M の xPSR bit9 による +4) の
//                       吸収はここで行う。呼び出しフレーム幾何はこの値だけを信じる
//                       (I-4。見込みを誤ると退避域を踏み潰して無言ロックアップする)
// - normalize_frame   : 「復帰時に SP を追加調整させない」状態へフレームを正規化する。
//                       カーネルが自分で 8B 境界へ置いた作業コピーに適用し、幾何を
//                       ずらさないようにする。適用後は psp_after_return が
//                       frame + exc_frame_bytes と厳密に一致すること
// - arg / set_args / set_result / set_entry :
//                       syscall ABI のスロット (a0..a4 / 戻り a0,a1 / 入口 pc,lr) への
//                       アクセス。番号と引数の載せ場所を ISA 側に閉じ込める
// - set_handler_info  : ハンドラを起こすときにカーネルが渡す情報 (svc 番号と
//                       **今のネスト数**) を、ハンドラが受け取れる場所 (callee-saved
//                       レジスタ等) へ置く。ネスト数は「何段戻すか」の申告に使う
// - return_stub       : ハンドラが普通に return したときの戻り口アドレス。自分の枠を
//                       1 段ぶん巻き戻す (申告には set_handler_info で渡された
//                       ネスト数を使う)
// - object_exit_stub<N> : オブジェクトの戻り口を作る。オブジェクトは RETURN を撃て
//                       ないので、番号 N の exit API を撃って戻る。N はオブジェクト
//                       ランドの持ち物なので外から与える (カーネルも arch も知らない)
// - handler_shim<F>   : ハンドラの ABI シム。callee-saved で渡した情報を C の引数へ
// - current_priv      : 「今の実行が特権か」の自己申告。自己テスト必須項目
//                       (DESIGN §16「状態の主張は対象自身に申告させる」) の実装点
// - set_priv          : 文脈が次に復帰するときの特権状態を設定する (即時ではない)
// - stack_limit_set / stack_limit : 文脈のスタック下限。ハード検出 (PSPLIM) が無い
//                       ISA はソフト検査へ縮退してよいが「下回ったら必ず検出」は守る
// - CALL_HEADROOM     : 呼び出しフレームを積むときスタック下限の手前に残す余裕 [byte]。
//                       呼び先のプロローグが下限を割らないだけの幅を取ること
// - cas32 / store_release32 / load_acquire32 :
//                       マルチコア atomic の継ぎ目 (DESIGN §14.5.3)。同一ベンダでも
//                       チップで実現手段が真逆になるため、必ずここを経由する
// - syscall           : オブジェクト側から見た syscall の発行口 (a0 = 番号)
// - enter_thread_mode : ブート時にスレッドスタックへ移り entry を呼ぶ (戻らない)
// - timer_oneshot / pend_context_switch : 時限実行権 (GRANT) の期限回収に使う
//                       ワンショットタイマと、最低優先度の遅延切替例外の起票。
//                       **GRANT 実装 (Phase 2b) で requires に追加する**。
//   ★前提規約: スレッド切替は「最低優先度の遅延例外」でのみ起こすこと。
//     syscall 例外 > タイマ例外 > 切替例外 の優先度順を board/arch 初期化で保証する。
//     これが 1 コア内の相互排除を無償で与える (DESIGN §14.5.1)。移植時に静かに
//     失われやすいので、初期化後に自己テストで優先度を読み戻して検算すること。
template <typename ARCH>
concept arch_requires =
    requires(typename ARCH::context_t &context,
             const typename ARCH::context_t &const_context,
             typename ARCH::exception_frame_t &frame,
             const typename ARCH::exception_frame_t &const_frame,
             const uintptr_t *args, volatile uint32_t *shared_word,
             uint32_t value, uintptr_t address, unsigned index, bool flag) {
      typename ARCH::context_t;
      typename ARCH::exception_frame_t;
      typename ARCH::method_t;
      { ARCH::exc_frame_bytes(const_context) } -> std::same_as<uint32_t>;
      { ARCH::psp_after_return(const_context) } -> std::same_as<uintptr_t>;
      { ARCH::normalize_frame(frame) };
      { ARCH::arg(const_frame, index) } -> std::same_as<uintptr_t>;
      { ARCH::set_args(frame, args) };
      { ARCH::set_result(frame, address, address) };
      { ARCH::set_entry(frame, address, address) };
      { ARCH::set_handler_info(context, address, address) };
      { ARCH::return_stub() } -> std::same_as<uintptr_t>;
      { ARCH::current_priv() } -> std::same_as<bool>;
      { ARCH::set_priv(context, flag) };
      { ARCH::stack_limit_set(context, address) };
      { ARCH::stack_limit(const_context) } -> std::same_as<uintptr_t>;
      { ARCH::CALL_HEADROOM } -> std::convertible_to<uint32_t>;
      { ARCH::cas32(shared_word, value, value) } -> std::same_as<bool>;
      { ARCH::store_release32(shared_word, value) };
      { ARCH::load_acquire32(shared_word) } -> std::same_as<uint32_t>;
      { ARCH::syscall(address, address, address, address, address) };
    };

} // namespace concepts
} // namespace shizuku
#endif // SHIZUKU_CONCEPTS_ARCH_HPP
