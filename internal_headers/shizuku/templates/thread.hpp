#ifndef SHIZUKU_TEMPLATE_THREAD_HPP
#define SHIZUKU_TEMPLATE_THREAD_HPP
#include <cstdint>
#include <type_traits>
#include "shizuku/kernel_abi.hpp"
namespace shizuku {

// SHIZUKU_LEAN_LEDGER 未定義時でも、user stack 上の header を権威とせず、
// 特権メモリで戻り先 (PC, SP, 特権) を保証するための特権退避域。窓は復帰時に現在行から得る。
struct caller_return_record {
  uintptr_t ret_pc = 0;
  uintptr_t ret_sp = 0;
  uint16_t prev_call = 0;
  uint8_t ret_privileged = 0;
  uint8_t reserved = 0;
  uint32_t generation = 0;
};

// in-place 復帰で台帳に付く arch 固有の型付き payload の既定 (空)。実体は無いので
// [[no_unique_address]] で台帳項目のサイズを増やさない (C6: 非 LEAN 16B / LEAN 32B)。
struct empty_ledger_payload {};

namespace templates {

// スレッド = カーネルが知る唯一の実行単位 (DESIGN §5)。
// ★実行中の識別状態は特権 call ledger にあり、メソッド表や export 情報は持たない。
//   「今どのオブジェクトとして、どの種別で走っているか」は ledger / base 状態から導き、
//   誰が何を export しているか・
//   誰が誰の親かといった話は全部カーネルオブジェクトの側にある。
//   ★2026-09-05: identity を文脈として持つこと自体は設計判断であって D1 違反ではない、
//     と方針を緩めた。緩めていないのは「役を ID で代用しない」という一点。
// 呼出しの記録を user stack 上のヘッダ枠に置く ARCH (design notes §5b) は、文脈型に
// CALL_HEADER_ON_STACK を定義する。そのとき call 段の識別の退避先 (identity スナップショット) は
// ヘッダの側にあり、スレッドごとの固定配列は要らない (配列は 1 要素に縮める)。ARM は従来どおり。
template <typename CONTEXT> struct call_identity_capacity {
  static constexpr uint32_t value = 32;
};
template <typename CONTEXT>
  requires requires { CONTEXT::CALL_HEADER_ON_STACK; }
struct call_identity_capacity<CONTEXT> {
  static constexpr uint32_t value = 1;
};

// call_ledger_entry の payload の型。context_t が ledger_payload_t を宣言した arch だけが
// 持つ (S2 で ARM が arch::inplace_payload_t を宣言する。S1 時点の ARM 本番は宣言せず空)。
template <typename CONTEXT, typename = void>
struct ledger_payload_trait {
  using type = empty_ledger_payload;
};

template <typename CONTEXT>
struct ledger_payload_trait<CONTEXT, std::void_t<typename CONTEXT::ledger_payload_t>> {
  using type = typename CONTEXT::ledger_payload_t;
};

template <typename CONTEXT>
using ledger_payload_of = typename ledger_payload_trait<CONTEXT>::type;

template <typename CONTEXT> struct thread {
  static constexpr uint32_t CALL_IDENTITY_CAPACITY = 32;
  // 配列の実際の大きさ (ヘッダ枠方式の ARCH は 1)。
  static constexpr uint32_t call_identity_storage = call_identity_capacity<CONTEXT>::value;
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
    // [SHIZUKU_LEAN_LEDGER] 最内の call 段 (LEDGER_CALL / LEDGER_DESCRIPTOR_CALL) の台帳の添字 + 1
    // (0 = call 段なし)。この版では current_call_* / caller_* / top は**更新しない** (台帳から導く:
    // identity_object() などの読み出しを使うこと)。既定の版は使わない。
    uint32_t call_top = 0;
    // call_count 番目の call (LEDGER_CALL / LEDGER_DESCRIPTOR_CALL の段) ごとに 1 つ (ARM)。
    // ヘッダ枠方式の ARCH ではヘッダが持つので使わない (1 要素に縮めてある)。
    struct call_identity_snapshot {
      uint32_t previous_caller_object;
      uint32_t previous_caller_generation;
    } identity[call_identity_storage]{};
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
#if defined(SHIZUKU_LEAN_LEDGER)
    // ---- [SHIZUKU_LEAN_LEDGER] 戻りと識別の権威を 1 つの記録 (この特権メモリの台帳) に集める ----
    uint32_t generation;      // call 段: 入ったときの呼び先の instance 世代。dispatch 段: 0
    uintptr_t ret_pc;         // この段から戻ったとき caller が再開する pc
    uintptr_t ret_sp;         // caller の例外時 SP (再開フレームはこの直下)
    uint16_t prev_call;       // この段を積む前の call_stack.call_top
    uint8_t ret_privileged;   // caller の特権
    uint8_t reserved;
#endif
    [[no_unique_address]] ledger_payload_of<CONTEXT> payload{};
  };

  caller_return_record *return_records = nullptr;
  // 呼び手の r4-r11 の退避域 (深さごとに 8 語)。戻り記録と別にしてあるのは、使わない ARCH
  // (RP2040 など) が .bss を払わないため。
  static constexpr uint32_t callee_saved_depth = 34;
  uint32_t (*callee_saved_records)[8] = nullptr;

