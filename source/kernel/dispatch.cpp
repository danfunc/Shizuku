// ===========================================================================
//  svc の受け口とプリミティブ — カーネルの中核 (DESIGN §6.2 / §7 / §8 / §9)
// ===========================================================================
//  ここは**カーネルの svc ハンドラ** (例外文脈で走る機構)。カーネルオブジェクトが
//  持つ**オブジェクトランドの svc ハンドラ** (スレッドモードで走る方針側) とは
//  別概念で、ここでは後者を「登録された entry」としてしか扱わない。
//
//  ★経路は ledger の実行主体で決まる (I-1):
//      PLAIN / HANDLER → 登録済み親ハンドラへ dispatch
//      登録済み KOBJ  → KOBJ 本人が kernel primitive を解釈
//    種別は特権 call ledger から読み、遷移させるのはカーネルだけ:
//    CALL では呼び先の申告 (call_request::callee_kind) を載せ、トランポリンでは
//    HANDLER を載せ、戻るときは ledger の段を pop する。
//    オブジェクト側からは書けないので偽装できない。
//
//  ★★2026-09-05: ここは長らく「ID 0 = カーネルオブジェクト」の決め打ちで動いて
//    いた (第 2 世代にあった種別フィールドが削除された退行)。種別を独立した値と
//    して復活させ、判定を種別に戻したのがこの版。採らなかった案を残しておく:
//    (1) **段数のパリティ**: 「ハンドラ = カーネルオブジェクト」を仮定しており、
//        **一般オブジェクトがハンドラを務める構成 (マルチ ABI) で崩れる**。
//        マルチ ABI はこの系の主張そのものなので採れない。冒頭にあった I-1 の
//        「パリティで決まる」という記述はその意味で誤りだった。
//        ※ 実測ではパリティでも自己テストは通った (96 passed / 3 failed で既定と
//          一致、費用も誤差内) が、**ハンドラが 1 つしか居ないから通っただけ**で
//          正しさの証明にはならない。
//    (2) **カーネル内に属性表を置いて ID で引く** (参照実装の形): D1 を緩めた今は
//        禁じ手ではないが、カーネルがオブジェクト空間の大きさを知る必要が出る。
//        種別は呼び出しのたびに kobj が申告すれば足りるので、表は持たない。
//    ★ID で代用しないことが要点。ID は名前であって役ではなく、決め打つと kobj を
//      差し替えることも多重化することもできない。
//
//  ★RETURN を解釈できるのは登録済み KOBJ だけ。各 dispatch 先には呼出し鎖の
//    unwind floor を第 5 引数で渡し、exit API は絶対戻り深さ d を指定する。
#if __has_include("shizuku/config.hpp")
#include "shizuku/kernel.hpp"
#endif
#include "shizuku/object_api.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/svc_attributes.hpp"
#include "shizuku/native_call.hpp"
#include "shizuku/hot.hpp"
#include <array>
#if defined(__arm__) || defined(__thumb__)
extern "C" char shizuku_v2_service_resume;
#endif

