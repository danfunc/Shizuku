#ifndef SHIZUKU_NATIVE_CALL_HPP
#define SHIZUKU_NATIVE_CALL_HPP
// ===========================================================================
//  trap 内で直接処理する呼出し系 API (CALL_METHOD / CALL_METHOD_DESCRIPTOR / EXIT_METHOD /
//  FORWARD_CHILD_EXIT) の本体と、ヘッダ枠方式 (ARCH::CALL_IN_PLACE) の呼出しの記録の出し入れ。
// ===========================================================================
//  ここに置く理由: 1 API = 1 関数にするため。RV32 の例外入口 (番号の表から直接入る関数) と
//  カーネル (svc_dispatch 経由・host 試験) の両方が同じ本体を使い、前者はこれを自分の中へ
//  展開する (always_inline)。検査の中身と順序は kernel_object 経由だった頃と同じ。
//  レジスタは素通し (arch/riscv/rv32_frame.h の目印): 戻りで消すのは callee が自分の意思で戻らない
//  巻き戻し (2 段以上の EXIT_METHOD、FORWARD_CHILD_EXIT) だけで、普通の 1 段の戻りは消さない。
//  ヘッダ枠方式でない ARCH (ARM) ではこの本体は使わない (if constexpr で落ちる)。
#include <cstdint>
#include <type_traits>
#include "shizuku/core_num.hpp"
#include "shizuku/kernel_abi.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/stack_window.hpp"
#include "shizuku/svc_attributes.hpp"
#include "shizuku/templates/thread.hpp"

// 呼出しの記録の方式は、ARCH の context_t::CALL_IN_PLACE (thread.hpp の call_in_place_of) で決まる。
//   真 (RP2350 / C6 / C5): 記録を caller の stack 枠に置く (STACK 方式。header_push_of / pop_in_place / enter_in_place)。
//   偽 (RP2040): 従来の call_frame_push と特権の配列 (thread.ledger) の経路を通る。
// context_t と ARCH::CALL_IN_PLACE の一致は、pop_in_place / enter_in_place の static_assert が見張る。

