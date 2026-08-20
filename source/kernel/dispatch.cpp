// ===========================================================================
//  svc の受け口とプリミティブ — カーネルの中核 (DESIGN §6.2 / §7 / §8 / §9)
// ===========================================================================
//  ここは**カーネルの svc ハンドラ** (例外文脈で走る機構)。カーネルオブジェクトが
//  持つ**オブジェクトランドの svc ハンドラ** (スレッドモードで走る方針側) とは
//  別概念で、ここでは後者を「登録された entry」としてしか扱わない。
//
//  ★経路は呼び出しフレームの段数のパリティだけで決まる (I-1):
//      偶数段 → オブジェクトが走っている → 登録済みハンドラをメソッドとして呼ぶ
//      奇数段 → ハンドラが走っている     → プリミティブを実行
//    積むのはトランポリン (オブジェクト→ハンドラ) と CALL (ハンドラ→オブジェクト)
//    の 2 つだけで必ず交互になるので、パリティがそのまま実行主体になる。
//    identity も信頼ビットも旗も持たない。
//
//  ★カーネルオブジェクト以外は RETURN を撃てないので、ハンドラを起こすときに
//    今のネスト数を渡す。オブジェクトは exit API に何段戻すかを載せて撃ち、
//    ハンドラがその段数で巻き戻す (D5)。
#include "shizuku/kernel.hpp"

