// ===========================================================================
//  svc の受け口とプリミティブ — カーネルの中核 (DESIGN §6.2 / §7 / §8 / §9)
// ===========================================================================
//  ここは**カーネルの svc ハンドラ** (例外文脈で走る機構)。カーネルオブジェクトが
//  持つ**オブジェクトランドの svc ハンドラ** (スレッドモードで走る方針側) とは
//  別概念で、ここでは後者を「登録された entry」としてしか扱わない。
//
//  ★経路は**今走っているオブジェクトの種別** (object_kind) だけで決まる (I-1):
//      PLAIN   → オブジェクトが走っている → 登録済みハンドラをメソッドとして呼ぶ
//      HANDLER → ハンドラが走っている     → プリミティブを実行
//    種別は thread.current_kind にあり、遷移させるのはカーネルだけ:
//    CALL では呼び先の申告 (call_request::callee_kind) を載せ、トランポリンでは
//    HANDLER を載せ、戻るときは呼び出しフレームのヘッダから読み戻す。
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
//  ★HANDLER 種別以外は RETURN を撃てないので、ハンドラを起こすときに
//    今のネスト数を渡す。オブジェクトは exit API に何段戻すかを載せて撃ち、
//    ハンドラがその段数で巻き戻す (D5)。
#include "shizuku/kernel.hpp"