namespace shizuku {

// ARCH が in-place call (caller の frame をコピーせず、レジスタは全部 callee への入力、戻りで
// 消える ABI) を持つか。持たない ARCH (ARM) は従来の call_frame_push だけを使う。
template <class A> constexpr bool arch_call_in_place() {
  if constexpr (requires { A::CALL_IN_PLACE; })
    return A::CALL_IN_PLACE;
  else
    return false;
}

template <typename C>
static constexpr bool is_context_unprivileged(const C *ctx) {
  if constexpr (requires { ctx->control; }) {
    return (ctx->control & 1u) != 0;
  } else if constexpr (requires { ctx->privileged; }) {
    return !ctx->privileged;
  } else {
    return false;
  }
}

// 呼出しの記録 (ヘッダ枠方式、ARCH::CALL_IN_PLACE) の出し入れ。dispatch.cpp の関数は明示特殊化
// (テンプレートではない) で、使わない側の枝も構文として通る必要があるため、ARCH 固有の呼び出しは
// ここ (依存名のテンプレート) に包む。
// caller の枠 (converter が空けた [return_sp + CALL_RECORD_OFFSET, +32)) に記録を積む。
// 前提: A::call_record_room_at(context, return_sp) が真 (枠が caller の天井の内側にある)。
// 呼び先の同定 (object・handler・kind・generation) は呼び出し側が戻り値の記録へ書く。特権は書かない。
template <class A, class Thread, class Context, class Frame>
[[gnu::always_inline]] inline auto *stack_record_push(Thread &thread, Context &context, Frame &frame,
                                                       uintptr_t return_sp, uint8_t via, uint32_t floor) {
  using Record = typename Thread::call_ledger_entry;
  auto &stack = thread.call_stack;
  auto *record = reinterpret_cast<Record *>(return_sp + A::CALL_RECORD_OFFSET);
  if (stack.depth == 0)
    stack.base_privileged = A::context_privileged(context) ? 1u : 0u;
  record->prev = stack.top;
  record->prev_call = stack.call_top_rec;
  record->ret_pc = A::frame_pc(frame);
  record->via = via;
  record->unwind_floor = (uint16_t)floor;
  record->depth = (uint16_t)(stack.depth + 1u);
  record->reserved = 0;
  record->generation = 0; // dispatch 段は世代を持たない。call 段は呼び出し側が上書きする。
  stack.top = reinterpret_cast<uintptr_t>(record);
  stack.depth = stack.depth + 1u;
  return record;
}
// 先頭の記録がカーネルの持つ先頭ポインタ・深さと食い違っていないか (深さの一致と、ひとつ下の記録との鎖)。
// 戻りの前に必ず調べる。試験 (call_ladder) も、記録を書き換えたときに検出できることをこれで確かめる。
template <class Thread>
[[gnu::always_inline]] inline bool stack_record_chain_ok(const Thread &thread) {
  using Record = typename Thread::call_ledger_entry;
  const uint32_t depth = thread.call_stack.depth;
  const auto *record = reinterpret_cast<const Record *>(thread.call_stack.top);
  if (depth == 0)
    return record == nullptr;
  if (record == nullptr || record->depth != depth)
    return false;
  const auto *prev = reinterpret_cast<const Record *>(record->prev);
  return (prev == nullptr) == (depth == 1u) && (prev == nullptr || prev->depth == depth - 1u);
}
// caller の枠にヘッダを書いて top を進める。caller の状態は frame (trap フレーム) と context から取る。
// via / floor は台帳 (特権側) が持つので、非特権スタック上のヘッダには書かない (呼び出し側が台帳に書く)。
template <class A, class Thread, class Context, class Frame>
[[gnu::always_inline]] inline bool header_push_of(Thread &thread, Context &context, Frame &frame, uint8_t via,
                                                  uint32_t floor) {
  if constexpr (arch_call_in_place<A>()) {
    const uintptr_t return_sp = A::call_return_sp(context);
    if (!A::call_record_room_at(context, return_sp))
      return false;
    stack_record_push<A>(thread, context, frame, return_sp, via, floor);
    return true;
  } else {
    return false;
  }
}
namespace native {

// enter_call (dispatch.cpp) の in-place の枝: header_push_of が書いた記録 (ret_pc・prev・prev_call) に、呼び先の同定と
// 世代だけを足し、call 段を 1 つ進める。集成体の代入で消さない。identity (current / caller) は call 段の列から導く
// (call_top_rec を更新する)。call_top は STACK では読まれないが、書くのをやめると .text が動くので残す。
// dispatch.cpp の特殊化は template ではなく、捨てた枝も型検査される。in-place でない台帳項目には generation が
// 無いので、書出しは依存名のこの template に包み、in-place の枝からだけ呼ぶ。
template <class Thread, class Entry, class Request>
[[gnu::always_inline]] inline void write_call_record(Thread &thread, Entry &entry, const Request &request,
                                                     uint8_t via, uint32_t unwind_floor) {
  entry.object = request.callee_object;
  entry.handler_object = request.parent_handler_object;
  entry.handler_entry = request.parent_handler_entry;
  entry.kind = (uint8_t)request.callee_kind;
  entry.via = via;
  entry.unwind_floor = (uint16_t)unwind_floor;
  entry.generation = request.callee_generation;
  thread.call_stack.call_count++;
  thread.call_stack.call_top = thread.call_stack.depth;
  thread.call_stack.call_top_rec = thread.call_stack.top;
}

template <class A>
[[gnu::always_inline]] inline bool defer_thread_switch(core_arg_t<A> core) {
  if constexpr (arch_has_core_num<A>) {
    return A::defer_svc_switch(core);
  } else {
    if constexpr (requires { A::defer_svc_switch(); })
      return A::defer_svc_switch();
    return false;
  }
}

template <class A>
requires (!arch_has_core_num<A>)
[[gnu::always_inline]] inline bool defer_thread_switch() {
  return defer_thread_switch<A>(no_core{});
}

template <class Objects, class Method>
[[gnu::always_inline]] inline auto resolve_method_view(
    Objects &objects, uintptr_t id, uintptr_t method, uint32_t caller,
    object_error &error, Method &entry) {
  if constexpr (requires { objects.resolve_method_view(id, method, caller, error, entry); }) {
    return objects.resolve_method_view(id, method, caller, error, entry);
  } else {
    const auto *row = objects.find_method_row(id, method, caller, error, entry);
    using View = decltype(objects.view_of_row((uint32_t)id, *row));
    struct resolution { bool found; View view; };
    return resolution{row != nullptr, row != nullptr ? objects.view_of_row((uint32_t)id, *row) : View{}};
  }
}

// 本体の結果。DONE = 処理した (応答を書いた)。FORWARD = kobj へ回す (呼び出し側が svc_forward)。
// CORRUPT = 台帳の不変条件が破れている (呼び出し側が panic。本体は関数呼出しを持たないので、
// 呼び出し側が tail call で停止できる)。
enum class status : uint8_t { DONE, FORWARD, CORRUPT };

struct return_region_window {
  uintptr_t base;
  uintptr_t limit;
};

// Resolve the resumed caller's window independently from the object identity used
// to find its row. A caller activation entered through dispatch keeps the kernel's
// closed window; every other caller uses its object's current row.
template <class OBJECTS>
[[gnu::always_inline]] inline return_region_window resolve_return_region_window(
    const OBJECTS &objects, uint8_t caller_via, uint8_t dispatch_via,
    uint32_t caller_object) {
  if (caller_via == dispatch_via)
    return {0, 0};
  const auto window = objects.return_region(caller_object);
  return {window.base, window.limit};
}

// 台帳の最上段を 1 つ落とす (call の段なら識別を戻す)。ヘッダ枠方式 ARCH 専用。
// 前提: depth >= 1 (呼び出し側が戻り先 d と count を検査済み)。call の段なのに call_count == 0 なら
// 何も変えず false (不変条件の破れ)。scrub は再開フレームの目印 (出口でレジスタを消すか):
// callee が自分の意思で戻らない巻き戻しのときだけ true。
template <class K, class KObj>
[[gnu::always_inline]] inline bool pop_in_place(KObj &kobj, typename K::THREAD &thread,
                                                typename K::CONTEXT &context,
                                                typename K::FRAME *&frame, bool restore,
                                                bool scrub,
                                                core_arg_t<typename K::ARCH> core) {
  using A = typename K::ARCH;
  static_assert(templates::call_in_place_of<typename K::CONTEXT>() == arch_call_in_place<A>(),
                "context_t::CALL_IN_PLACE must agree with ARCH::CALL_IN_PLACE");
  if constexpr (arch_call_in_place<A>()) {
    const uint32_t depth = thread.call_stack.depth;
    // 先頭の記録は caller の stack 枠にある (callee の窓の外)。カーネルが持つ先頭ポインタ・深さと
    // 照合してから使う: 深さの食い違い・鎖の切れは何も変えずに false (呼び出し側が panic する)。
    using Record = typename K::THREAD::call_ledger_entry;
    if (depth == 0 || !stack_record_chain_ok(thread))
      return false;
    const auto *record = reinterpret_cast<const Record *>(thread.call_stack.top);
    const uintptr_t prev_address = record->prev;
    const auto *prev = reinterpret_cast<const Record *>(prev_address);
    const uint8_t via = record->via;
    const uint8_t caller_via = prev != nullptr ? prev->via : 0;
    const bool caller_is_dispatch = caller_via == K::LEDGER_DISPATCH;
    const bool is_call = via == K::LEDGER_CALL || via == K::LEDGER_DESCRIPTOR_CALL;
    if (is_call && thread.call_stack.call_count == 0)
      return false;
    const uintptr_t prev_call_address = record->prev_call;
    const uint32_t caller_object = prev_call_address != 0
        ? reinterpret_cast<const Record *>(prev_call_address)->object
        : thread.base_object;
    // 特権は記録に置かない: 再開する caller の活動 (ひとつ下の記録の呼び先) から引く。dispatch で入った
    // 活動は常に特権、call で入った活動は object の表の flags、最外はスレッドの最外の特権。
    const bool caller_privileged = prev == nullptr
        ? thread.call_stack.base_privileged != 0
        : (caller_is_dispatch || kobj.privileged_of(prev->object));
    const uintptr_t ret_pc = record->ret_pc;
    const uintptr_t ret_sp = A::record_return_sp(reinterpret_cast<uintptr_t>(record));
    if (is_call) {
      thread.call_stack.call_count--;
      thread.call_stack.call_top_rec = prev_call_address;
    }
    // 戻る段が U callee で窓を持っていたなら、その窓をゼロ化して外側の窓へ戻す。
    // ゼロ化は [cur_win_lo, cur_win_hi) だけ。記録 (ret_sp の上 32B) は窓の上端 (ret_sp & ~15) より上なので壊れない。
    // 外側に U の窓が無ければ全域、あれば外側の U callee の窓 = [戻り先 SP - K, その callee の入場の ret_sp)。
    if constexpr (K::THREAD::HAS_STACK_WINDOW) {
      if (is_call && !kobj.privileged_of(record->object)) {
        zero_window_of<A>(shizuku::stack_window{thread.stk.cur_win_lo, thread.stk.cur_win_hi});
        const uint32_t remaining = thread.stk.u_depth - 1u;
        thread.stk.u_depth = remaining;
        if (remaining == 0 || prev_address == 0) {
          thread.stk.u_depth = 0;
          thread.stk.cur_win_lo = thread.stk.stack_lo;
          thread.stk.cur_win_hi = thread.stk.stack_hi;
        } else {
          const uintptr_t hi = A::record_return_sp(prev_address) & ~uintptr_t(15);
          const uintptr_t sp = ret_sp & ~uintptr_t(15);
          const uintptr_t lo = sp > thread.stk.stack_lo + kStackWindowStep
                                   ? sp - kStackWindowStep
                                   : thread.stk.stack_lo;
          thread.stk.cur_win_lo = lo;
          thread.stk.cur_win_hi = hi;
        }
      }
    }
    if (restore) {
      if constexpr (arch_has_core_num<A>) {
        frame = A::call_resume_at(context, ret_pc, ret_sp, caller_privileged, 0, 0, scrub, core);
      } else {
        frame = A::call_resume_at(context, ret_pc, ret_sp, caller_privileged, 0, 0, scrub);
      }
      const auto window = resolve_return_region_window(
          kobj, caller_via, K::LEDGER_DISPATCH, caller_object);
      A::set_region_window(context, window.base, window.limit);
      if (!caller_is_dispatch && !A::context_privileged(context)) {
        if constexpr (requires { A::set_unprivileged_resume_metadata(*frame, ret_sp, A::trusted_stack_top(), scrub); }) {
          A::set_unprivileged_resume_metadata(*frame, ret_sp, A::trusted_stack_top(), scrub);
        }
        if constexpr (arch_has_core_num<A>) {
          A::switch_caller_protection(caller_object, core);
        } else {
          if constexpr (requires { A::switch_caller_protection(caller_object); }) {
            A::switch_caller_protection(caller_object);
          }
        }
      }
    }
    thread.call_stack.top = prev_address;
    thread.call_stack.depth = depth - 1;
  }
  return true;
}

template <class K, class KObj>
requires (!arch_has_core_num<typename K::ARCH>)
[[gnu::always_inline]] inline bool pop_in_place(KObj &kobj, typename K::THREAD &thread,
                                                typename K::CONTEXT &context,
                                                typename K::FRAME *&frame, bool restore,
                                                bool scrub) {
  return pop_in_place<K, KObj>(kobj, thread, context, frame, restore, scrub, no_core{});
}

// 台帳の上から count 段を落とす。最後に落とした段の caller だけが再開する。
// 再開の目印は、2 段以上を一度に落とす (callee が自分の意思で戻らない巻き戻し) か forced_scrub
// (FORWARD_CHILD_EXIT) のとき消去 (RV32_TAG_ECALL_SCRUB)、1 段の普通の戻りのときは素通し。
template <class K, class KObj>
[[gnu::always_inline]] inline bool unwind_in_place(KObj &kobj, typename K::THREAD &thread,
                                                   typename K::CONTEXT &context,
                                                   typename K::FRAME *&frame, uintptr_t count,
                                                   bool forced_scrub,
                                                   core_arg_t<typename K::ARCH> core) {
  const bool scrub = forced_scrub || count > 1;
  for (uintptr_t index = 0; index < count; ++index)
    if (!pop_in_place<K, KObj>(kobj, thread, context, frame, index + 1 == count, scrub, core))
      return false;
  return true;
}

template <class K, class KObj>
requires (!arch_has_core_num<typename K::ARCH>)
[[gnu::always_inline]] inline bool unwind_in_place(KObj &kobj, typename K::THREAD &thread,
                                                   typename K::CONTEXT &context,
                                                   typename K::FRAME *&frame, uintptr_t count,
                                                   bool forced_scrub) {
  return unwind_in_place<K, KObj>(kobj, thread, context, frame, count, forced_scrub, no_core{});
}

// 呼び先の入口フレームに「入口 entry_pc へ入り、普通に return すると戻り口へ落ちる」形を書く。
// ARCH が呼び先オブジェクト専用の U 側 stub を持てる (ARM: SRAM だけで U を走らせるため) ときは
// それを使い、持たない ARCH は従来どおりカーネルの戻り口 1 本 (return_stub) を使う。
template <class A, class Frame>
[[gnu::always_inline]] inline void set_callee_entry(Frame &frame, uintptr_t entry_pc,
                                                    uint32_t callee_id) {
  if constexpr (requires { A::set_entry_for_object(frame, entry_pc, callee_id); }) {
    A::set_entry_for_object(frame, entry_pc, callee_id);
  } else {
    (void)callee_id;
    A::set_entry(frame, entry_pc, A::return_stub());
  }
}

// 呼び先 id (入口 entry_pc) へ入る (enter_call + do_call の後半)。ヘッダ枠方式 ARCH 専用。
//   台帳段を via で積み、戻りの床 (unwind floor) を積む前の深さに置く。呼び先の入口フレームは
//   caller の trap フレームそのもので、入口・戻り口・特権・窓を書き換える (引数は呼び出し側)。
//   値は call_request に組まず、呼び出し側が作った view (行の値か項目の前計算) から書く場所で直接読む。
//   この関数は行を引かない。前提: view.id は範囲内・作成済み。
//   入れなかったとき (BAD_REQUEST / NO_STACK) は何も書かない。
// 入場の本体。呼び先の形 (view: 種別・親ハンドラ・窓・特権・instance 世代) は CALL_METHOD が行から、
// 方法記述子の直接 call が項目の前計算から渡す。どちらも同じ書き出し (台帳段・入口フレーム) になる。
template <class K, class KObj>
[[gnu::always_inline]] inline kernel_error enter_in_place(
    K &kernel, typename K::THREAD &thread, typename K::CONTEXT &context,
    const typename KObj::callee_view &view, uintptr_t entry_pc, uint8_t via,
    core_arg_t<typename K::ARCH> core) {
  using A = typename K::ARCH;
  static_assert(templates::call_in_place_of<typename K::CONTEXT>() == arch_call_in_place<A>(),
                "context_t::CALL_IN_PLACE must agree with ARCH::CALL_IN_PLACE");
  if constexpr (arch_call_in_place<A>()) {
    auto &stack = thread.call_stack;
    // ★偽装防止: 一般の呼び先が KERNEL_OBJECT 種別や root ハンドラの ID を名乗ることは拒否する。
    //   CALL_METHOD は「kernel object は native 経路で呼ばせない」振り分けとして、ここで行から作った
    //   view を調べる。方法記述子の呼出しは、記述子の作成 (create_method_descriptor) が同じ拒否を
    //   一度だけ済ませてあり、項目はカーネルが書き写した値なので、呼出しのたびには調べない
    //   (via は呼び出し側で定数なので、展開後は枝ごと消える)。
    if (via != K::LEDGER_DESCRIPTOR_CALL &&
        (view.kind == object_kind::KERNEL_OBJECT || view.id == kernel.svc_handler_object()))
      return kernel_error::BAD_REQUEST;
    // 深さの上限は stack の大きさで決まる (MAX_DEPTH の検査は無い)。
    const uint32_t depth = stack.depth; // 呼び先の unwind floor
    // caller の戻り SP は 1 回だけ求め、stack の余地の検査と記録の書出しで使い回す。
    const uintptr_t return_sp = A::call_return_sp(context);
    // 記録は caller の stack 枠に置く。深さの上限は stack の大きさで決まり (記録 1 つにつき 64B 以上を使う)、
    // 台帳の枠・MAX_DEPTH・16 bit の検査は要らない。残るのは「枠が caller の天井の内側にあり、PSPLIM との間に
    // 余白がある」の 1 本だけ。depth / unwind_floor は 16 bit: RP2350 の SRAM 520KiB / 64B < 65535 で溢れない。
    if (!A::call_record_room_at(context, return_sp))
      return kernel_error::NO_STACK;
    // U callee の窓を開く (窓を持つのは U callee の段だけ)。全検査の後で状態を変える。
    // 特権 caller が外側の U 窓の中にいる (depth != 0 かつ u_depth != 0) なら、戻りで外側の窓を復元できないので断る。
    if constexpr (K::THREAD::HAS_STACK_WINDOW) {
      if (!view.privileged) {
        if (A::context_privileged(context) && stack.depth != 0 && thread.stk.u_depth != 0)
          return kernel_error::NO_STACK;
        if ((thread.stk.stack_lo != 0 || thread.stk.stack_hi != 0) &&
            (return_sp < thread.stk.stack_lo || return_sp > thread.stk.stack_hi))
          return kernel_error::NO_STACK;
        const auto win = open_window(return_sp, thread.stk.stack_lo);
        if (!win.is_valid())
          return kernel_error::NO_STACK;
        thread.stk.cur_win_lo = win.lo;
        thread.stk.cur_win_hi = win.hi;
        thread.stk.u_depth = thread.stk.u_depth + 1;
      }
    }
    typename K::FRAME &frame = *context.sp;
    {
      auto *record = stack_record_push<A>(thread, context, frame, return_sp, via, depth);
      record->object = (uint16_t)view.id;
      record->handler_object = (uint16_t)view.parent_handler_object;
      record->handler_entry = view.parent_handler_entry;
      record->kind = (uint8_t)view.kind;
      record->generation = view.instance_generation;
      stack.call_count = stack.call_count + 1;
      stack.call_top_rec = reinterpret_cast<uintptr_t>(record);
      set_callee_entry<A>(frame, entry_pc, view.id);
    }
    A::set_priv(context, view.privileged);
    A::set_region_window(context, view.region_base, view.region_limit);
    if constexpr (arch_has_core_num<A>) {
      A::switch_callee_protection(context, frame, view.id, view.privileged, core);
    } else {
      A::switch_callee_protection(context, frame, view.id, view.privileged);
    }
    return kernel_error::OK;
  } else {
    return kernel_error::BAD_REQUEST;
  }
}

template <class K, class KObj>
requires (!arch_has_core_num<typename K::ARCH>)
[[gnu::always_inline]] inline kernel_error enter_in_place(
    K &kernel, typename K::THREAD &thread, typename K::CONTEXT &context,
    const typename KObj::callee_view &view, uintptr_t entry_pc, uint8_t via) {
  return enter_in_place<K, KObj>(kernel, thread, context, view, entry_pc, via, no_core{});
}

// CALL_METHOD (4): a1 = オブジェクト ID, a2 = メソッド番号, a3 = 引数。応答は (error, 0)。
//   エラーの種類・順序: BAD_OBJECT → NOT_PRIVILEGED → BAD_METHOD → UNDECLARED_METHOD → NO_STACK。
template <class K, class KObj>
[[gnu::always_inline]] inline void call_method_in_place(K &kernel,
                                                        typename K::THREAD &thread,
                                                        typename K::CONTEXT &context,
                                                        KObj &kobj,
                                                        core_arg_t<typename K::ARCH> core) {
  using A = typename K::ARCH;
  if constexpr (arch_call_in_place<A>()) {
    if constexpr (K::CORE_COUNT == 1) {
      // Keep the single-core C6 instruction path and its validation order.
      typename K::FRAME &frame = *context.sp;
      const uintptr_t id = A::arg(frame, 1);
      const uint32_t caller_id = thread.identity_object();
      object_error error;
      typename KObj::method_t entry;
      const auto *row = kobj.find_method_row(id, A::arg(frame, 2), caller_id, error, entry);
      if (row == nullptr) {
        A::set_result(frame, (uintptr_t)error, 0);
        return;
      }
      const uintptr_t argument = A::arg(frame, 3);
      const kernel_error entered = enter_in_place<K, KObj>(
          kernel, thread, context, kobj.view_of_row((uint32_t)id, *row), (uintptr_t)entry,
          K::LEDGER_CALL, core);
      if (entered != kernel_error::OK) {
        A::set_result(frame, (uintptr_t)(entered == kernel_error::NO_STACK
                                             ? object_error::NO_STACK
                                             : object_error::BAD_OBJECT), 0);
        return;
      }
      A::set_method_argument(frame, argument);
    } else {
      typename K::FRAME &frame = *context.sp;
      const uintptr_t id = A::arg(frame, 1);
      const uint32_t caller_id = thread.identity_object();
      object_error error;
      typename KObj::method_t entry;
      const auto resolved = resolve_method_view(kobj, id, A::arg(frame, 2), caller_id, error, entry);
      if (!resolved.found) {
        A::set_result(frame, (uintptr_t)error, 0);
        return;
      }
      const uintptr_t argument = A::arg(frame, 3);
      const kernel_error entered = enter_in_place<K, KObj>(
          kernel, thread, context, resolved.view, (uintptr_t)entry,
          K::LEDGER_CALL, core);
      if (entered != kernel_error::OK) {
        A::set_result(frame, (uintptr_t)(entered == kernel_error::NO_STACK
                                             ? object_error::NO_STACK
                                             : object_error::BAD_OBJECT),
                      0);
        return;
      }
      // 呼び先の引数は a0 (ARM では r0) = argument だけ。a1 以降は書かない (caller の入力のまま素通し。
      // 呼び先は宣言した引数以外を信用しない)。
      A::set_method_argument(frame, argument);
    }
  }
}

template <class K, class KObj>
requires (!arch_has_core_num<typename K::ARCH>)
[[gnu::always_inline]] inline void call_method_in_place(K &kernel,
                                                        typename K::THREAD &thread,
                                                        typename K::CONTEXT &context,
                                                        KObj &kobj) {
  call_method_in_place<K, KObj>(kernel, thread, context, kobj, no_core{});
}

// CALL_METHOD_DESCRIPTOR (33): a1 = 記述子の index, a2..a7 = 呼び先の引数 6 個 (a0..a5 へ渡る)。
//   trap 内で表の錠 (table_lock) は取らず、版番号 seq による読み取り検証を行う。
//   項目は呼び先の前計算 (入口・種別・親・窓・特権・instance 世代) を持つので、行を引かず、表は [持ち主][index]
//   の 32 バイト項目をシフトで引く。
//   trap 内で再試行ループを回さない、錠を取らない、LR/SC を使わない。
template <class K, class KObj>
[[gnu::always_inline]] inline void call_descriptor_in_place(K &kernel,
                                                            typename K::THREAD &thread,
                                                            typename K::CONTEXT &context,
                                                            KObj &kobj,
                                                            core_arg_t<typename K::ARCH> core) {
  using A = typename K::ARCH;
  if constexpr (arch_call_in_place<A>()) {
    typename K::FRAME &frame = *context.sp;
    const uint32_t caller_id = thread.identity_object();
    const uint32_t caller_generation = thread.identity_generation();
    const uint32_t index = (uint32_t)A::arg(frame, 1);

    if (index >= KObj::METHOD_DESCRIPTOR_CAPACITY || caller_id >= KObj::OBJECT_COUNT) {
      A::set_result(frame, (uintptr_t)object_error::BAD_DESCRIPTOR, 0);
      return;
    }

    const auto &desc = kobj.descriptor_entry(caller_id, index);

    // 1. seq を load-acquire (単一コアなら acquire fence 不要、RELAXED + コンパイラバリア)
    uint8_t seq1;
    if constexpr (K::CORE_COUNT == 1) {
      seq1 = __atomic_load_n(&desc.seq, __ATOMIC_RELAXED);
      asm volatile("" ::: "memory");
    } else {
      seq1 = __atomic_load_n(&desc.seq, __ATOMIC_ACQUIRE);
    }

    // 奇数なら書込み中なので即 STALE
    if ((seq1 & 1u) != 0) {
      A::set_result(frame, (uintptr_t)object_error::STALE_DESCRIPTOR, 0);
      return;
    }

    // 2. seq 区間の中で、判定と入場に要る欄だけを読む (項目全体を写さない。seq 自体は写さない)。
    //    1 回目と 2 回目の seq が同じなら、ここで読んだ値は書込み中のものと混ざっていない。
    const auto view = kobj.view_of_descriptor(desc);
    const uintptr_t entry_pc = desc.target_entry;
    const uint32_t owner_generation = desc.owner_generation;
    const uint32_t target_generation = desc.target_generation;

    // 3. seq を再読し、変わっていれば即 STALE
    uint8_t seq2;
    if constexpr (K::CORE_COUNT == 1) {
      asm volatile("" ::: "memory");
      seq2 = __atomic_load_n(&desc.seq, __ATOMIC_RELAXED);
    } else {
      seq2 = __atomic_load_n(&desc.seq, __ATOMIC_ACQUIRE);
    }
    if (seq1 != seq2) {
      A::set_result(frame, (uintptr_t)object_error::STALE_DESCRIPTOR, 0);
      return;
    }

    // 4. 読んだ値に対する照合 (順序: BAD_DESCRIPTOR -> STALE_DESCRIPTOR)
    const uint32_t callee = view.id;
    if (callee == KObj::DESCRIPTOR_NONE || callee >= KObj::OBJECT_COUNT) {
      A::set_result(frame, (uintptr_t)object_error::BAD_DESCRIPTOR, 0);
      return;
    }
    if (owner_generation != caller_generation) {
      A::set_result(frame, (uintptr_t)object_error::STALE_DESCRIPTOR, 0);
      return;
    }
    const uint32_t version = kobj.object_version(callee);
    if (version == 0 || target_generation != version) {
      A::set_result(frame, (uintptr_t)object_error::STALE_DESCRIPTOR, 0);
      return;
    }

    // 5. 読んだ値から入場の形を作る (呼び先の行を引かない)。
    const kernel_error entered = enter_in_place<K, KObj>(
        kernel, thread, context, view, entry_pc, K::LEDGER_DESCRIPTOR_CALL, core);

    if (entered != kernel_error::OK) {
      A::set_result(frame, (uintptr_t)(entered == kernel_error::NO_STACK
                                           ? object_error::NO_STACK
                                           : object_error::BAD_DESCRIPTOR),
                    0);
      return;
    }
    // enter_in_place は pc/ra/tag と context/台帳だけを書き、引数欄を触らない。
    // 検査がすべて通ったあとで前からずらす。6 引数 (ARM では 1 引数) を入口処理をまたいでレジスタに
    // 保持する必要がなく、失敗時にも caller の入力はそのまま残る (応答 a0/a1 または r0/r1 以外)。
    A::move_descriptor_arguments(frame);
  }
}

template <class K, class KObj>
requires (!arch_has_core_num<typename K::ARCH>)
[[gnu::always_inline]] inline void call_descriptor_in_place(K &kernel,
                                                            typename K::THREAD &thread,
                                                            typename K::CONTEXT &context,
                                                            KObj &kobj) {
  call_descriptor_in_place<K, KObj>(kernel, thread, context, kobj, no_core{});
}

// EXIT_METHOD (2, Forward == false) / FORWARD_CHILD_EXIT (27, Forward == true): 戻り先の絶対深さ
// d を検証して current_depth - d 段を落とす。a1 = d, a2 = value, a3 = error。FORWARD = kobj へ回す
// (スレッドの最外のメソッドが戻る場合 = スレッドの終わり)。
// 非 KOBJ の return stub (a4 = RETURN_STUB_MARKER、2 のみ) は d を持たないので、台帳の unwind floor
// (= 呼び先を積む前の深さ) を d とする。27 は専用 HANDLER からだけ (parent_ledger 権限)。
// 検査は primitive RETURN と同じ (validate_return_depth)。弾いたら 1 枚も落とさず
// UNWIND_REJECTED + kernel_error を返す。
template <class K, class KObj, bool Forward>
[[gnu::always_inline]] inline status exit_in_place(K &kernel, KObj &kobj, typename K::THREAD &thread,
                                                   typename K::CONTEXT &context,
                                                   core_arg_t<typename K::ARCH> core) {
  using A = typename K::ARCH;
  if constexpr (arch_call_in_place<A>()) {
    typename K::FRAME *frame = context.sp;
    const uint32_t depth = thread.call_stack.depth;
    if constexpr (Forward) {
      // 出どころ確認。発行元の段 (台帳の最上段、dispatch 段は積まれていない) が専用 HANDLER で、
      // root kobj 本人ではないこと。
      const bool from_handler =
          depth != 0 &&
          thread.ledger_top().kind == (uint8_t)object_kind::HANDLER &&
          thread.ledger_top().object != kernel.svc_handler_object();
      if (!svc_permission_allows((uintptr_t)object_api::FORWARD_CHILD_EXIT,
                                 !is_context_unprivileged(&context), false, from_handler)) {
        A::set_result(*frame, (uintptr_t)object_error::NOT_PRIVILEGED, 0);
        return status::DONE;
      }
    }
    const uint32_t floor = depth ? thread.ledger_top().unwind_floor : 0;
    uintptr_t d = A::arg(*frame, 1);
    if constexpr (!Forward) {
      if (A::arg(*frame, 4) == RETURN_STUB_MARKER)
        d = (uintptr_t)floor;
    }
    // スレッドの最外のメソッドが戻ったとき (d == 0 かつ呼出し段なし) は「スレッドの終わり」で、
    // 次のスレッドの選び方に踏み込むので kobj の exit_thread に任せる。
    if (d == 0 && thread.call_stack.call_count == 0)
      return status::FORWARD;
    const uint32_t target = (uint32_t)d;
    const uintptr_t count = target < depth ? depth - target : 0;
    const kernel_error validation = validate_return_depth(depth, floor, target, count);
    if (validation != kernel_error::OK) {
      A::set_result(*frame, (uintptr_t)object_error::UNWIND_REJECTED, (uintptr_t)validation);
      return status::DONE;
    }
    // 戻りの再開フレームは今の trap フレームと重なりうるので、戻り値は先に読んでおく。
    const uintptr_t value = A::arg(*frame, 2);
    const uintptr_t error = A::arg(*frame, 3);
    // FORWARD_CHILD_EXIT は親ハンドラが子を取り上げて戻す巻き戻しなので、1 段でも消去の目印。
    if (!unwind_in_place<K, KObj>(kobj, thread, context, frame, count, Forward, core))
      return status::CORRUPT;
    A::set_result(*frame, error, value);
    return status::DONE;
  } else {
    return status::FORWARD;
  }
}

template <class K, class KObj, bool Forward>
requires (!arch_has_core_num<typename K::ARCH>)
[[gnu::always_inline]] inline status exit_in_place(K &kernel, KObj &kobj, typename K::THREAD &thread,
                                                   typename K::CONTEXT &context) {
  return exit_in_place<K, KObj, Forward>(kernel, kobj, thread, context, no_core{});
}

} // namespace native
} // namespace shizuku
#endif // SHIZUKU_NATIVE_CALL_HPP
