// ===========================================================================
//  スレッドと実行権 — 生成 / バトンパス / 時限つきの貸し出し (DESIGN §10)
// ===========================================================================
//  ★資源の貸し借りは 1 つの形しか使わない (DESIGN §10.1):
//      持つ者が貸す / 外側の値でクランプ / 積んで戻りで pop /
//      超過は超過した者にだけ当たる (系は死なない)
//    ここでは CPU 時間にそれを当てる。同じ形をメモリ・I/O にも後で当てる。
//  ★期限は quantum ではない。正常時は期限より十分早く返すので発火せず、その限り
//    「返さない限り処理は原子」という協調の性質は保たれる (§10.2)。期限は
//    「1 つの暴走が全系を凍らせない」ための安全網。
#include "shizuku/kernel.hpp"

namespace shizuku {

// 実装より前に使う (do_switch が grant_unwind を、grant_unwind が arm_timer を呼ぶ)
// ので、特殊化の宣言を先に置く。無いと暗黙の実体化が先に起きる。
template <> void KERNEL::grant_unwind(grant_end);
template <> void KERNEL::arm_timer(uint64_t);
template <> bool KERNEL::claim(uint32_t, kernel_error &);
template <> void KERNEL::set_recovery_thread(uint32_t);
template <> void KERNEL::fault_dispatch(KERNEL::CONTEXT *);

// ---- スレッドの生成 (スレッドモードから呼ぶ C++ API。syscall ではない) ------
template <>
KERNEL::spawn_result KERNEL::spawn(uintptr_t entry_pc, uintptr_t argument,
                                   uint32_t protection, uint32_t affinity) {
  // ★「空いているか見てから作る」は 2 コアで TOCTOU になる。CAS で枠を予約してから
  //   中身を書く (参照実装はここで二重初期化検査に引っかかり panic していた = I-9 違反)。
  uint32_t index = THREAD_COUNT;
  for (uint32_t candidate = 1; candidate < THREAD_COUNT; ++candidate) {
    if (ARCH::cas32(&m_threads[candidate].state,
                    (uint32_t)THREAD::state_t::UNINITIALIZED,
                    (uint32_t)THREAD::state_t::RESERVED)) {
      index = candidate;
      break;
    }
  }
  if (index == THREAD_COUNT)
    return {kernel_error::NO_THREAD, 0};

  THREAD &thread = m_threads[index];
  auto allocation = memory_manager.kernel_malloc(THREAD_STACK_BYTES);
  if (!allocation) {
    ARCH::store_release32(&thread.state,
                          (uint32_t)THREAD::state_t::UNINITIALIZED);
    return {kernel_error::NO_MEMORY, 0};
  }
  const uintptr_t base = (uintptr_t)allocation.value();
  thread.context = &m_contexts[index];
  *thread.context = CONTEXT{};
  thread.call_stack = {};
  thread.affinity = affinity == 0 ? 0b1 : affinity;
  ARCH::stack_limit_set(*thread.context, (base + 7) & ~(uintptr_t)7);
  ARCH::set_priv(*thread.context, (protection & PROTECTION_UNPRIVILEGED) == 0);
  // 最初の 1 回は「例外から復帰してきた」ように見せかける必要があるので、
  // 例外フレームを自分で組み立てる。戻り口は普通の呼び出しと同じ 1 本
  // (スレッドの入口が return したときは戻り先が無いので、受け取ったカーネル
  //  オブジェクトが終了させる)。
  ARCH::prepare_thread_entry(*thread.context, base + THREAD_STACK_BYTES,
                             entry_pc, ARCH::return_stub(), argument);
  // ★READY は最後に release で公開する。ここまでの初期化が全部見えてから他コアが
  //   claim できるようにするため (先に公開すると途中初期化のまま走り出せる)。
  ARCH::store_release32(&thread.state, (uint32_t)THREAD::state_t::READY);
  return {kernel_error::OK, index};
}

template <> void KERNEL::terminate(uint32_t thread) {
  if (thread >= THREAD_COUNT)
    return;
  ARCH::store_release32(&m_threads[thread].state,
                        (uint32_t)THREAD::state_t::TERMINATED);
}

// ---- 実行権の受け渡し -------------------------------------------------------
// READY → RUNNING を CAS で取る。これが「2 コアが同じ文脈を走らせない」根。
template <> bool KERNEL::claim(uint32_t thread, kernel_error &error) {
  if (thread >= THREAD_COUNT) {
    error = kernel_error::NOT_READY;
    return false;
  }
  if ((m_threads[thread].affinity & (1u << BOARD::core_num())) == 0) {
    error = kernel_error::BAD_AFFINITY;
    return false;
  }
  if (!ARCH::cas32(&m_threads[thread].state, (uint32_t)THREAD::state_t::READY,
                   (uint32_t)THREAD::state_t::RUNNING)) {
    error = kernel_error::NOT_READY;
    return false;
  }
  return true;
}

template <> kernel_error KERNEL::do_switch(uint32_t target) {
  const uint32_t core = BOARD::core_num();
  // ★借り手として走っている最中の switch は「早めに返す」の意味にする。対象は見ない
  //   — 借りた実行権を第三者へ又貸しできてしまうと、貸し手の期限が意味を失う。
  if (m_grants[core].depth != 0) {
    grant_unwind(grant_end::YIELDED);
    return kernel_error::OK;
  }
  const uint32_t current = m_current[core];
  if (target == current)
    return kernel_error::OK; // 自分へ渡すのは何もしないのと同じ
  kernel_error error = kernel_error::OK;
  if (!claim(target, error))
    return error;
  m_current[core] = target;
  // 渡した側は READY へ。release 以降、他コアが拾ってよい (自分の文脈の退避は
  // 例外入口が済ませている)。
  ARCH::store_release32(&m_threads[current].state,
                        (uint32_t)THREAD::state_t::READY);
  return kernel_error::OK;
}

template <>
kernel_error KERNEL::do_grant(uint32_t target, uint32_t microseconds) {
  const uint32_t core = BOARD::core_num();
  const uint32_t current = m_current[core];
  if (target == current)
    return kernel_error::OK; // 自分へ貸すのは何もしないのと同じ
  grant_stack &grants = m_grants[core];
  if (grants.depth >= grant_stack::MAX_DEPTH)
    return kernel_error::GRANT_DEPTH;
  kernel_error error = kernel_error::OK;
  if (!claim(target, error))
    return error;
  // ★期限は外側の期限でクランプする (I-7)。借りたものを又貸しで延長できない。
  uint64_t deadline = BOARD::time_us() + (uint64_t)microseconds;
  if (grants.depth != 0 && grants.frames[grants.depth - 1].deadline < deadline)
    deadline = grants.frames[grants.depth - 1].deadline;
  grants.frames[grants.depth++] = {current, deadline};
  // 貸し手は WAIT_GRANT。READY ではないので他コアに拾われず、復帰はこのコアの
  // 巻き取り経路 (期限 or 早期復帰) だけになる。
  m_threads[current].set_state(THREAD::state_t::WAIT_GRANT);
  m_current[core] = target;
  arm_timer(deadline);
  return kernel_error::OK;
}

// 貸した実行権を 1 段巻き取る。期限切れ (EXPIRED) と早期復帰 (YIELDED) の共通経路。
template <> void KERNEL::grant_unwind(grant_end reason) {
  const uint32_t core = BOARD::core_num();
  grant_stack &grants = m_grants[core];
  if (grants.depth == 0)
    return;
  const uint32_t borrower = m_current[core];
  const uint32_t lender = grants.frames[grants.depth - 1].lender;
  grants.depth--;
  // 貸し手が待っていた syscall の戻り値を書く (a0 は貸した時点で OK 済み)。
  ARCH::set_result(*m_threads[lender].context->sp, (uintptr_t)kernel_error::OK,
                   (uintptr_t)reason);
  m_threads[lender].set_state(THREAD::state_t::RUNNING);
  m_current[core] = lender;
  // 借り手を返す。走り終えていたらそのまま (終了させたスレッドを生き返らせない)。
  if (m_threads[borrower].is_state(THREAD::state_t::RUNNING))
    ARCH::store_release32(&m_threads[borrower].state,
                          (uint32_t)THREAD::state_t::READY);
  // 次の期限へ張り替える (空なら止める。既に過ぎていれば即もう一段巻き取らせる)。
  if (grants.depth == 0)
    ARCH::timer_cancel();
  else if (grants.frames[grants.depth - 1].deadline <= BOARD::time_us())
    ARCH::pend_context_switch();
  else
    arm_timer(grants.frames[grants.depth - 1].deadline);
}

template <> void KERNEL::arm_timer(uint64_t deadline_us) {
  const uint64_t now = BOARD::time_us();
  uint64_t remaining = deadline_us > now ? deadline_us - now : 1;
  uint64_t cycles = remaining * (uint64_t)BOARD::cycles_per_us();
  // タイマは幅が有限なので、遠い期限は刻んで継ぎ足す (期限自体は変わらない)。
  if (cycles > ARCH::TIMER_MAX_CYCLES)
    cycles = ARCH::TIMER_MAX_CYCLES;
  if (cycles < ARCH::TIMER_MIN_CYCLES)
    cycles = ARCH::TIMER_MIN_CYCLES;
  ARCH::timer_oneshot((uint32_t)cycles);
}

// タイマ例外 (優先度は syscall より下、切替より上)。ここでは**切替を起票するだけ**で、
// 実際の切替は最低優先度の遅延例外で起こす — これが 1 コア内の相互排除を無償で
// 与える規約 (DESIGN §14.5.1)。
template <> void KERNEL::timer_expired() {
  const uint32_t core = BOARD::core_num();
  grant_stack &grants = m_grants[core];
  if (grants.depth == 0) {
    ARCH::timer_cancel(); // 早期復帰と競合した後の遅れて来た発火
    return;
  }
  const uint64_t deadline = grants.frames[grants.depth - 1].deadline;
  if (BOARD::time_us() >= deadline) {
    ARCH::timer_cancel();
    ARCH::pend_context_switch();
  } else {
    arm_timer(deadline); // 刻みの継ぎ足し
  }
}

template <> void KERNEL::set_recovery_thread(uint32_t thread) {
  m_recovery_thread = thread;
}

// 保護違反やスタック上限違反の受け口。
// ★系を止めない。落ちたのは 1 本のスレッドなので、そのスレッドだけを止めて他は
//   走り続けさせる (I-9 / DESIGN §11.2.4「違反しても系は止まらない」)。
//   ここを「全部止める」にすると、1 つのオブジェクトの誤りが全系を道連れにする —
//   それは資源管理で「超過した者にだけ当たる」と決めたことと矛盾する。
template <> void KERNEL::fault_dispatch(KERNEL::CONTEXT *context) {
  const uint32_t core = BOARD::core_num();
  const uint32_t thread = m_current[core];
  const uint32_t status = ARCH::fault_status();
  // ★触ろうとした先は**消す前に**読む。違反の記録を消すと同時に無効になるので、
  //   順序を逆にすると意味のない値を報告してしまう (実際それで嘘の値が出た)。
  const uintptr_t address = ARCH::fault_address();
  const uintptr_t pc = ARCH::frame_pc(*context->sp);
  m_faults.count++;
  m_faults.pc = pc;
  m_faults.status = status;
  m_faults.thread = thread;
  ARCH::fault_status_clear();

  // ★止めても直らないのは「カーネル自身が落ちた」場合だけ。スレッドモードで
  //   落ちたのなら、そのスレッドを止めれば系は続けられる。
  if (!ARCH::faulted_in_thread_mode(*context)) {
    BOARD::diag_printf("[FAULT] kernel itself faulted: pc=%08lx cfsr=%08lx\n",
                       (unsigned long)pc, (unsigned long)status);
    BOARD::panic("fault in kernel context");
  }

  BOARD::diag_printf("[FAULT] thread %lu stopped: pc=%08lx addr=%08lx "
                     "cfsr=%08lx sp=%08lx (系は継続)\n",
                     (unsigned long)thread, (unsigned long)pc,
                     (unsigned long)address,
                     (unsigned long)status, (unsigned long)(uintptr_t)context->sp);

  ARCH::store_release32(&m_threads[thread].state,
                        (uint32_t)THREAD::state_t::TERMINATED);

  // 借り手として走っていたなら、貸し手へ返すのが自然な復帰先 (貸した側は
  // 「期限が来た」のと同じ形で戻ってくる)。
  if (m_grants[core].depth != 0) {
    grant_unwind(grant_end::EXPIRED);
    return;
  }
  // そうでなければ、あらかじめ教えられている復帰先へ渡す。誰に渡すかは方針なので
  // カーネルは選ばない — 教えられていないなら渡す先が無い。
  kernel_error error = kernel_error::OK;
  if (m_recovery_thread < THREAD_COUNT && claim(m_recovery_thread, error)) {
    m_current[core] = m_recovery_thread;
    return;
  }
  BOARD::diag_printf("[FAULT] 渡す先が無い (recovery=%lu)\n",
                     (unsigned long)m_recovery_thread);
  BOARD::panic("no thread to run after fault");
}

} // namespace shizuku
