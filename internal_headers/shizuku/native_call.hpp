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
#include "shizuku/kernel_abi.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/svc_attributes.hpp"

#if defined(SHIZUKU_C6_UMODE) && !defined(SHIZUKU_LEAN_LEDGER)
#error "C6 U-mode requires kernel-resident call return authority (SHIZUKU_LEAN_LEDGER)"
#endif

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
template <class A, class Context>
[[gnu::always_inline]] inline bool header_has_room_of(const Context &context, uint32_t caller_object) {
  if constexpr (arch_call_in_place<A>())
    return A::call_header_has_room(context, caller_object);
  else
    return false;
}
// caller の枠にヘッダを書いて top を進める。caller の状態は frame (trap フレーム) と context から取る。
// via / floor は台帳 (特権側) が持つので、非特権スタック上のヘッダには書かない (呼び出し側が台帳に書く)。
template <class A, class Thread, class Context, class Frame>
[[gnu::always_inline]] inline bool header_push_of(Thread &thread, Context &context, Frame &frame, uint8_t /*via*/,
                                                  uint32_t /*floor*/) {
  if constexpr (arch_call_in_place<A>()) {
    if (thread.ledger == nullptr || thread.call_stack.depth >= thread.ledger_capacity ||
        !A::call_header_has_room(context, thread.identity_object()))
      return false;
#if defined(SHIZUKU_LEAN_LEDGER)
    // 戻りの権威は特権の台帳に置く (user stack にヘッダは書かない)。段の中身 (object・handler・kind・
    // via・floor) は呼び出し側が depth - 1 の項目へ書く。ここでは戻りに要るものだけを書く。
    auto &entry = thread.ledger[thread.call_stack.depth];
    entry.generation = 0;
    entry.ret_pc = A::frame_pc(frame);
    entry.ret_sp = A::call_return_sp(context);
    entry.prev_call = (uint16_t)thread.call_stack.call_top;
    entry.ret_privileged = A::context_privileged(context) ? 1 : 0;
    thread.call_stack.depth++;
    return true;
#else
    auto *header = A::call_header_slot(context);
    header->prev = thread.call_stack.top;
    header->pc = A::frame_pc(frame);
    header->caller_object = thread.call_stack.caller_object;
    header->caller_generation = thread.call_stack.caller_generation;
    header->region_base = context.region_base;
    header->region_limit = context.region_limit;
    header->privileged = A::context_privileged(context) ? 1 : 0;
    if constexpr (requires { A::set_call_header_extra(*header, frame); }) {
      A::set_call_header_extra(*header, frame);
    }
    thread.call_stack.top = reinterpret_cast<uintptr_t>(header);
    thread.call_stack.depth++;
    return true;
#endif
  } else {
    return false;
  }
}
// 先頭ヘッダを 1 つ落とす。restore なら caller の再開フレームを作って frame を差し替える。
// is_call なら call 段が積む前の caller の識別を戻す。scrub は再開フレームの目印 (出口でレジスタを
// 消すか): callee が自分の意思で戻らない巻き戻しのときだけ true。
template <class A, class Thread, class Context, class Frame>
[[gnu::always_inline]] inline void header_pop_of(Thread &thread, Context &context, Frame *&frame, bool is_call, bool restore,
                                                 bool scrub) {
  if constexpr (arch_call_in_place<A>()) {
    auto *header = reinterpret_cast<typename A::call_header_t *>(thread.call_stack.top);
    if (is_call) {
      thread.call_stack.call_count--;
      thread.call_stack.current_call_object = thread.call_stack.caller_object;
      thread.call_stack.current_call_generation = thread.call_stack.caller_generation;
      thread.call_stack.caller_object = header->caller_object;
      thread.call_stack.caller_generation = header->caller_generation;
    }
    thread.call_stack.top = header->prev;
    if (restore)
      frame = A::call_resume(context, header, scrub);
  }
}

