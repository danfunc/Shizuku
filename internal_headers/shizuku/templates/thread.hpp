#ifndef SHIZUKU_TEMPLATE_THREAD_HPP
#define SHIZUKU_TEMPLATE_THREAD_HPP
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "shizuku/kernel_abi.hpp"
#include "shizuku/stack_window.hpp"
namespace shizuku {

// in-place でない ARCH (RP2040) で、user stack 上の header を権威とせず、
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
struct legacy_thread_layout_word { uintptr_t reserved = 0; };

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

// stack 窓機構 (open/extend/zero) を持つ文脈か。context_t が HAS_STACK_WINDOW = true を宣言した
// arch だけ (C6 U-mode)。既定は false で、ARM など他 arch に新メンバを要求しない。
template <typename CONTEXT> constexpr bool stack_window_enabled() {
  if constexpr (requires { CONTEXT::HAS_STACK_WINDOW; })
    return CONTEXT::HAS_STACK_WINDOW;
  else
    return false;
}

template <typename CONTEXT> constexpr bool compact_ledger_enabled() {
  if constexpr (requires { CONTEXT::COMPACT_LEDGER; }) return CONTEXT::COMPACT_LEDGER;
  return false;
}

// この文脈の ARCH が in-place call (呼出しの記録を caller の stack 枠に置く方式) を持つか。
// context_t が CALL_IN_PLACE を宣言した arch だけ true (rv32 / armv8m)。既定 (宣言なし) は false (RP2040)。
// ARCH::CALL_IN_PLACE (native_call.hpp の arch_call_in_place) と必ず一致させる: native_call.hpp の
// enter_in_place / pop_in_place の static_assert が見張る。
template <typename CONTEXT> constexpr bool call_in_place_of() {
  if constexpr (requires { CONTEXT::CALL_IN_PLACE; }) return CONTEXT::CALL_IN_PLACE;
  else return false;
}

template <typename CONTEXT> struct thread {
  static constexpr bool IN_PLACE = call_in_place_of<CONTEXT>();
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
    // STACK では読まれない (書くのは dispatch.cpp の enter_call だけ)。消すと sizeof(thread) が 480 → 476 になり、
    // thread の添字計算 (×480 の shift+add) が mul に変わって hot path の命令列と .text が動く (3c のゲートで実測)。
    // 消すかどうかは性能の判断と一緒に別途決める。それまで寸法を下の static_assert で固定する。
    uint32_t call_top = 0;
    // [STACK 方式 = in-place の ARCH] 最新の記録 (top) と、最新の call 段の記録のアドレス (0 = なし)。記録は
    // caller の stack 枠にあり、カーネルが持つのはこの 2 本のポインタと深さだけ。base_privileged は
    // 最外の活動の特権 (記録を積む前の文脈の特権。戻りで最外の caller の特権を引く)。
    // この版では current_call_* / caller_* は更新しない (identity_object() などの読み出しを使うこと)。
    uintptr_t call_top_rec = 0;
    uint32_t base_privileged = 0;
    // call_count 番目の call (LEDGER_CALL / LEDGER_DESCRIPTOR_CALL の段) ごとに 1 つ (ARM)。
    // ヘッダ枠方式の ARCH ではヘッダが持つので使わない (1 要素に縮めてある)。
    struct call_identity_snapshot {
      uint32_t previous_caller_object;
      uint32_t previous_caller_generation;
    } identity[call_identity_storage]{};
  };
  // 寸法の固定 (32-bit の実機): top 4 + uint32 8 本 (call_top を含む) 32 + call_top_rec 4 + base_privileged 4
  // + identity。call_stack_t の大きさが thread の大きさと添字計算を決める (上の call_top のコメント)。
  static_assert(sizeof(uintptr_t) != 4 || offsetof(call_stack_t, call_top_rec) == 36,
                "call_stack_t layout (call_top must stay: see the comment on call_top)");
  static_assert(sizeof(uintptr_t) != 4 ||
                    sizeof(call_stack_t) == 44 + 8 * call_identity_storage,
                "call_stack_t size must not change (thread stride 480 on RP2350)");

