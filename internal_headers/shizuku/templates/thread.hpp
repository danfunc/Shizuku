#ifndef SHIZUKU_TEMPLATE_THREAD_HPP
#define SHIZUKU_TEMPLATE_THREAD_HPP
#include <cstdint>
namespace shizuku {
namespace templates {

// スレッド = カーネルが知る唯一の実行単位 (DESIGN §5)。
// ★カーネルはオブジェクトを知らない (D1)。「どのオブジェクトとして走っているか」は
//   ここに無い — それはカーネルオブジェクトの台帳の話で、カーネルは呼び出しフレームの
//   積み下ろししか知らない。
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