namespace native {

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
// 何も変えず false (不変条件の破れ)。scrub は header_pop_of を見よ。
template <class K, class KObj>
[[gnu::always_inline]] inline bool pop_in_place(KObj &kobj, typename K::THREAD &thread,
                                                typename K::CONTEXT &context,
                                                typename K::FRAME *&frame, bool restore,
                                                bool scrub) {
  using A = typename K::ARCH;
  if constexpr (arch_call_in_place<A>()) {
    const uint32_t depth = thread.call_stack.depth;
#if defined(SHIZUKU_LEAN_LEDGER)
    // PC/SP/privilege and caller identity come from protected ledger memory. The window is resolved below.
    const auto &entry = thread.ledger[depth - 1];
    const uint8_t via = entry.via;
    // The resumed caller is the preceding activation. Inspect its via directly so a
    // dispatch entry is not skipped while resolving the caller object's call chain.
    const uint8_t caller_via = depth > 1 ? thread.ledger[depth - 2].via : 0;
    const bool caller_is_dispatch = caller_via == K::LEDGER_DISPATCH;
    const bool is_call = via == K::LEDGER_CALL || via == K::LEDGER_DESCRIPTOR_CALL;
    if (is_call && thread.call_stack.call_count == 0)
      return false;
    if (entry.ret_privileged > 1u || entry.prev_call >= depth ||
        entry.prev_call > thread.ledger_capacity)
      return false;
    const uint32_t caller_object = entry.prev_call != 0
        ? thread.ledger[entry.prev_call - 1].object
        : thread.base_object;
    if (is_call) {
      thread.call_stack.call_count--;
      thread.call_stack.call_top = entry.prev_call;
    }
    if (restore) {
      frame = A::call_resume_at(context, entry.ret_pc, entry.ret_sp, entry.ret_privileged != 0,
                                0, 0, scrub);
      const auto window = resolve_return_region_window(
          kobj, caller_via, K::LEDGER_DISPATCH, caller_object);
      A::set_region_window(context, window.base, window.limit);
      if (!caller_is_dispatch && !A::context_privileged(context)) {
        if constexpr (requires { A::set_unprivileged_resume_metadata(*frame, entry.ret_sp, A::trusted_stack_top(), scrub); }) {
          A::set_unprivileged_resume_metadata(*frame, entry.ret_sp, A::trusted_stack_top(), scrub);
        }
        if constexpr (requires { A::switch_caller_protection(caller_object); }) {
          A::switch_caller_protection(caller_object);
        }
      }
    }
    thread.call_stack.depth = depth - 1;
#else
    const uint8_t via = thread.ledger[depth - 1].via;
    const uint8_t caller_via = depth > 1 ? thread.ledger[depth - 2].via : 0;
    const bool caller_is_dispatch = caller_via == K::LEDGER_DISPATCH;
    const bool is_call = via == K::LEDGER_CALL || via == K::LEDGER_DESCRIPTOR_CALL;
    if (is_call && thread.call_stack.call_count == 0)
      return false;
    header_pop_of<A>(thread, context, frame, is_call, restore, scrub);
    if (restore) {
      const uint32_t caller_object = thread.call_stack.call_count != 0
          ? thread.call_stack.current_call_object
          : thread.base_object;
      const auto window = resolve_return_region_window(
          kobj, caller_via, K::LEDGER_DISPATCH, caller_object);
      A::set_region_window(context, window.base, window.limit);
      if (!caller_is_dispatch && !A::context_privileged(context)) {
        if constexpr (requires { A::switch_caller_protection(caller_object); })
          A::switch_caller_protection(caller_object);
      }
    }
    thread.call_stack.depth = depth - 1;
#endif
  }
  return true;
}

// 台帳の上から count 段を落とす。最後に落とした段の caller だけが再開する。
// 再開の目印は、2 段以上を一度に落とす (callee が自分の意思で戻らない巻き戻し) か forced_scrub
// (FORWARD_CHILD_EXIT) のとき消去 (RV32_TAG_ECALL_SCRUB)、1 段の普通の戻りのときは素通し。
template <class K, class KObj>
[[gnu::always_inline]] inline bool unwind_in_place(KObj &kobj, typename K::THREAD &thread,
                                                   typename K::CONTEXT &context,
                                                   typename K::FRAME *&frame, uintptr_t count,
                                                   bool forced_scrub) {
  const bool scrub = forced_scrub || count > 1;
  for (uintptr_t index = 0; index < count; ++index)
    if (!pop_in_place<K>(kobj, thread, context, frame, index + 1 == count, scrub))
      return false;
  return true;
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
    const typename KObj::callee_view &view, uintptr_t entry_pc, uint8_t via) {
  using A = typename K::ARCH;
  if constexpr (arch_call_in_place<A>()) {
    auto &stack = thread.call_stack;
    // ★偽装防止: 一般の呼び先が KERNEL_OBJECT 種別や root ハンドラの ID を名乗ることは拒否する。
    if (view.kind == object_kind::KERNEL_OBJECT || view.id == kernel.svc_handler_object())
      return kernel_error::BAD_REQUEST;
    if (stack.call_count >= KObj::MAX_DEPTH)
      return kernel_error::NO_STACK;
    const uint32_t depth = stack.depth; // 呼び先の unwind floor
    // 4A: 最後の 1 枠 (ledger_capacity - 1) は return dispatch 専用に先取り予約する。
    if (depth > UINT16_MAX || thread.ledger == nullptr || depth + 1u >= thread.ledger_capacity ||
        !A::call_header_has_room(context, thread.identity_object()))
      return kernel_error::NO_STACK;
    typename K::FRAME &frame = *context.sp;
#if defined(SHIZUKU_LEAN_LEDGER)
    // 記帳は台帳の 1 項目だけ: 呼び先の同定 (object・handler・kind・via・floor・世代) と、この段から
    // 戻るときの caller の再開点 (pc・sp・特権) と、ひとつ外側の call 段への鎖 (prev_call)。窓は復帰時に解決する。
    // user stack のヘッダは書かない。current / caller の識別は call 段の列から導く (call_top だけ更新)。
    auto &entry = thread.ledger[depth];
    entry.object = view.id;
    entry.handler_object = view.parent_handler_object;
    entry.handler_entry = view.parent_handler_entry;
    entry.kind = (uint8_t)view.kind;
    entry.via = via;
    entry.unwind_floor = (uint16_t)depth;
    entry.generation = view.instance_generation;
    entry.ret_pc = A::frame_pc(frame);
    entry.ret_sp = A::call_return_sp(context);
    entry.prev_call = (uint16_t)stack.call_top;
    entry.ret_privileged = A::context_privileged(context) ? 1 : 0;
    stack.depth = depth + 1;
    stack.call_count = stack.call_count + 1;
    stack.call_top = depth + 1;
    A::set_entry(frame, entry_pc, A::return_stub());
#else
    auto *header = A::call_header_slot(context);
    header->prev = stack.top;
    header->pc = A::frame_pc(frame);
    header->caller_object = stack.caller_object;
    header->caller_generation = stack.caller_generation;
    header->region_base = context.region_base;
    header->region_limit = context.region_limit;
    // unwind_floor / via はヘッダに書かない: 同じ値を台帳 (下の thread.ledger[depth]) が持つ。
    header->privileged = A::context_privileged(context) ? 1 : 0;
    if constexpr (requires { A::set_call_header_extra(*header, frame); }) {
      A::set_call_header_extra(*header, frame);
    }
    stack.top = reinterpret_cast<uintptr_t>(header);
    stack.depth = depth + 1;
    thread.ledger[depth] = {view.id, view.parent_handler_object, view.parent_handler_entry,
                            (uint8_t)view.kind, via, (uint16_t)depth};
    const uint32_t count = stack.call_count;
    stack.caller_object = count == 0 ? thread.base_object : stack.current_call_object;
    stack.caller_generation =
        count == 0 ? stack.base_object_generation : stack.current_call_generation;
    stack.current_call_object = view.id;
    stack.current_call_generation = view.instance_generation;
    stack.call_count = count + 1;
    A::set_entry(frame, entry_pc, A::return_stub());
#endif
    A::set_priv(context, view.privileged);
    A::set_region_window(context, view.region_base, view.region_limit);
    A::switch_callee_protection(context, frame, view.id, view.privileged);
    return kernel_error::OK;
  } else {
    return kernel_error::BAD_REQUEST;
  }
}

// CALL_METHOD (4): a1 = オブジェクト ID, a2 = メソッド番号, a3 = 引数。応答は (error, 0)。
//   エラーの種類・順序: BAD_OBJECT → NOT_PRIVILEGED → BAD_METHOD → UNDECLARED_METHOD → NO_STACK。
template <class K, class KObj>
[[gnu::always_inline]] inline void call_method_in_place(K &kernel,
                                                        typename K::THREAD &thread,
                                                        typename K::CONTEXT &context,
                                                        KObj &kobj) {
  using A = typename K::ARCH;
  if constexpr (arch_call_in_place<A>()) {
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
    // 従来は呼び先を引いたあと、台帳に入る前にここで深さを断っていた (入口の BAD_REQUEST より先)。
    if (thread.call_stack.call_count >= KObj::MAX_DEPTH) {
      A::set_result(frame, (uintptr_t)object_error::NO_STACK, 0);
      return;
    }
    const uintptr_t argument = A::arg(frame, 3);
    const kernel_error entered = enter_in_place<K, KObj>(
        kernel, thread, context, kobj.view_of_row((uint32_t)id, *row), (uintptr_t)entry,
        K::LEDGER_CALL);
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

// CALL_METHOD_DESCRIPTOR (33): a1 = 記述子の index, a2..a7 = 呼び先の引数 6 個 (a0..a5 へ渡る)。
//   trap 内で表の錠 (table_lock) は取らず、版番号 seq による読み取り検証を行う。
//   項目は呼び先の前計算 (入口・種別・親・窓・特権・instance 世代) を持つので、行を引かず、表は [持ち主][index]
//   の 32 バイト項目をシフトで引く。
//   trap 内で再試行ループを回さない、錠を取らない、LR/SC を使わない。
template <class K, class KObj>
[[gnu::always_inline]] inline void call_descriptor_in_place(K &kernel,
                                                            typename K::THREAD &thread,
                                                            typename K::CONTEXT &context,
                                                            KObj &kobj) {
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

    // 2. 必要な値をローカルに写す
    const typename KObj::method_descriptor_entry local_desc = desc;

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

    // 4. 写した値に対する照合 (順序: BAD_DESCRIPTOR -> STALE_DESCRIPTOR)
    const uint8_t callee = local_desc.callee;
    if (callee == KObj::DESCRIPTOR_NONE || callee >= KObj::OBJECT_COUNT) {
      A::set_result(frame, (uintptr_t)object_error::BAD_DESCRIPTOR, 0);
      return;
    }
    if (local_desc.owner_generation != caller_generation) {
      A::set_result(frame, (uintptr_t)object_error::STALE_DESCRIPTOR, 0);
      return;
    }
    const uint32_t version = kobj.object_version(callee);
    if (version == 0 || local_desc.target_generation != version) {
      A::set_result(frame, (uintptr_t)object_error::STALE_DESCRIPTOR, 0);
      return;
    }

    // 5. 写した値から入場の形を作る (呼び先の行を引かない)。
    const kernel_error entered = enter_in_place<K, KObj>(
        kernel, thread, context, kobj.view_of_descriptor(local_desc), local_desc.target_entry,
        K::LEDGER_DESCRIPTOR_CALL);

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

// EXIT_METHOD (2, Forward == false) / FORWARD_CHILD_EXIT (27, Forward == true): 戻り先の絶対深さ
// d を検証して current_depth - d 段を落とす。a1 = d, a2 = value, a3 = error。FORWARD = kobj へ回す
// (スレッドの最外のメソッドが戻る場合 = スレッドの終わり)。
// 非 KOBJ の return stub (a4 = RETURN_STUB_MARKER、2 のみ) は d を持たないので、台帳の unwind floor
// (= 呼び先を積む前の深さ) を d とする。27 は専用 HANDLER からだけ (parent_ledger 権限)。
// 検査は primitive RETURN と同じ (validate_return_depth)。弾いたら 1 枚も落とさず
// UNWIND_REJECTED + kernel_error を返す。
template <class K, class KObj, bool Forward>
[[gnu::always_inline]] inline status exit_in_place(K &kernel, KObj &kobj, typename K::THREAD &thread,
                                                   typename K::CONTEXT &context) {
  using A = typename K::ARCH;
  if constexpr (arch_call_in_place<A>()) {
    typename K::FRAME *frame = context.sp;
    const uint32_t depth = thread.call_stack.depth;
    if constexpr (Forward) {
      // 出どころ確認。発行元の段 (台帳の最上段、dispatch 段は積まれていない) が専用 HANDLER で、
      // root kobj 本人ではないこと。
      const bool from_handler =
          depth != 0 &&
          thread.ledger[depth - 1].kind == (uint8_t)object_kind::HANDLER &&
          thread.ledger[depth - 1].object != kernel.svc_handler_object();
      if (!svc_permission_allows((uintptr_t)object_api::FORWARD_CHILD_EXIT,
                                 !is_context_unprivileged(&context), false, from_handler)) {
        A::set_result(*frame, (uintptr_t)object_error::NOT_PRIVILEGED, 0);
        return status::DONE;
      }
    }
    const uint32_t floor = depth ? thread.ledger[depth - 1].unwind_floor : 0;
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
    if (!unwind_in_place<K, KObj>(kobj, thread, context, frame, count, Forward))
      return status::CORRUPT;
    A::set_result(*frame, error, value);
    return status::DONE;
  } else {
    return status::FORWARD;
  }
}

} // namespace native
} // namespace shizuku
#endif // SHIZUKU_NATIVE_CALL_HPP
