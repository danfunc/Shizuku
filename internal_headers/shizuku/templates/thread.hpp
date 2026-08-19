#ifndef SHIZUKU_TEMPLATE_THREAD_HPP
#define SHIZUKU_TEMPLATE_THREAD_HPP
#include <cstdint>
namespace shizuku {
namespace templates {

// スレッド = カーネルが知る唯一の実行単位 (DESIGN §5)。
// ★カーネルはオブジェクトを知らない (D1)。「今どのオブジェクトとして走っているか」は
//   不透明な cookie でしか持たない — 中身の意味づけはカーネルオブジェクトの仕事。
template <typename CONTEXT> struct thread {
  enum struct state_t : uint32_t {
    UNINITIALIZED = 0,
    // 枠を確保したが初期化が終わっていない過渡状態。READY でないのでどの
    // スケジューラにも拾われない (「見てから作る」の TOCTOU を CAS で閉じるため)。
    RESERVED,
    READY,
    RUNNING,
    SUSPENDED,
    WAIT_GRANT, // 実行権を貸して復帰を待っている (Phase 2b)
  };

  // 呼び出しフレームのスタック。実体はスレッド自身のスタック上にあり、ここは
  // 最内ヘッダのアドレスと段数だけを持つ (DESIGN §8.1)。
  struct call_stack_t {
    uintptr_t top = 0; // 最内 call_frame_header のアドレス (0 = 空)
    uint32_t depth = 0; // RETURN の「今のネスト数」検算に使う
  };

  // ★state は CAS の対象なので 32bit 幅を固定する (ARCH::cas32 が uint32_t* で叩く)。
  uint32_t state = (uint32_t)state_t::UNINITIALIZED;
  CONTEXT *context = nullptr;

  // ---- 活性化の状態 (呼び出しのたびに差し替わり、フレームヘッダへ退避される) ----
  uintptr_t cookie = 0;        // 現在オブジェクト (カーネルには不透明)
  uintptr_t caller_cookie = 0; // 誰に呼ばれたか (identity。DESIGN §12.1 の修復点)
  // 信頼された活性化か。**経路判定に使う唯一のビット** (I-1)。
  // ハンドラとして走っていることは特権を意味しない (I-8) ので、委譲された
  // サブハンドラにはこれを立てない。
  bool trusted = false;

  call_stack_t call_stack;
  uint32_t affinity = 0b1; // bit0 = core0
  uint64_t wake_at = 0;    // sleep 中の起床時刻 (Phase 2b)

  bool is_state(state_t expected) const { return state == (uint32_t)expected; }
  void set_state(state_t next) { state = (uint32_t)next; }
};

} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATE_THREAD_HPP
