#ifndef SHIZUKU_TEMPLATE_THREAD_HPP
#define SHIZUKU_TEMPLATE_THREAD_HPP
#include <cstdint>
#include "shizuku/kernel_abi.hpp"
namespace shizuku {
namespace templates {

// スレッド = カーネルが知る唯一の実行単位 (DESIGN §5)。
// ★実行中の識別状態は特権 call ledger にあり、メソッド表や export 情報は持たない。
//   「今どのオブジェクトとして、どの種別で走っているか」は ledger / base 状態から導き、
//   誰が何を export しているか・
//   誰が誰の親かといった話は全部カーネルオブジェクトの側にある。
//   ★2026-09-05: identity を文脈として持つこと自体は設計判断であって D1 違反ではない、
//     と方針を緩めた。緩めていないのは「役を ID で代用しない」という一点。
// in-place call (ABI: レジスタは全部 callee への入力、caller の frame をコピーしない) を持つ ARCH は
// CONTEXT::call_resume_t に「caller を再開するのに要る最小限」(sp・pc・特権・窓・有効フラグ) を
// 定義する。持たない ARCH (ARM) は場所を取らない空の型になる。
// 再開状態は call_count の浅い段 N 個ぶんだけ持つ (スレッド数 × 段数ぶん RAM が増えるため)。
// それより深い段、または in-place を持たない ARCH の段は従来の call_frame_push (frame を複製)。
template <typename CONTEXT, uint32_t N> struct call_resume_store {
  static constexpr bool enabled = false;
};
template <typename CONTEXT, uint32_t N>
  requires requires { typename CONTEXT::call_resume_t; }
struct call_resume_store<CONTEXT, N> {
  static constexpr bool enabled = true;
  typename CONTEXT::call_resume_t items[N]{};
};

template <typename CONTEXT> struct thread {
  static constexpr uint32_t CALL_IDENTITY_CAPACITY = 32;
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
    uint32_t depth = 0; // call / dispatch ledger の現在段数
    uint32_t call_count = 0; // LEDGER_CALL のアクティブ段数 (O(1) MAX_DEPTH 検査用)
    uint32_t current_call_object = 0;
    uint32_t caller_object = 0;
    uint32_t base_object_generation = 0;
    uint32_t current_call_generation = 0;
    uint32_t caller_generation = 0;
    // call_count 番目の call (LEDGER_CALL / LEDGER_DESCRIPTOR_CALL の段) ごとに 1 つ。
    struct call_identity_snapshot {
      uint32_t previous_caller_object;
      uint32_t previous_caller_generation;
    } identity[CALL_IDENTITY_CAPACITY]{};
    // in-place call の段 (call_count < INPLACE_DEPTH) の再開状態。resume.items[call_count] が
    // valid ならその段は in-place で、戻りでは caller の frame を作り直し a0/a1 以外を消す。
    // valid でなければ従来の段 (ヘッダに文脈を丸ごと退避)。
    static constexpr uint32_t INPLACE_DEPTH = 4;
    [[no_unique_address]] call_resume_store<CONTEXT, INPLACE_DEPTH> resume;
  };

  // ★state は CAS の対象なので 32bit 幅を固定する (ARCH::cas32 が uint32_t* で叩く)。
  uint32_t state = (uint32_t)state_t::UNINITIALIZED;
  CONTEXT *context = nullptr;
  call_stack_t call_stack;
  struct call_ledger_entry {
    uint32_t object;
    uint32_t handler_object;
    uintptr_t handler_entry;
    uint8_t kind;
    uint8_t via;
    uint16_t unwind_floor; // earliest depth this activation may return to
  };
  call_ledger_entry *ledger = nullptr;
  uint32_t ledger_capacity = 0;
  uint32_t base_object = 0;
  uint32_t base_kind = (uint32_t)object_kind::PLAIN;
  uint32_t base_handler_object = 0;
  uintptr_t base_handler_entry = 0;
  const call_ledger_entry &ledger_top() const {
    return ledger[call_stack.depth - 1];
  }
  uint32_t current_object() const {
    return call_stack.depth ? ledger_top().object : base_object;
  }
  uint32_t current_kind() const {
    return call_stack.depth ? ledger_top().kind : base_kind;
  }
  uint32_t current_handler_object() const {
    return call_stack.depth ? ledger_top().handler_object : base_handler_object;
  }
  uintptr_t current_handler_entry() const {
    return call_stack.depth ? ledger_top().handler_entry : base_handler_entry;
  }
  uint32_t affinity = 0b1; // bit0 = core0 (どのコアで走ってよいか)
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