namespace shizuku {

// ISA 層の例外入口が退避先・復帰先として使う文脈。dispatch が現在スレッドを
// 差し替えれば、そのまま切替になる (退避後と復帰前の 2 回呼ばれる)。
template <> KERNEL::CONTEXT *KERNEL::current_context() {
  return m_threads[m_current[BOARD::core_num()]].context;
}

template <> void KERNEL::set_object_handler(uintptr_t entry_pc) {
  m_object_svc_handler = entry_pc;
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
  const uintptr_t limit = ARCH::stack_limit(*context);
  if (limit != 0 && callee_frame < limit + ARCH::CALL_HEADROOM)
    return false;

  call_frame_header *header = (call_frame_header *)snapshot;
  header->prev = thread.call_stack.top;
  header->total_bytes = total;
  header->frame_bytes = frame_bytes;
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
  if (!call_frame_push(thread, context, frame))
    return kernel_error::NO_STACK;
  // 戻り口は常にカーネルの 1 本。撃つ svc は同じでも、そこから出たときの段数の
  // パリティで「プリミティブとしての巻き戻し」か「メソッドが戻った知らせ」かが
  // 決まる (発行側が戻り口を選ぶ必要は無い)。
  ARCH::set_entry(**frame, request.entry_pc, ARCH::return_stub());
  ARCH::set_args(**frame, request.args);
  ARCH::set_priv(*context, (request.protection & PROTECTION_UNPRIVILEGED) == 0);
  return kernel_error::OK;
}

template <> void KERNEL::svc_dispatch(KERNEL::CONTEXT *context) {
  THREAD &thread = current_thread();
  FRAME *frame = context->sp;
  const uintptr_t number = ARCH::arg(*frame, 0);

  if ((thread.call_stack.depth & 1u) == 0) {
    // 偶数段 = オブジェクトが走っている。その svc はオブジェクトランドのハンドラへの
    // **メソッド呼び出し**として届けるだけ。カーネルは番号を解釈しない (I-1) し、
    // 誰が担当かも知らない。
    if (m_object_svc_handler == 0)
      BOARD::panic("no object-land svc handler registered");
    call_request request{};
    request.entry_pc = m_object_svc_handler;
    request.protection = PROTECTION_PRIVILEGED;
    for (unsigned index = 0; index < 4; ++index)
      request.args[index] = ARCH::arg(*frame, index); // 元の引数をそのまま渡す
    const kernel_error error = do_call(thread, context, &frame, request);
    if (error != kernel_error::OK) {
      ARCH::set_result(*frame, (uintptr_t)error, 0);
      return;
    }
    // ★ハンドラへ「今のネスト数」を渡す。オブジェクトは RETURN を撃てないので、
    //   何段巻き戻すかを決めるのはハンドラであり、その検算材料がこの値になる。
    ARCH::set_handler_info(*context, number, thread.call_stack.depth);
    return;
  }

  // ここから先は奇数段 = ハンドラだけ。プリミティブを撃てるのがカーネルオブジェクトの
  // ハンドラに限られること (I-2) は、この分岐自体が保証している。
  switch ((primitive)number) {
  case primitive::CALL: {
    const call_request *pointer = (const call_request *)ARCH::arg(*frame, 1);
    if (pointer == nullptr) {
      ARCH::set_result(*frame, (uintptr_t)kernel_error::BAD_REQUEST, 0);
      break;
    }
    // フレームを書き換える前に内容を控える。
    const call_request request = *pointer;
    // 呼び先は 1 段深くなる = 偶数段 = オブジェクトとして走る。ハンドラから
    // 呼ばれただけの普通のメソッドがプリミティブを撃てるようにはならない (I-8)。
    const kernel_error error = do_call(thread, context, &frame, request);
    // 成功時はこの syscall から戻らない (呼び先が戻ったときに、復元された
    // 呼び出し元フレームへ戻り値が載る)。失敗時だけその場でエラー復帰する。
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
    // ★段数の検算 (§9.3)。落とす枚数は発行側の自由だが、発行側が想定している
    //   呼び出し文脈と実際がズレていたら 1 枚も落としてはいけない — ズレたまま
    //   落とすと無関係な祖先が偽の戻り値で再開する (無音ロックアップの正体)。
    if (depth == 0) {
      ARCH::set_result(*frame, (uintptr_t)kernel_error::BAD_COUNT, 0);
      break;
    }
    if (claim != 0 && claim != depth) {
      // 実際の深さを返して発行側が自己診断できるようにする (両側チェック)。
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
    // 切り替わった場合、この結果を見るのは**次にこのスレッドが再開したとき**。
    ARCH::set_result(*frame, (uintptr_t)error, 0);
    break;
  }
  case primitive::GRANT: {
    const uint32_t target = (uint32_t)ARCH::arg(*frame, 1);
    const uint32_t microseconds = (uint32_t)ARCH::arg(*frame, 2);
    // 貸し手の戻り値は先に OK を置いておく (借り手が返してきたとき a1 に理由が入る)。
    ARCH::set_result(*frame, (uintptr_t)kernel_error::OK,
                     (uintptr_t)grant_end::YIELDED);
    const kernel_error error = do_grant(target, microseconds);
    if (error != kernel_error::OK)
      ARCH::set_result(*frame, (uintptr_t)error, 0);
    break;
  }
  default:
    // ★ここへ来るのはカーネルオブジェクトのハンドラだけ。存在しないプリミティブを
    //   撃つのは**カーネル自身の不変条件の破れ**なので panic してよい (§14)。
    //   エラーで返す相手が居ないので kernel_error にこの語彙は無い。ただし
    //   **黙って捨てるのは禁止** — 無音の握り潰しは「効いていないのに動いて見える」
    //   計測事故を生む (DESIGN §11.2.0 で実際に起きた)。
    BOARD::panic("unknown kernel primitive");
    break;
  }
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
  if (grants.frames[grants.depth - 1].deadline > BOARD::time_us()) {
    arm_timer(grants.frames[grants.depth - 1].deadline);
    return;
  }
  grant_unwind(grant_end::EXPIRED);
}

} // namespace shizuku

// 戻り口の巻き戻しが弾かれたときの落ち先。
//   a0 = kernel_error, a1 = 実際の深さ
// ★panic しない。段数の申告はオブジェクト側の責任なので、間違えた者だけが止まるべきで、
//   系全体を道連れにするのは方針として誤り (I-9)。記録を残してこのスレッドを隔離する。
extern "C" [[noreturn]] void shizuku_return_stub_failed(uintptr_t error,
                                                        uintptr_t depth) {
  shizuku::KERNEL::BOARD::diag_printf(
      "[KERNEL] return rejected: error=%lu depth=%lu (thread parked)\n",
      (unsigned long)error, (unsigned long)depth);
  // TODO(Phase 2b): SWITCH を撃って他スレッドへ譲り、このスレッドだけを隔離する。
  // 現状はスレッドが 1 本しかないのでスピンするしかない。
  while (true) {
  }
}