  // ★state は CAS の対象なので 32bit 幅を固定する (ARCH::cas32 が uint32_t* で叩く)。
  uint32_t state = (uint32_t)state_t::UNINITIALIZED;
  CONTEXT *context = nullptr;
  call_stack_t call_stack;
  struct standard_call_ledger_entry {
    uint32_t object;
    uint32_t handler_object;
    uintptr_t handler_entry;
    uint8_t kind;
    uint8_t via;
    uint16_t unwind_floor; // earliest depth this activation may return to
    [[no_unique_address]] ledger_payload_of<CONTEXT> payload{};
  };

  // ---- in-place の ARCH: 呼出しの記録 = caller の stack 枠に置く 32B (実機) ----------------------
  //   記録は callee の窓の外 (caller の SP 以上) にあり、callee・別 object は書けない。特権は置かない
  //   (戻りで caller の活動の特権を object の表から引く)。ret_sp は記録の位置から arch が求める。
  //   prev = 一つ外側の記録、prev_call = 一つ外側の call 段の記録 (識別の鎖)。depth = 積んだ後の深さ。
  struct stack_call_record {
    uintptr_t prev;
    uintptr_t prev_call;
    uintptr_t ret_pc;
    uintptr_t handler_entry;
    uint32_t generation;
    uint16_t object;
    uint16_t handler_object;
    uint16_t unwind_floor;
    uint16_t depth;
    uint8_t kind;
    uint8_t via;
    uint16_t reserved;
  };
  static_assert(sizeof(uintptr_t) != 4 || sizeof(stack_call_record) == 32,
                "stack call record must fill the 32-byte slot");
  // in-place でない ARCH (RP2040) は従来の 16B の台帳項目。
  using call_ledger_entry = std::conditional_t<IN_PLACE, stack_call_record, standard_call_ledger_entry>;

  caller_return_record *return_records = nullptr;
  // Preserve the C6/RP2040 record ABI while ARMv8-M drops the obsolete register
  // sidecar pointer. This word has no storage/restore semantics and no array.
  [[no_unique_address]] std::conditional_t<compact_ledger_enabled<CONTEXT>(),
      empty_ledger_payload, legacy_thread_layout_word> legacy_layout_word{};

  // 台帳への読み書きを 1 か所に閉じ込めるアクセサ
  struct ledger_accessor {
    // 記録先が無い/範囲外なら false (呼び元は呼出しを拒否する = fail closed)。
    static bool record_caller_return_authority(
        thread &t, uint32_t depth,
        uintptr_t ret_pc, uintptr_t ret_sp,
        uint8_t ret_privileged,
        uint16_t prev_call, uint32_t generation) {
      if (t.return_records == nullptr || depth >= 34) return false;
      auto &rec = t.return_records[depth];
      rec.ret_pc = ret_pc;
      rec.ret_sp = ret_sp;
      rec.ret_privileged = ret_privileged;
      rec.prev_call = prev_call;
      rec.generation = generation;
      return true;
    }


    // 記録が無い (台帳/退避域が未設定、depth が範囲外) とき false。呼び元は false のとき
    // 出力を使わず、従来の復帰経路をそのまま使う。
    static bool restore_caller_return_authority(
        const thread &t, uint32_t depth,
        uintptr_t &ret_pc, uintptr_t &ret_sp,
        bool &ret_privileged) {
      if (t.return_records == nullptr || depth >= 34) return false;
      const auto &rec = t.return_records[depth];
      ret_pc = rec.ret_pc;
      ret_sp = rec.ret_sp;
      ret_privileged = (rec.ret_privileged != 0);
      return true;
    }
  };

  uint32_t stack_rbar = 0;
  uint32_t stack_rlar = 0;
  // 窓機構は arch の能力 (CONTEXT::HAS_STACK_WINDOW) で有効になる。既定 (宣言なし) は false で、
  // そのとき stk は空 ([[no_unique_address]]) で thread の大きさを増やさない。
  // [STACK 方式] 窓は thread.stk の cur_win と u_depth だけで持つ (段ごとの窓の記帳は無い)。
  static constexpr bool HAS_STACK_WINDOW = stack_window_enabled<CONTEXT>();
  // thread 自身の stack 全域 [stack_lo, stack_hi) と、いまの窓 [cur_win_lo, cur_win_hi)。
  // HAS_STACK_WINDOW == false の thread では空で、メンバへのアクセスは if constexpr の中だけ。
  struct stack_extent {
    uintptr_t stack_lo = 0;
    uintptr_t stack_hi = 0;
    uintptr_t cur_win_lo = 0;
    uintptr_t cur_win_hi = 0;
    // [STACK 方式] いま開いている U callee の窓を持つ段の数 (0 = 外側に U の窓が無い)。
    uint32_t u_depth = 0;
  };
  [[no_unique_address]] std::conditional_t<HAS_STACK_WINDOW, stack_extent,
                                           empty_ledger_payload> stk{};