namespace shizuku {

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


template <>
bool KERNEL::call_frame_push(KERNEL::THREAD &thread, KERNEL::CONTEXT *context,
                             KERNEL::FRAME **frame) {
  const uint32_t frame_bytes = ARCH::exc_frame_bytes(*context);
  uint32_t total = (uint32_t)sizeof(call_frame_header) + frame_bytes;
  total = (total + 7u) & ~7u; // 8B 境界を保つ
  // 呼び出し元の生スタック境界。元の例外フレームはこのすぐ下に居る。
  const uintptr_t caller_stack = (uintptr_t)context->sp + frame_bytes;
  const uintptr_t snapshot = caller_stack - total; // 退避域の先頭 (ヘッダの位置)
  const uintptr_t callee_frame = snapshot - frame_bytes; // 書き換え用フレーム

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
  const uintptr_t reserve =
      (uintptr_t)sizeof(call_frame_header) + frame_bytes + ARCH::CALL_HEADROOM;
  const uintptr_t limit = ARCH::stack_limit(*context);
  if (limit != 0 && callee_frame < limit + reserve)
    return false;

  call_frame_header *header = (call_frame_header *)snapshot;
  header->prev = thread.call_stack.top;
  header->total_bytes = total;
  header->frame_bytes = frame_bytes;
  header->caller_object = thread.current_object; // ★呼び出し元オブジェクトを退避
  header->caller_kind = thread.current_kind;     // ★その種別も一緒に退避
  header->caller_handler_object = thread.current_handler_object; // ★親ハンドラ情報も退避
  header->caller_handler_entry = thread.current_handler_entry;
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
  thread.call_stack.top = snapshot;
  thread.call_stack.depth++;
  return true;
}

template <>
bool KERNEL::call_frame_pop(KERNEL::THREAD &thread, KERNEL::CONTEXT *context,
                            KERNEL::FRAME **frame) {
  if (thread.call_stack.top == 0)
    return false;
  const call_frame_header *header =
      (const call_frame_header *)thread.call_stack.top;
  // ★幾何は push 時に記録した値を読み戻す。再計算しない (I-5)。
  // 元の例外フレームは退避域の中に元の位置のまま生きているので、文脈を丸ごと
  // 戻すだけで復帰先が正しく決まる (書き戻しも再配置も不要)。
  const uintptr_t previous = header->prev;
  thread.current_object = header->caller_object; // ★呼び出し元オブジェクトを復元
  thread.current_kind = header->caller_kind;     // ★種別も一緒に復元
  thread.current_handler_object = header->caller_handler_object; // ★親ハンドラも復元
  thread.current_handler_entry = header->caller_handler_entry;
  *context = header->saved;
  *frame = context->sp;
  thread.call_stack.top = previous;
  thread.call_stack.depth--;
  return true;
}

template <>
kernel_error KERNEL::do_call(KERNEL::THREAD &thread, KERNEL::CONTEXT *context,
                             KERNEL::FRAME **frame,
                             const call_request &request) {
  if (request.entry_pc == 0)
    return kernel_error::BAD_REQUEST;
  // ★偽装防止 (指摘4): 一般 call_request から KERNEL_OBJECT 種別や
  //   m_object_svc_handler_object を名乗ることは拒否する。
  if (request.callee_kind == (uint32_t)object_kind::KERNEL_OBJECT ||
      request.callee_object == m_object_svc_handler_object) {
    return kernel_error::BAD_REQUEST;
  }
  if (!call_frame_push(thread, context, frame))
    return kernel_error::NO_STACK;
  thread.current_object = request.callee_object; // ★呼び先オブジェクトIDへ遷移
  thread.current_kind = request.callee_kind;     // ★種別もここで切り替わる
  thread.current_handler_object = request.parent_handler_object;
  thread.current_handler_entry = request.parent_handler_entry;
  // 戻り口は常にカーネルの 1 本。撃つ svc は同じでも、そこから出たときの**種別**で
  // 「プリミティブとしての巻き戻し」か「メソッドが戻った知らせ」かが決まる
  // (発行側が戻り口を選ぶ必要は無い)。
  ARCH::set_entry(**frame, request.entry_pc, ARCH::return_stub());
  ARCH::set_args(**frame, request.args);
  ARCH::set_priv(*context, (request.protection & PROTECTION_UNPRIVILEGED) == 0);
  ARCH::set_region_window(*context, request.region_base, request.region_limit);
  return kernel_error::OK;
}

template <> void KERNEL::svc_dispatch(KERNEL::CONTEXT *context) {
  THREAD &thread = current_thread();
  FRAME *frame = context->sp;

  // ★「kernel object 本人であるか」の厳格判定 (指摘3, 指摘4)。
  //   単なる HANDLER ではなく、登録済み kernel object (種別 KERNEL_OBJECT かつ
  //   ID が m_object_svc_handler_object) だけが kernel primitive を直接解釈できる。
  if (thread.current_kind == (uint32_t)object_kind::KERNEL_OBJECT &&
      thread.current_object == m_object_svc_handler_object) {
    const uintptr_t number = ARCH::arg(*frame, 0);
    switch ((primitive)number) {
    case primitive::CALL: {
      const call_request *pointer = (const call_request *)ARCH::arg(*frame, 1);
      if (pointer == nullptr) {
        ARCH::set_result(*frame, (uintptr_t)kernel_error::BAD_REQUEST, 0);
        break;
      }
      const call_request request = *pointer;
      const kernel_error error = do_call(thread, context, &frame, request);
      if (error != kernel_error::OK)
        ARCH::set_result(*frame, (uintptr_t)error, 0);
      break;
    }
    case primitive::RETURN: {
      const uintptr_t count = ARCH::arg(*frame, 1);
      const uintptr_t value = ARCH::arg(*frame, 2);
      const uintptr_t error = ARCH::arg(*frame, 3);
      const uintptr_t claim = ARCH::arg(*frame, 4);
      const uint32_t depth = thread.call_stack.depth;
      if (depth == 0) {
        ARCH::set_result(*frame, (uintptr_t)kernel_error::BAD_COUNT, 0);
        break;
      }
      if (claim != 0 && claim != depth) {
        ARCH::set_result(*frame, (uintptr_t)kernel_error::DEPTH_MISMATCH, depth);
        break;
      }
      if (count == 0 || count > depth) {
        ARCH::set_result(*frame, (uintptr_t)kernel_error::BAD_COUNT, depth);
        break;
      }
      for (uintptr_t index = 0; index < count; ++index)
        call_frame_pop(thread, context, &frame);
      ARCH::set_result(*frame, error, value);
      break;
    }
    case primitive::SWITCH: {
      const kernel_error error = do_switch((uint32_t)ARCH::arg(*frame, 1));
      ARCH::set_result(*frame, (uintptr_t)error, 0);
      break;
    }
    case primitive::GRANT: {
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
    return;
  }

  // ここへ来るのは非 kernel object (一般 child や専用 handling object など)。
  // 親ハンドラが未登録なら黙って root へ fallback せず、診断可能な panic とする (指摘2)。
  if (thread.current_handler_entry == 0) {
    BOARD::panic("unregistered parent handler for object syscall");
  }

  // 親ハンドラへの直接ディスパッチ (O(1)、テーブル検索ゼロ、同期例外中の再帰なし)。
  const uintptr_t target_entry = thread.current_handler_entry;
  const uint32_t target_object = thread.current_handler_object;
  const bool to_root = (target_object == m_object_svc_handler_object);

  if (!call_frame_push(thread, context, &frame)) {
    ARCH::set_result(*frame, KERNEL_ERROR_MARK | (uintptr_t)kernel_error::NO_STACK, 0);
    return;
  }

  thread.current_object = target_object;
  thread.current_kind = to_root ? (uint32_t)object_kind::KERNEL_OBJECT
                                : (uint32_t)object_kind::HANDLER;
  // 親ハンドラ自身の親ハンドラは root kernel object。root 自身の親は自身。
  thread.current_handler_object = m_object_svc_handler_object;
  thread.current_handler_entry = m_object_svc_handler;

  uintptr_t args[4];
  for (unsigned index = 0; index < 4; ++index)
    args[index] = ARCH::arg(*frame, index);

  ARCH::set_entry(*frame, target_entry, ARCH::return_stub());
  ARCH::set_args(*frame, args);
  ARCH::set_priv(*context, true);
  ARCH::set_region_window(*context, 0, 0);
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
  if (current_thread().current_kind == (uint32_t)object_kind::HANDLER ||
      current_thread().current_kind == (uint32_t)object_kind::KERNEL_OBJECT) {
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