#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
extern "C" void shizuku_boot_trace_phase(uint32_t);
extern "C" void shizuku_boot_trace_push(uint32_t);
extern "C" void shizuku_boot_trace_push_context(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
extern "C" void shizuku_boot_trace_primitive_number(uint32_t);
extern "C" void shizuku_boot_trace_call_enter(uint32_t, uint32_t);
extern "C" void shizuku_boot_trace_call_leave(uint32_t, uint32_t);
extern "C" void shizuku_boot_trace_return_enter(uint32_t, uint32_t);
extern "C" void shizuku_boot_trace_return_leave(uint32_t, uint32_t);
extern "C" void shizuku_boot_trace_primitive_stage(uint32_t);
#endif

namespace shizuku {

// trap 内で直接処理する API の種別 (密な連番)。番号 → 種別の表引きで O(1) に振り分ける。
// 表は svc_attributes の mode == handler の集合と一致することを static_assert で縛る。
enum class trap_api : uint8_t {
  NONE = 0,
  GET_CURRENT,
  GET_CALLER,
  YIELD,
  EXIT_METHOD,
  FORWARD_CHILD_EXIT,
  CALL_METHOD,
  RUN_FOR,
  SET_BUDGET,
  CALL_DESCRIPTOR,
};
inline constexpr auto trap_api_table = [] {
  std::array<trap_api, 34> t{};
  t[(unsigned)object_api::GET_CURRENT_OBJECT] = trap_api::GET_CURRENT;
  t[(unsigned)object_api::GET_CALLER_OBJECT] = trap_api::GET_CALLER;
  t[(unsigned)object_api::YIELD] = trap_api::YIELD;
  t[(unsigned)object_api::EXIT_METHOD] = trap_api::EXIT_METHOD;
  t[(unsigned)object_api::FORWARD_CHILD_EXIT] = trap_api::FORWARD_CHILD_EXIT;
  t[(unsigned)object_api::CALL_METHOD] = trap_api::CALL_METHOD;
  t[(unsigned)object_api::RUN_FOR] = trap_api::RUN_FOR;
  t[(unsigned)object_api::SET_BUDGET] = trap_api::SET_BUDGET;
  t[(unsigned)object_api::CALL_METHOD_DESCRIPTOR] = trap_api::CALL_DESCRIPTOR;
  return t;
}();
constexpr trap_api trap_api_of(uintptr_t id) {
  return id < trap_api_table.size() ? trap_api_table[id] : trap_api::NONE;
}
// svc_attributes の handler 集合と表が食い違わないこと。権限検査を FORWARD_CHILD_EXIT だけで
// 行うので、それ以外の handler API は権限制限なし (unprivileged_allowed・authority なし) であること。
constexpr bool trap_api_table_consistent() {
  for (unsigned id = 0; id < trap_api_table.size(); ++id) {
    const auto *a = svc_attribute_for(id);
    const bool handler = a != nullptr && a->mode == svc_mode::handler;
    if (handler != (trap_api_table[id] != trap_api::NONE)) return false;
    if (handler && id != (unsigned)object_api::FORWARD_CHILD_EXIT &&
        (a->permission != svc_permission::unprivileged_allowed ||
         a->authority != svc_authority::none))
      return false;
  }
  return true;
}
static_assert(trap_api_table_consistent(),
              "trap_api_table must match svc_attributes (mode == handler)");

// ISA 層の例外入口が退避先・復帰先として使う文脈。dispatch が現在スレッドを
// 差し替えれば、そのまま切替になる (退避後と復帰前の 2 回呼ばれる)。
template <> KERNEL::CONTEXT *KERNEL::current_context() {
  return m_threads[m_current[BOARD::core_num()]].thread.context;
}

template <> void KERNEL::set_object_handler(uintptr_t entry_pc,
                                            uint32_t object_id) {
  m_object_svc_handler = entry_pc;
  m_object_svc_handler_object = object_id;
}

template <> uintptr_t KERNEL::current_object(uint32_t thread_id) const {
  if (thread_id >= m_thread_count)
    return (uintptr_t)KERNEL_OBJECT::NO_OBJECT;
  const THREAD &thread = m_threads[thread_id].thread;
  return thread.call_stack.call_count != 0
             ? thread.call_stack.current_call_object
             : thread.base_object;
}

template <> uintptr_t KERNEL::caller_object(uint32_t thread_id) const {
  if (thread_id >= m_thread_count)
    return (uintptr_t)KERNEL_OBJECT::NO_OBJECT;
  const THREAD &thread = m_threads[thread_id].thread;
  return thread.call_stack.call_count != 0
             ? thread.call_stack.caller_object
             : (uintptr_t)KERNEL_OBJECT::NO_OBJECT;
}


#if defined(SHIZUKU_DEEP_DIAG)
extern "C" { volatile uint32_t shizuku_push_diag[6]; volatile uint32_t shizuku_push_diag_n; volatile uint32_t shizuku_push_diag_kind; volatile uint32_t shizuku_push_diag_reject_kind; }
#endif
template <>
SHIZUKU_HOT bool KERNEL::call_frame_push(KERNEL::THREAD &thread, KERNEL::CONTEXT *context,
                             KERNEL::FRAME **frame) {
  if (arch_call_in_place<ARCH>()) {
    // ヘッダ枠方式: frame はコピーせず、caller の枠に書くだけ。via / floor はヘッダの参考値で、
    // 呼び出し側が台帳へ書いたあとに合わせる (ここでは 0)。
    return header_push_of<ARCH>(thread, *context, **frame, 0, 0);
  }
  const uint32_t frame_bytes = ARCH::exc_frame_bytes(*context);
  const uint32_t stack_arg_bytes = ARCH::HANDLER_STACK_ARG_BYTES;
  uint32_t total = (uint32_t)sizeof(call_frame_header) + frame_bytes +
                   stack_arg_bytes;
  total = (total + 7u) & ~7u; // 8B 境界を保つ
  // 呼び出し元の生スタック境界。元の例外フレームはこのすぐ下に居る。
  const uintptr_t caller_stack = (uintptr_t)context->sp + frame_bytes;
  const uintptr_t snapshot = caller_stack - total; // 呼び先の復帰 SP / stack arg area
  const uintptr_t header_address = snapshot + stack_arg_bytes;
  const uintptr_t callee_frame = snapshot - frame_bytes;
  if (thread.ledger == nullptr || thread.call_stack.depth >= thread.ledger_capacity) {
#if defined(SHIZUKU_DEEP_DIAG)
    // 台帳超過は diag[0..5] と _n に触れない (意味を変えないため)。経路だけ残す。
    shizuku_push_diag_reject_kind = 0x100 | shizuku_push_diag_kind;
#endif
    return false; // 台帳容量超過は呼び出し側が NO_STACK にする
  }

  // スタック下限の手前で止める。ここで false を返せば呼び出し側がエラーを返すので、
  // スタック不足が無音ロックアップにならない (I-9)。
  // ★★余白は**この構造体の大きさから算術で出す** (2026-09-05)。ARCH::CALL_HEADROOM
  //   という固定値だけを見ていた頃は、ヘッダに 1 語足すと余白がその分だけ痩せ、
  //   「NO_STACK を返すはずが PSPLIM の UsageFault で無言ロックアップ」に化けた。
  //   ★実測: 種別フィールドを足した最初の試み (2026-09-05) が実機で起動しなく
  //     なった原因はこれ。**中身に関係なくヘッダを 1 語太らせるだけで同じように
  //     死ぬ**ことを詰め物で確認した (call_ladder のスタック掘り切りで固まり、
  //     フォールト報告すら出ない = 一番情報の少ない壊れ方)。
  //   呼び先が最低 1 回はカーネルを呼び返せること = ハンドラへのトランポリンを
  //   もう 1 枚積めること、なので**その 1 枚ぶんは構造から計算して足す**。
  //   ARCH::CALL_HEADROOM に残るのは「ハンドラ連鎖の C フレーム」の見積もりだけで、
  //   カーネルの幾何が変わってもそちらは動かなくてよい。
  //   ★2026-10-05: 上の 1 枚ぶんに加え、呼び先で fault が起きた場合に積まれる
  //     量も算術で足す。ハードが積む最大の例外フレーム (FPU 拡張 104B) と、fault
  //     入口の CTX_SAVE が退避する文脈。これが無いと、トランポリンは積めるのに
  //     その後の fault 入口が PSPLIM を割り、フォールト報告すら出ず lockup した。
  const uintptr_t reserve = (uintptr_t)sizeof(call_frame_header) + frame_bytes +
                            stack_arg_bytes +
                            ARCH::EXC_FRAME_MAX_BYTES +
                            ARCH::FAULT_CONTEXT_BYTES + ARCH::CALL_HEADROOM;
  const uintptr_t limit = ARCH::stack_limit(*context);
  if (limit != 0 && callee_frame < limit + reserve) {
#if defined(SHIZUKU_DEEP_DIAG)
    shizuku_push_diag[0] = (uint32_t)callee_frame; shizuku_push_diag[1] = (uint32_t)limit;
    shizuku_push_diag[2] = (uint32_t)reserve; shizuku_push_diag[3] = (uint32_t)sizeof(call_frame_header);
    shizuku_push_diag[4] = (uint32_t)thread.call_stack.depth; shizuku_push_diag[5] = ++shizuku_push_diag_n;
    shizuku_push_diag_reject_kind = shizuku_push_diag_kind;
#endif
    return false;
  }

  call_frame_header *header = (call_frame_header *)header_address;
  header->prev = thread.call_stack.top;
  header->total_bytes = total;
  header->frame_bytes = frame_bytes;
  // ★識別情報は非特権から届かない特権 ledger に置く。ここには復帰用文脈だけを残す。
  header->saved = *context; // sp を含めて丸ごと (= 元フレームの位置も記録される)

  // ★元の例外フレームは動かさない (I-3)。下へ複製するのは書き換え用の作業コピー。
  __builtin_memcpy((void *)callee_frame, (const void *)context->sp, frame_bytes);
  context->sp = (FRAME *)callee_frame;
  // 作業コピーは 8B 境界に置いてあるので、復帰時の追加調整を消しておく (I-4)。
  ARCH::normalize_frame(*context->sp);
  // ★幾何の検算: 呼び先が復帰した直後の SP が退避域の底と厳密に一致すること。
  //   ここがズレるとヘッダを踏み潰して無言で壊れる (実機で踏んだ落とし穴)。
  //   破れたらカーネル自身の不変条件の破れなので panic してよい (D12)。
  if (ARCH::psp_after_return(*context) != snapshot)
    BOARD::panic("call frame geometry mismatch");

  *frame = context->sp;
  thread.call_stack.top = header_address;
  thread.call_stack.depth++;
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_push_context(
      (uint32_t)callee_frame,
      (uint32_t)m_current[BOARD::core_num()],
      thread.call_stack.depth,
      thread.current_object,
      thread.current_kind,
      thread.current_handler_object);
#endif
  return true;
}

template <>
SHIZUKU_HOT bool KERNEL::call_frame_pop(KERNEL::THREAD &thread, KERNEL::CONTEXT *context,
                            KERNEL::FRAME **frame, bool restore, bool scrub) {
  if (thread.call_stack.top == 0)
    return false;
  if (arch_call_in_place<ARCH>()) {
    // ヘッダ枠方式: ヘッダから caller の識別・pc・特権・窓を読み、最後の 1 段だけ caller の
    // 再開フレームを caller の SP の直下に作る (a0/a1 以外は素通し。scrub のときは出口が消す)。
    // frame のコピーも戻しも無い。
    // (call の段の識別の戻しと underflow の検査は trap 内の EXIT_METHOD と共有: native_call.hpp)
    if (!native::pop_in_place<KERNEL>(thread, *context, *frame, restore, scrub))
      BOARD::panic("call ledger count underflow");
    return true;
  }
  // 落とす段が call の段 (LEDGER_CALL / LEDGER_DESCRIPTOR_CALL) か。identity の復元が要る。
  const bool is_call =
      thread.call_stack.depth > 0 &&
      (thread.ledger[thread.call_stack.depth - 1].via == LEDGER_CALL ||
       thread.ledger[thread.call_stack.depth - 1].via == LEDGER_DESCRIPTOR_CALL);
  if (is_call && thread.call_stack.call_count == 0)
    BOARD::panic("call ledger count underflow");
  // ★幾何は push 時に記録した値を読み戻す。再計算しない (I-5)。
  // 元の例外フレームは退避域の中に元の位置のまま生きているので、文脈を丸ごと
  // 戻すだけで復帰先が正しく決まる (書き戻しも再配置も不要)。
  const call_frame_header *header = (const call_frame_header *)thread.call_stack.top;
  const uintptr_t previous = header->prev;
  *context = header->saved;
  *frame = context->sp;
  if (is_call) {
    thread.call_stack.call_count--;
    const uint32_t restore_index = thread.call_stack.call_count;
    thread.call_stack.current_call_object = thread.call_stack.caller_object;
    thread.call_stack.current_call_generation = thread.call_stack.caller_generation;
    thread.call_stack.caller_object =
        thread.call_stack.identity[restore_index].previous_caller_object;
    thread.call_stack.caller_generation =
        thread.call_stack.identity[restore_index].previous_caller_generation;
  }
  thread.call_stack.top = previous;
  thread.call_stack.depth--;
  return true;
}

template <>
SHIZUKU_HOT void KERNEL::call_unwind(KERNEL::THREAD &thread, KERNEL::CONTEXT *context,
                                     KERNEL::FRAME **frame, uintptr_t count) {
  // 途中の段は再開しない。最後に落とす段の caller だけが走り出す。2 段以上を一度に落とす
  // 巻き戻しは callee が自分の意思で戻らないので、再開の出口でレジスタを消す (scrub)。
  const bool scrub = count > 1;
  for (uintptr_t index = 0; index < count; ++index)
    call_frame_pop(thread, context, frame, index + 1 == count, scrub);
}

// 通常 CALL と方法記述子の直接 call が共有する前半。台帳段を via (LEDGER_CALL か
// LEDGER_DESCRIPTOR_CALL) で積み、戻りの床を積む前の深さに置く。呼び先の入口・引数・保護は
// 呼び出し側が決める。
template <>
SHIZUKU_HOT kernel_error KERNEL::enter_call(KERNEL::THREAD &thread, KERNEL::CONTEXT *context,
                                KERNEL::FRAME **frame,
                                const call_request &request, uint8_t via) {
  // ★偽装防止 (指摘4): 一般 call_request から KERNEL_OBJECT 種別や
  //   m_object_svc_handler_object を名乗ることは拒否する。
  if (request.callee_kind == (uint32_t)object_kind::KERNEL_OBJECT ||
      request.callee_object == m_object_svc_handler_object) {
    return kernel_error::BAD_REQUEST;
  }
  if (thread.call_stack.call_count >= KERNEL_OBJECT::MAX_DEPTH)
    return kernel_error::NO_STACK;
  const uint32_t unwind_floor = thread.call_stack.depth;
  if (!arch_call_in_place<ARCH>()) {
    // 呼び先の普通の return は svc_dispatch 経由でもう 1 枚 call_frame_push する。
    // その 1 枚ぶんの余白を CALL 時に先取りする (全 arch 共通。2026-10-21 実機で
    // RV32 の OBJECT_DEEP が最深で return を断られ 0xE005 停止したため)。
    // identity の退避先は固定配列 (ARM)。ヘッダ枠方式 (RV32) は上限なし (MAX_DEPTH は上で検査)。
    if (thread.call_stack.call_count >= THREAD::CALL_IDENTITY_CAPACITY)
      return kernel_error::NO_STACK;
    if (!ARCH::call_entry_has_return_room(*context, sizeof(call_frame_header)))
      return kernel_error::NO_STACK;
  }
  if (unwind_floor > UINT16_MAX ||
      !(arch_call_in_place<ARCH>()
            ? header_push_of<ARCH>(thread, *context, **frame, via, unwind_floor)
            : call_frame_push(thread, context, frame)))
    return kernel_error::NO_STACK;
  if (!arch_call_in_place<ARCH>())
    thread.call_stack.identity[thread.call_stack.call_count] = {
        thread.call_stack.caller_object, thread.call_stack.caller_generation};
  auto &entry = thread.ledger[thread.call_stack.depth - 1];
  entry = {request.callee_object, request.parent_handler_object,
           request.parent_handler_entry, (uint8_t)request.callee_kind,
           via, (uint16_t)unwind_floor};
  thread.call_stack.caller_object = thread.call_stack.call_count == 0
                                        ? thread.base_object
                                        : thread.call_stack.current_call_object;
  thread.call_stack.caller_generation = thread.call_stack.call_count == 0
                                            ? thread.call_stack.base_object_generation
                                            : thread.call_stack.current_call_generation;
  thread.call_stack.current_call_object = request.callee_object;
  thread.call_stack.current_call_generation = request.callee_generation;
  thread.call_stack.call_count++;
  return kernel_error::OK;
}

template <>
kernel_error KERNEL::do_call(KERNEL::THREAD &thread, KERNEL::CONTEXT *context,
                             KERNEL::FRAME **frame,
                             const call_request &request) {
  if (request.entry_pc == 0)
    return kernel_error::BAD_REQUEST;
#if defined(SHIZUKU_RP2040)
  if (!ARCH::region_range_representable(request.region_base, request.region_limit))
    return kernel_error::BAD_REQUEST;
#endif
#if defined(SHIZUKU_DEEP_DIAG)
  shizuku_push_diag_kind = 1; // do_call 経由の push
#endif
  const kernel_error error = enter_call(thread, context, frame, request, LEDGER_CALL);
  if (error != kernel_error::OK)
    return error;
  // 戻り口は常にカーネルの 1 本。撃つ svc は同じでも、そこから出たときの**種別**で
  // 「プリミティブとしての巻き戻し」か「メソッドが戻った知らせ」かが決まる
  // (発行側が戻り口を選ぶ必要は無い)。
  ARCH::set_entry(**frame, request.entry_pc, ARCH::return_stub());
  ARCH::set_args(**frame, request.args);
  ARCH::set_priv(*context, (request.protection & PROTECTION_UNPRIVILEGED) == 0);
  ARCH::set_region_window(*context, request.region_base, request.region_limit);
  return kernel_error::OK;
}

// ---------------------------------------------------------------------------
//  svc の処理を呼び出し元の立場と番号ごとに分けたもの。svc_dispatch はこれらを順に
//  判定して呼ぶ薄い振り分けで、arch の例外入口 (RV32) は番号の表から同じ関数へ直接入る
//  (svc_native_route と svc_entry を使う)。どちらから入っても結果は同じ。
// ---------------------------------------------------------------------------

// 呼び出し元が root KernelObject 宛ての native API を撃てる立場か (svc_native_route) は
//   kernel.hpp に inline で置く: svc_dispatch の「kobj 本人でない」「親ハンドラが登録済み」
//   「宛先が root」の 3 つの判定をまとめたもの。

// 呼び出し元が登録済み kernel object 本人のとき: 番号は primitive の番号空間 (CALL / RETURN /
// SWITCH / GRANT)。
template <> SHIZUKU_HOT void KERNEL::svc_primitive(KERNEL::CONTEXT *context) {
  THREAD &thread = current_thread();
  FRAME *frame = context->sp;
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_phase(411);
#endif
  const uintptr_t number = ARCH::arg(*frame, 0);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_primitive_number((uint32_t)number);
#endif
  switch ((primitive)number) {
  case primitive::CALL: {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    shizuku_boot_trace_call_enter((uint32_t)context->sp, (uint32_t)frame);
#endif
    const call_request *pointer = (const call_request *)ARCH::arg(*frame, 1);
    if (pointer == nullptr) {
      ARCH::set_result(*frame, (uintptr_t)kernel_error::BAD_REQUEST, 0);
      break;
    }
    const call_request request = *pointer;
    const kernel_error error = do_call(thread, context, &frame, request);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    shizuku_boot_trace_call_leave((uint32_t)context->sp, (uint32_t)frame);
#endif
    if (error != kernel_error::OK)
      ARCH::set_result(*frame, (uintptr_t)error, 0);
    break;
  }
  case primitive::RETURN: {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    shizuku_boot_trace_return_enter((uint32_t)context->sp, (uint32_t)frame);
#endif
    uintptr_t count = ARCH::arg(*frame, 1);
    const uintptr_t value = ARCH::arg(*frame, 2);
    const uintptr_t error = ARCH::arg(*frame, 3);
    const uint32_t depth = thread.call_stack.depth;
    const uintptr_t target_arg = ARCH::arg(*frame, 4);
    const uint32_t floor = current_unwind_floor();
    uint32_t target_depth;
    if (target_arg == RETURN_STUB_MARKER) {
      const auto request = resolve_return_stub(depth);
      target_depth = request.target_depth;
      count = request.count;
    } else if (target_arg > UINT32_MAX) {
      ARCH::set_result(*frame, (uintptr_t)kernel_error::DEPTH_MISMATCH,
                       depth);
      break;
    } else {
      target_depth = (uint32_t)target_arg;
    }
    const kernel_error validation = validate_return_depth(
        depth, floor, target_depth, count);
    if (validation != kernel_error::OK) {
      ARCH::set_result(*frame, (uintptr_t)validation, depth);
      break;
    }
    call_unwind(thread, context, &frame, count);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    shizuku_boot_trace_return_leave((uint32_t)context->sp, (uint32_t)frame);
#endif
    ARCH::set_result(*frame, error, value);
    break;
  }
  case primitive::SWITCH: {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    shizuku_boot_trace_primitive_stage(417);
#endif
    const kernel_error error = do_switch((uint32_t)ARCH::arg(*frame, 1));
    ARCH::set_result(*frame, (uintptr_t)error, 0);
    break;
  }
  case primitive::GRANT: {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    shizuku_boot_trace_primitive_stage(418);
#endif
    const uint32_t target = (uint32_t)ARCH::arg(*frame, 1);
    const uint32_t cycles = (uint32_t)ARCH::arg(*frame, 2);
    ARCH::set_result(*frame, (uintptr_t)kernel_error::OK,
                     (uintptr_t)grant_end::YIELDED);
    const kernel_error error = do_grant(target, cycles);
    if (error != kernel_error::OK)
      ARCH::set_result(*frame, (uintptr_t)error, 0);
    break;
  }
  default:
    BOARD::panic("unknown kernel primitive");
    break;
  }
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_primitive_stage(425);
#endif
}

// ---- trap 内で直接処理する native API (O(1)、待たない、ループしない) --------------------
// 前提: svc_native_route() (root KernelObject 宛て、呼び出し元は kobj 本人でない)。
// 戻り値 true = 処理した (応答を書いた)、false = kobj へ回す。

template <> SHIZUKU_HOT bool KERNEL::svc_get_current(KERNEL::CONTEXT *context) {
  ARCH::set_result(*context->sp, (uintptr_t)object_error::OK,
                   current_object(current_thread_id()));
  return true;
}

template <> SHIZUKU_HOT bool KERNEL::svc_get_caller(KERNEL::CONTEXT *context) {
  ARCH::set_result(*context->sp, (uintptr_t)object_error::OK,
                   caller_object(current_thread_id()));
  return true;
}

// YIELD (8): CPU を渡してくれた相手 (grant 段の先頭の貸し手) へ返す。O(1)。
//   grant_unwind(YIELDED) で貸し手の応答 (OK, YIELDED) が書かれ、自分の応答 (OK, 0) は
//   次に実行権を貰って再開したときのために先に置いておく。
//   grant 段が無い (貸し手が居ない = 起点のスレッド) ときは、起点のスレッドがスケジューラ役
//   なので従来どおり kobj の schedule() へ転送する (false)。
template <> SHIZUKU_HOT bool KERNEL::svc_yield(KERNEL::CONTEXT *context) {
  if (!grant_active())
    return false;
  ARCH::set_result(*context->sp, (uintptr_t)object_error::OK, 0);
  grant_unwind(grant_end::YIELDED);
  return true;
}

// EXIT_METHOD (2) / FORWARD_CHILD_EXIT (27): 戻り先の絶対深さ d を検証して
//   current_depth - d 段を落とす。27 は専用 HANDLER からだけ (parent_ledger 権限)。
//   a1 = d, a2 = value, a3 = error。非 KOBJ の return stub (a4 = RETURN_STUB_MARKER、2 のみ) は
//   d を持たないので、台帳の unwind floor (= 呼び先を積む前の深さ) を d とする。
//   検査は primitive RETURN と同じ (validate_return_depth)。弾いたら 1 枚も落とさず
//   UNWIND_REJECTED + kernel_error を呼び出し元へ返す (従来の exit_method の reply と同じ)。
//   従来は dispatch 段が 1 枚積まれていたため d == current_depth も通ったが、今は段が
//   無いので d >= current_depth は DEPTH_MISMATCH で弾く。
template <> SHIZUKU_HOT bool KERNEL::svc_exit(KERNEL::CONTEXT *context, bool forward) {
  THREAD &thread = current_thread();
  // ヘッダ枠方式 (RV32) の本体は native_call.hpp (例外入口が 1 関数に展開するものと同じ)。
  if (arch_call_in_place<ARCH>()) {
    const native::status result =
        forward ? native::exit_in_place<KERNEL, true>(*this, thread, *context)
                : native::exit_in_place<KERNEL, false>(*this, thread, *context);
    if (result == native::status::CORRUPT)
      BOARD::panic("call ledger count underflow");
    return result == native::status::DONE;
  }
  FRAME *frame = context->sp;
  if (forward) {
    // 出どころ確認。発行元の段 (台帳の最上段、dispatch 段は積まれていない) が専用 HANDLER で、
    // root kobj 本人ではないこと。従来の forward_child_exit は dispatch 段の 1 つ下
    // (ledger_entry(thread, 1)) を見ていた。
    const bool from_handler =
        thread.call_stack.depth != 0 &&
        thread.ledger_top().kind == (uint8_t)object_kind::HANDLER &&
        thread.ledger_top().object != m_object_svc_handler_object;
    if (!svc_permission_allows((uintptr_t)object_api::FORWARD_CHILD_EXIT,
                               !is_context_unprivileged(context), false,
                               from_handler)) {
      ARCH::set_result(*frame, (uintptr_t)object_error::NOT_PRIVILEGED, 0);
      return true;
    }
  }
  const uint32_t depth = thread.call_stack.depth;
  const uint32_t floor = current_unwind_floor();
  const bool from_stub = !forward && ARCH::arg(*frame, 4) == RETURN_STUB_MARKER;
  const uintptr_t d = from_stub ? (uintptr_t)floor : ARCH::arg(*frame, 1);
  // スレッドの最外のメソッドが戻ったとき (d == 0 かつ呼出し段なし) は「スレッドの
  // 終わり」で、次のスレッドの選び方に踏み込むので kobj の exit_thread に任せる
  // (EXIT_THREAD が trap 内に入るまでは従来経路)。
  if (d == 0 && thread.call_stack.call_count == 0)
    return false;
  const uint32_t target = (uint32_t)d;
  const uintptr_t count = target < depth ? depth - target : 0;
  const kernel_error validation =
      validate_return_depth(depth, floor, target, count);
  if (validation != kernel_error::OK) {
    ARCH::set_result(*frame, (uintptr_t)object_error::UNWIND_REJECTED,
                     (uintptr_t)validation);
    return true;
  }
  const uintptr_t value = ARCH::arg(*frame, 2);
  const uintptr_t error = ARCH::arg(*frame, 3);
  call_unwind(thread, context, &frame, count);
  ARCH::set_result(*frame, error, value);
  return true;
}

template <> SHIZUKU_HOT bool KERNEL::svc_exit_method(KERNEL::CONTEXT *context) {
  return svc_exit(context, false);
}

template <> SHIZUKU_HOT bool KERNEL::svc_forward_child_exit(KERNEL::CONTEXT *context) {
  return svc_exit(context, true);
}

// CALL_METHOD (4): a1 = オブジェクト ID, a2 = メソッド番号, a3 = 引数。
//   kobj の中継 (call_method → primitive CALL) をやめ、m_objects[id].methods[method] を
//   O(1) で引いて呼び先へ直接入る。台帳段は本当の呼び出し元の上に積まれるので、
//   呼び先から見た GET_CALLER_OBJECT は呼び出し元そのものになる。
//   エラーの種類・順序は従来と同じ (BAD_OBJECT → BAD_METHOD → UNDECLARED_METHOD →
//   NO_STACK)。応答は (error, 0)。
template <> SHIZUKU_HOT bool KERNEL::svc_call_method(KERNEL::CONTEXT *context) {
  THREAD &thread = current_thread();
  // ヘッダ枠方式 (RV32) の本体は native_call.hpp (例外入口が 1 関数に展開するものと同じ)。
  if (arch_call_in_place<ARCH>()) {
    native::call_method_in_place(*this, thread, *context, kernel_object_instance);
    return true;
  }
  FRAME *frame = context->sp;
  call_request request{};
  const object_error lookup = kernel_object_instance.method_call_request(
      ARCH::arg(*frame, 1), ARCH::arg(*frame, 2), request);
  if (lookup != object_error::OK) {
    ARCH::set_result(*frame, (uintptr_t)lookup, 0);
    return true;
  }
  if (thread.call_stack.call_count >= KERNEL_OBJECT::MAX_DEPTH) {
    ARCH::set_result(*frame, (uintptr_t)object_error::NO_STACK, 0);
    return true;
  }
  request.args[0] = ARCH::arg(*frame, 3);
  const kernel_error entered = do_call(thread, context, &frame, request);
  // 入れなかったときは台帳に段が残らない (frame もそのまま = 呼び出し元の svc フレーム)。
  if (entered != kernel_error::OK)
    ARCH::set_result(*frame, (uintptr_t)(entered == kernel_error::NO_STACK
                                             ? object_error::NO_STACK
                                             : object_error::BAD_OBJECT),
                     0);
  return true;
}

// RUN_FOR (9): a1 = スレッド ID, a2 = クロック数。GRANT をそのまま撃つ。
//   応答は貸し手が戻ったときに grant_unwind が (OK, grant_end) で書く。先に既定値
//   (OK, YIELDED) を置いておく点は primitive GRANT と同じ。貸せなかったときの
//   変換は従来どおり: GRANT_TOO_SMALL → NO_TIME (こちらに配れる持ち分が無い。
//   NOT_RUNNABLE と区別しないとスケジューラが空回りする)、それ以外 → NOT_RUNNABLE。
template <> SHIZUKU_HOT bool KERNEL::svc_run_for(KERNEL::CONTEXT *context) {
  FRAME *frame = context->sp;
  // 引数は応答の既定値を置く前に読む (応答は同じ a0/a1 に載る)。
  const uint32_t target = (uint32_t)ARCH::arg(*frame, 1);
  const uint32_t cycles = (uint32_t)ARCH::arg(*frame, 2);
  ARCH::set_result(*frame, (uintptr_t)object_error::OK, (uintptr_t)grant_end::YIELDED);
  const kernel_error granted = do_grant(target, cycles);
  if (granted != kernel_error::OK)
    ARCH::set_result(*frame,
                     (uintptr_t)(granted == kernel_error::GRANT_TOO_SMALL
                                     ? object_error::NO_TIME
                                     : object_error::NOT_RUNNABLE),
                     0);
  return true;
}

// SET_BUDGET (12): a1 = スレッド ID, a2 = クロック数。範囲外は黙って何もしない。
template <> SHIZUKU_HOT bool KERNEL::svc_set_budget(KERNEL::CONTEXT *context) {
  FRAME *frame = context->sp;
  kernel_object_instance.set_budget(ARCH::arg(*frame, 1), ARCH::arg(*frame, 2));
  ARCH::set_result(*frame, (uintptr_t)object_error::OK, 0);
  return true;
}

// CALL_METHOD_DESCRIPTOR (33): 記述子の引き・世代・権限の検査は表の錠の中、台帳段は通常の CALL と
//   同じ形 (via = LEDGER_DESCRIPTOR_CALL)。本体は native_call.hpp (例外入口が 1 関数に展開する
//   ものと同じ)。ヘッダ枠方式でない ARCH (ARM) は未対応 (UNKNOWN_API)。
//   Region grants can change after descriptor creation. The live trusted window is used,
//   while keeping authorization out of the O(1) call path.
template <> SHIZUKU_HOT bool KERNEL::svc_call_descriptor(KERNEL::CONTEXT *context) {
  if (!arch_call_in_place<ARCH>()) {
    ARCH::set_result(*context->sp, (uintptr_t)object_error::UNKNOWN_API, 0);
    return true;
  }
#if defined(SHIZUKU_DEEP_DIAG)
  shizuku_push_diag_kind = 3; // CALL_METHOD_DESCRIPTOR 経路 (riscv のみ)
#endif
  native::call_descriptor_in_place(*this, current_thread(), *context, kernel_object_instance);
  return true;
}

// 親ハンドラ (root KernelObject か専用ハンドラ) への配送: dispatch 段を積み、親ハンドラの
// 入口へ svc の引数を渡して入る。kobj へ回す thread mode の API と、専用の親ハンドラ宛ての
// raw svc (全番号、無加工) がここを通る。
template <> SHIZUKU_HOT void KERNEL::svc_forward(KERNEL::CONTEXT *context, bool service_gate) {
  THREAD &thread = current_thread();
  FRAME *frame = context->sp;
  const uintptr_t target_entry = service_gate ? m_object_svc_handler : thread.current_handler_entry();
  const uint32_t target_object = service_gate ? m_object_svc_handler_object : thread.current_handler_object();
  const bool to_root = (target_object == m_object_svc_handler_object);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_phase(412);
#endif
  const uint32_t unwind_floor = thread.call_stack.depth
                                    ? thread.ledger_top().unwind_floor
                                    : 0;
#if defined(SHIZUKU_DEEP_DIAG)
  shizuku_push_diag_kind = 2; // svc_dispatch の通常 CALL 経路
#endif
  if (unwind_floor > UINT16_MAX ||
      !(arch_call_in_place<ARCH>()
            ? header_push_of<ARCH>(thread, *context, *frame, LEDGER_DISPATCH, unwind_floor)
            : call_frame_push(thread, context, &frame))) {
    // ★この拒否は object_api の呼出しへの答え。object 版 NO_STACK で返し、呼出し元が
    //   「どちらの push で断られたか」を区別しなくて済むようにする (KERNEL_ERROR_MARK は
    //   語彙を持たないエラー用に残す)。
    ARCH::set_result(*frame, (uintptr_t)object_error::NO_STACK, 0);
    return;
  }

#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_phase(413);
#endif

  auto &entry = thread.ledger[thread.call_stack.depth - 1];
  entry.object = target_object;
  entry.kind = to_root ? (uint8_t)object_kind::KERNEL_OBJECT
                       : (uint8_t)object_kind::HANDLER;
  entry.via = LEDGER_DISPATCH;
  // 親ハンドラ自身の親ハンドラは root kernel object。root 自身の親は自身。
  // ★制約 (2026-09-12): 専用ハンドラの親ハンドラは常に Root Kernel Object に設定されるため、
  //   現在の実装における専用ハンドラ階層は「Child → Dedicated Handler → Root」の 1 段限定である。
  //   無制限のハンドラ多段ネストはサポートしない (独立監査指摘)。
  entry.handler_object = m_object_svc_handler_object;
  entry.handler_entry = m_object_svc_handler;
  entry.unwind_floor = (uint16_t)unwind_floor;

  uintptr_t args[5];
  for (unsigned index = 0; index < 4; ++index)
    args[index] = ARCH::arg(*frame, index);
  // A return stub cannot know its parent's absolute depth. Its private marker
  // turns the legacy one-pop notification into a return to the actor's floor.
  if (args[0] == (uintptr_t)primitive::RETURN &&
      ARCH::arg(*frame, 4) == RETURN_STUB_MARKER)
    args[1] = unwind_floor;
  args[4] = unwind_floor;

  ARCH::set_entry(*frame, target_entry, ARCH::return_stub());
  ARCH::set_handler_args(*frame, args,
                         ARCH::psp_after_return(*context));
  ARCH::set_priv(*context, true);
  ARCH::set_region_window(*context, 0, 0);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_phase(414);
#endif
}

template <> SHIZUKU_HOT void KERNEL::svc_dispatch(KERNEL::CONTEXT *context) {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_phase(410);
#endif
  THREAD &thread = current_thread();
  FRAME *frame = context->sp;
  // Only this privileged internal gate bypasses parent routing. The public
  // objectland raw-SVC path below, including colliding numbers, is unchanged.
#if defined(__arm__) || defined(__thumb__)
  const bool service_gate = ARCH::frame_pc(*frame) ==
      reinterpret_cast<uintptr_t>(&shizuku_v2_service_resume);
#else
  constexpr bool service_gate = false;
#endif
  if (service_gate) {
    if (is_context_unprivileged(context)) {
      ARCH::set_result(*frame, (uintptr_t)object_error::NOT_PRIVILEGED, 0);
      return;
    }
    const auto id = ARCH::arg(*frame, 0);
    if (id != (uintptr_t)object_api::YIELD && id != (uintptr_t)object_api::RUN_FOR &&
        id != (uintptr_t)object_api::SLEEP_US) {
      ARCH::set_result(*frame, (uintptr_t)object_error::UNKNOWN_API, 0);
      return;
    }
  }

  // ★「kernel object 本人であるか」の厳格判定 (指摘3, 指摘4)。
  //   単なる HANDLER ではなく、登録済み kernel object (種別 KERNEL_OBJECT かつ
  //   ID が m_object_svc_handler_object) だけが kernel primitive を直接解釈できる。
  if (!service_gate && thread.current_kind() == (uint32_t)object_kind::KERNEL_OBJECT &&
      thread.current_object() == m_object_svc_handler_object) {
    svc_primitive(context);
    return;
  }

  // ここへ来るのは非 kernel object (一般 child や専用 handling object など)。
  // 親ハンドラが未登録なら黙って root へ fallback せず、診断可能な panic とする (指摘2)。
  if (!service_gate && thread.current_handler_entry() == 0) {
    BOARD::panic("unregistered parent handler for object syscall");
  }

  // 親ハンドラへの直接ディスパッチ (O(1)、テーブル検索ゼロ、同期例外中の再帰なし)。
  const uint32_t target_object = service_gate ? m_object_svc_handler_object : thread.current_handler_object();
  const bool to_root = (target_object == m_object_svc_handler_object);

  // 宛先が信頼された root KernelObject かつ mode == svc_mode::handler の同期・有界 native API
  // は trap 内で直接処理し、dispatch frame を積まずに即時復帰する。
  //
  // 重要 (Luna レビュー指摘 1):
  // 移行対象外の mode == svc_mode::thread は、trap 内で先行して属性・権限検査を行わず、
  // 既存の KOBJ actor dispatch 経路へそのまま通す (先行拒絶の禁止)。
  // また、専用親ハンドラ宛て (to_root == false) の raw SVC は全 ID で親ハンドラへ無加工配送される。
  if (to_root) {
    // 番号 → 密な種別の表引き (O(1))。その種別の switch は密な連番なのでジャンプ表になる。
    // RV32 の例外入口はこの振り分けを使わず、同じ番号の表から各 svc_* へ直接入る。
    switch (trap_api_of(ARCH::arg(*frame, 0))) {
    case trap_api::NONE:
      break;
    case trap_api::GET_CURRENT:
      svc_get_current(context);
      return;
    case trap_api::GET_CALLER:
      svc_get_caller(context);
      return;
    case trap_api::YIELD:
      if (svc_yield(context)) return;
      break;
    case trap_api::EXIT_METHOD:
      if (svc_exit_method(context)) return;
      break;
    case trap_api::FORWARD_CHILD_EXIT:
      if (svc_forward_child_exit(context)) return;
      break;
    case trap_api::CALL_METHOD:
      svc_call_method(context);
      return;
    case trap_api::RUN_FOR:
      svc_run_for(context);
      return;
    case trap_api::SET_BUDGET:
      svc_set_budget(context);
      return;
    case trap_api::CALL_DESCRIPTOR:
      svc_call_descriptor(context);
      return;
    }
  }

  svc_forward(context, service_gate);
}

// 最低優先度の遅延例外 = 実行権の強制巻き取り。ここへ来た時点で全ての割り込みは
// 捌けており、syscall の途中でもない (優先度規約が保証する)。
// ★発火が余分でも壊れないようにガードは自己安定型にしてある — 早期復帰と期限が
//   競っても「もう貸していない / まだ期限前」なら何もしないで戻るだけ。
template <> void KERNEL::pendsv_dispatch(KERNEL::CONTEXT *context) {
  (void)context; // 借り手の文脈退避は例外入口が済ませている
  const uint32_t core = BOARD::core_num();
  grant_stack &grants = m_grants[core];
  if (grants.depth == 0)
    return;
  grant_charge();
  if (grants.frames[grants.depth - 1].remaining != 0) {
    arm_timer();
    return;
  }
  // ★借り手がオブジェクトランドのハンドラまたはカーネルオブジェクトの中に居るなら、
  //   共有台帳等の同期保護のため取り上げを見送る (指摘3: execution role)。
  if (current_thread().current_kind() == (uint32_t)object_kind::HANDLER ||
      current_thread().current_kind() == (uint32_t)object_kind::KERNEL_OBJECT) {
    grants.frames[grants.depth - 1].remaining = GRANT_RETRY_CYCLES;
    arm_timer();
    return;
  }
  grant_unwind(grant_end::EXPIRED);
}

} // namespace shizuku

// 戻り口の巻き戻しが弾かれたときの落ち先。
//   a0 = kernel_error, a1 = 実際の深さ
// ★panic しない。段数の申告はオブジェクト側の責任なので、間違えた者だけが止まるべきで、
//   系全体を道連れにするのは方針として誤り (I-9)。記録を残してこのスレッドを隔離する。
// ★ここへ来る典型的な原因は「スタックが尽きて戻ることすらできない」なので、
//   **印字してはいけない** — printf は数百バイト使うので、その場で PSPLIM を
//   割って HardFault になり、一番情報の少ない壊れ方をする (実際に踏んだ)。
//   記録だけ残して止まり、報告は余裕のある側 (フォールトハンドラや他スレッド) に任せる。
extern "C" {
volatile uintptr_t shizuku_return_stub_failure_error;
volatile uintptr_t shizuku_return_stub_failure_depth;
volatile uint32_t shizuku_return_stub_failure_count;
}

extern "C" [[noreturn]] void shizuku_return_stub_failed(uintptr_t error,
                                                        uintptr_t depth) {
  shizuku_return_stub_failure_error = error;
  shizuku_return_stub_failure_depth = depth;
  shizuku_return_stub_failure_count = shizuku_return_stub_failure_count + 1;
  // TODO(Phase 2b): SWITCH を撃って他スレッドへ譲り、このスレッドだけを隔離する。
  while (true) {
  }
}