  // thread が実際に走る stack 領域を登録する。lo は 8B 切上げ、hi は 8B 切下げ。cur_win は全域に戻す。
  // HAS_STACK_WINDOW == false の arch では何もしない。
  void set_stack_bounds(uintptr_t lo, uintptr_t hi) noexcept {
    if constexpr (HAS_STACK_WINDOW) {
      lo = (lo + 7u) & ~uintptr_t(7);
      hi = hi & ~uintptr_t(7);
      if (hi < lo) hi = lo;
      stk.stack_lo = lo;
      stk.stack_hi = hi;
      stk.cur_win_lo = lo;
      stk.cur_win_hi = hi;
      stk.u_depth = 0;
    } else {
      (void)lo; (void)hi;
    }
  }
  void set_user_stack(uintptr_t lo, uintptr_t hi) noexcept { set_stack_bounds(lo, hi); }
  // 台帳を作り直すとき (thread の再利用) に窓の記帳を空にする。前の利用者の残りを持ち越さない。
  void reset_windows() noexcept {
    if constexpr (HAS_STACK_WINDOW) {
      stk.cur_win_lo = stk.stack_lo;
      stk.cur_win_hi = stk.stack_hi;
      stk.u_depth = 0;
    }
  }
  call_ledger_entry *ledger = nullptr;
  uint32_t ledger_capacity = 0;
  uint32_t base_object = 0;
  uint32_t base_kind = (uint32_t)object_kind::PLAIN;
  uint32_t base_handler_object = 0;
  uintptr_t base_handler_entry = 0;
  static const call_ledger_entry *record_at(uintptr_t address) {
    return reinterpret_cast<const call_ledger_entry *>(address);
  }
  const call_ledger_entry &ledger_top() const {
    if constexpr (IN_PLACE)
      return *record_at(call_stack.top);
    else
      return ledger[call_stack.depth - 1];
  }
  // 積んだ直後の先頭の記録に呼び先の同定を書くための可変の口。
  call_ledger_entry &ledger_top_mut() {
    if constexpr (IN_PLACE)
      return *reinterpret_cast<call_ledger_entry *>(call_stack.top);
    else
      return ledger[call_stack.depth - 1];
  }
  // ---- 識別 (call 段の列から) ----------------------------------------------------------------
  //   既定の版 (RP2040) は call_stack のフィールドを読む。in-place の版はそれらを更新しないので、
  //   最内の call 段の記録 (call_top_rec) とその prev_call の鎖から導く。どちらの版でも同じ意味:
  //     identity_*        : call 段が無ければ base、あればその段の呼び先 (今「自分」として走っているもの)
  //     call_caller_*     : (前提 call_count != 0) その段を呼んだ側 = ひとつ外側の call 段の呼び先か base
  uint32_t identity_object() const {
    if constexpr (IN_PLACE)
      return call_stack.call_count != 0 ? record_at(call_stack.call_top_rec)->object : base_object;
    else
      return call_stack.call_count != 0 ? call_stack.current_call_object : base_object;
  }
  uint32_t identity_generation() const {
    if constexpr (IN_PLACE)
      return call_stack.call_count != 0 ? record_at(call_stack.call_top_rec)->generation
                                        : call_stack.base_object_generation;
    else
      return call_stack.call_count != 0 ? call_stack.current_call_generation
                                        : call_stack.base_object_generation;
  }
  uint32_t call_caller_object() const {
    if constexpr (IN_PLACE) {
      const uintptr_t previous = record_at(call_stack.call_top_rec)->prev_call;
      return previous != 0 ? record_at(previous)->object : base_object;
    } else {
      return call_stack.caller_object;
    }
  }
  uint32_t call_caller_generation() const {
    if constexpr (IN_PLACE) {
      const uintptr_t previous = record_at(call_stack.call_top_rec)->prev_call;
      return previous != 0 ? record_at(previous)->generation : call_stack.base_object_generation;
    } else {
      return call_stack.caller_generation;
    }
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
