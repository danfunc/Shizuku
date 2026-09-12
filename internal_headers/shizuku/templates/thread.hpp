#ifndef SHIZUKU_TEMPLATE_THREAD_HPP
#define SHIZUKU_TEMPLATE_THREAD_HPP
#include <cstdint>
#include "shizuku/kernel_abi.hpp"
namespace shizuku {
namespace templates {

// スレッド = カーネルが知る唯一の実行単位 (DESIGN §5)。
// ★カーネルはオブジェクトの**台帳**を知らない (D1)。ここにあるのは「今どのオブジェクト
//   として、どの種別で走っているか」という文脈だけで、誰が何を export しているか・
//   誰が誰の親かといった話は全部カーネルオブジェクトの側にある。
//   ★2026-09-05: identity を文脈として持つこと自体は設計判断であって D1 違反ではない、
//     と方針を緩めた。緩めていないのは「役を ID で代用しない」という一点。
template <typename CONTEXT> struct thread {
  enum struct state_t : uint32_t {
    UNINITIALIZED = 0,
    // 枠を確保したが初期化が終わっていない過渡状態。READY でないのでどの
    // スケジューラにも拾われない (「見てから作る」の TOCTOU を CAS で閉じるため)。
    RESERVED,
    READY,
    RUNNING,
    SUSPENDED,
    // 実行権を貸して復帰を待っている。READY ではないので他コアに拾われず、
    // 復帰は貸した側のコアの巻き取り経路だけ。
    WAIT_GRANT,
    TERMINATED, // 走り終えた (メソッドが return して戻り先が無かった)
  };

  // 呼び出しフレームのスタック。実体はスレッド自身のスタック上にあり、ここは
  // 最内ヘッダのアドレスと段数だけを持つ (DESIGN §8.1)。
  struct call_stack_t {
    uintptr_t top = 0;  // 最内 call_frame_header のアドレス (0 = 空)
    uint32_t depth = 0; // 巻き戻しの「今のネスト数」検算に使う
  };

  // ★state は CAS の対象なので 32bit 幅を固定する (ARCH::cas32 が uint32_t* で叩く)。
  uint32_t state = (uint32_t)state_t::UNINITIALIZED;
  CONTEXT *context = nullptr;
  call_stack_t call_stack;
  uint32_t affinity = 0b1; // bit0 = core0 (どのコアで走ってよいか)
  uint32_t current_object = 0;
  // ★今走っているオブジェクトの**種別**。ID とは独立に持つ (ID は名前であって
  //   役ではない)。svc の経路も、取り上げを見送るかどうかも、これだけで決まる。
  //   遷移させるのはカーネルだけ: CALL で呼び先の申告を載せ、戻るときに
  //   呼び出しフレームのヘッダから読み戻す。
  uint32_t current_kind = (uint32_t)object_kind::PLAIN;
  // ★今走っているオブジェクトの親 handling object の情報 (解決済み binding)
  uint32_t current_handler_object = 0;
  uintptr_t current_handler_entry = 0;
  // ★枠が使い回されたことを外から見分けるための番号。release のたびに 1 進む。
  //   スレッド番号だけを控えていると、控えた相手が終わって同じ番号に別の
  //   スレッドが入ったとき、**控えた側は気づけない** (デバッガが止めたつもりの
  //   相手が既に別人、という形で効く)。番号 + 世代なら食い違いが検出できる。
  uint32_t generation = 0;
  bool is_debug_protected = false; // GDB stub/agent bypass
  // ★sleep の起床時刻やスケジューリングの優先度はここに無い。それは方針なので
  //   カーネルオブジェクトが自分の表で持つ (D1)。

  bool is_state(state_t expected) const { return state == (uint32_t)expected; }
  void set_state(state_t next) { state = (uint32_t)next; }
};

} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATE_THREAD_HPP