  // 台帳への読み書きを 1 か所に閉じ込めるアクセサ
  struct ledger_accessor {
    // 記録先が無い/範囲外なら false (呼び元は呼出しを拒否する = fail closed)。
    static bool record_caller_return_authority(
        thread &t, uint32_t depth,
        uintptr_t ret_pc, uintptr_t ret_sp,
        uint8_t ret_privileged,
        uint16_t prev_call, uint32_t generation) {
#if defined(SHIZUKU_LEAN_LEDGER)
      if (t.ledger == nullptr || depth >= t.ledger_capacity) return false;
      auto &entry = t.ledger[depth];
      entry.ret_pc = ret_pc;
      entry.ret_sp = ret_sp;
      entry.ret_privileged = ret_privileged;
      entry.prev_call = prev_call;
      entry.generation = generation;
      return true;
#else
      if (t.return_records == nullptr || depth >= 34) return false;
      auto &rec = t.return_records[depth];
      rec.ret_pc = ret_pc;
      rec.ret_sp = ret_sp;
      rec.ret_privileged = ret_privileged;
      rec.prev_call = prev_call;
      rec.generation = generation;
      return true;
#endif
    }

    // 呼び手の callee-saved (r4-r11) を特権の退避域 (callee_saved_records) へ退避/復元する
    // (ARCH::SCRUB_CALLEE_SAVED_ON_CALL の ARCH だけが使う)。記録先が無いとき false = fail closed。
    static bool save_callee_saved(thread &t, uint32_t depth, const uint32_t (&regs)[8]) {
      if (t.callee_saved_records == nullptr || depth >= callee_saved_depth) return false;
      for (unsigned i = 0; i < 8; ++i) t.callee_saved_records[depth][i] = regs[i];
      return true;
    }
    static bool restore_callee_saved(const thread &t, uint32_t depth, uint32_t (&regs)[8]) {
      if (t.callee_saved_records == nullptr || depth >= callee_saved_depth) return false;
      for (unsigned i = 0; i < 8; ++i) regs[i] = t.callee_saved_records[depth][i];
      return true;
    }

    // 記録が無い (台帳/退避域が未設定、depth が範囲外) とき false。呼び元は false のとき
    // 出力を使わず、従来の復帰経路をそのまま使う。
    static bool restore_caller_return_authority(
        const thread &t, uint32_t depth,
        uintptr_t &ret_pc, uintptr_t &ret_sp,
        bool &ret_privileged) {
#if defined(SHIZUKU_LEAN_LEDGER)
      if (t.ledger == nullptr || depth >= t.ledger_capacity) return false;
      const auto &entry = t.ledger[depth];
      ret_pc = entry.ret_pc;
      ret_sp = entry.ret_sp;
      ret_privileged = (entry.ret_privileged != 0);
      return true;
#else
      if (t.return_records == nullptr || depth >= 34) return false;
      const auto &rec = t.return_records[depth];
      ret_pc = rec.ret_pc;
      ret_sp = rec.ret_sp;
      ret_privileged = (rec.ret_privileged != 0);
      return true;
#endif
    }
  };

  uint32_t stack_rbar = 0;
  uint32_t stack_rlar = 0;
  call_ledger_entry *ledger = nullptr;
  uint32_t ledger_capacity = 0;
  uint32_t base_object = 0;
  uint32_t base_kind = (uint32_t)object_kind::PLAIN;
  uint32_t base_handler_object = 0;
  uintptr_t base_handler_entry = 0;
  const call_ledger_entry &ledger_top() const {
    return ledger[call_stack.depth - 1];
  }
  // ---- 識別 (call 段の列から) ----------------------------------------------------------------
  //   既定の版は call_stack のフィールドを読む。SHIZUKU_LEAN_LEDGER の版はそれらを更新しないので、
  //   最内の call 段 (call_top) とその prev_call の鎖から導く。どちらの版でも同じ意味:
  //     identity_*        : call 段が無ければ base、あればその段の呼び先 (今「自分」として走っているもの)
  //     call_caller_*     : (前提 call_count != 0) その段を呼んだ側 = ひとつ外側の call 段の呼び先か base
  uint32_t identity_object() const {
#if defined(SHIZUKU_LEAN_LEDGER)
    return call_stack.call_count != 0 ? ledger[call_stack.call_top - 1].object : base_object;
#else
    return call_stack.call_count != 0 ? call_stack.current_call_object : base_object;
#endif
  }
  uint32_t identity_generation() const {
#if defined(SHIZUKU_LEAN_LEDGER)
    return call_stack.call_count != 0 ? ledger[call_stack.call_top - 1].generation
                                      : call_stack.base_object_generation;
#else
    return call_stack.call_count != 0 ? call_stack.current_call_generation
                                      : call_stack.base_object_generation;
#endif
  }
  uint32_t call_caller_object() const {
#if defined(SHIZUKU_LEAN_LEDGER)
    const uint32_t previous = ledger[call_stack.call_top - 1].prev_call;
    return previous != 0 ? ledger[previous - 1].object : base_object;
#else
    return call_stack.caller_object;
#endif
  }
  uint32_t call_caller_generation() const {
#if defined(SHIZUKU_LEAN_LEDGER)
    const uint32_t previous = ledger[call_stack.call_top - 1].prev_call;
    return previous != 0 ? ledger[previous - 1].generation : call_stack.base_object_generation;
#else
    return call_stack.caller_generation;
#endif
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
