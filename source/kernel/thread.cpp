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
#if __has_include("shizuku/config.hpp")
#include "shizuku/kernel.hpp"
#endif
#include "shizuku/hot.hpp"
#include "shizuku/cost_probe.hpp"

#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
extern "C" void shizuku_selftest_fault_phase(uint32_t, uint32_t);
#endif
namespace shizuku {

namespace {
#if !defined(SHIZUKU_LEAN_LEDGER) && !defined(__riscv)
// ARM (RP2350 等) で HAS_OBJECT_PROTECTION かつ LEAN_LEDGER 未定義時のフォールバック用レコード。
// C6 (非 U-mode / HAS_OBJECT_PROTECTION == false) では .bss を一切消費しないよう除外する。
// スレッド数は configs/kit.bzl の SHIZUKU_THREAD_COUNT (32) に整合させる。
static constexpr size_t kMaxReturnThreads = 32;
static constexpr size_t kMaxReturnDepth = 34;
alignas(8) caller_return_record s_thread_returns[kMaxReturnThreads][kMaxReturnDepth] = {};
#endif
}

// 実装より前に使う (do_switch が grant_unwind を、grant_unwind が arm_timer を呼ぶ)
// ので、特殊化の宣言を先に置く。無いと暗黙の実体化が先に起きる。
template <> void KERNEL::grant_unwind(grant_end);
template <> void KERNEL::arm_timer();
template <> void KERNEL::grant_charge();
template <> bool KERNEL::claim(uint32_t, kernel_error &);
template <> kernel_error KERNEL::do_grant(uint32_t, uint64_t);
template <> void KERNEL::set_recovery_thread(uint32_t);
template <> void KERNEL::set_thread_storage(void *, uintptr_t);
template <> void KERNEL::release(uint32_t);
template <> bool KERNEL::terminate_if_idle(uint32_t);
template <> KERNEL::spawn_result KERNEL::reserve_thread();
template <> void KERNEL::fault_dispatch(KERNEL::CONTEXT *);
template <> void KERNEL::debug_dispatch(KERNEL::CONTEXT *);
template <> void KERNEL::suspend(uint32_t);
template <> void KERNEL::resume(uint32_t);
template <> void KERNEL::arm_step(uint32_t);
template <> void KERNEL::consume_pending_step_slow();

// ---- 貸してもらった記憶をスレッド表として据える ----------------------------
// ★所有はオブジェクトランド、保護はここで検査する。カーネルの簿記が非特権から
//   届く場所にあると、非特権オブジェクトが他スレッドの文脈を書き換えられてしまう —
//   「気をつける」では守れないので、組み立ての時点で弾く。
template <> void KERNEL::set_thread_storage(void *memory, uintptr_t bytes) {
  const uintptr_t base = (uintptr_t)memory;
  if (base >= BOARD::unprivileged_floor())
    BOARD::panic("thread storage must be out of reach of unprivileged objects");
  m_threads = (thread_record *)base;
  m_thread_count = (uint32_t)(bytes / sizeof(thread_record));
  if (m_thread_count == 0)
    BOARD::panic("thread storage too small");
  for (uint32_t index = 0; index < m_thread_count; ++index) {
    m_threads[index].thread = THREAD{};
    m_threads[index].context = CONTEXT{};
    m_threads[index].thread.context = &m_threads[index].context;
#if !defined(SHIZUKU_LEAN_LEDGER) && !defined(__riscv)
    if constexpr (requires { ARCH::HAS_OBJECT_PROTECTION; } && ARCH::HAS_OBJECT_PROTECTION) {
      if (index < kMaxReturnThreads) {
        m_threads[index].thread.return_records = s_thread_returns[index];
      }
    }
#endif
  }
  // 現在のスレッドのキャッシュを新しい表に張り直す (m_current は init で 0)。
  for (uint32_t core = 0; core < CORE_COUNT; ++core)
    m_current_thread[core] = m_current[core] < m_thread_count ? &m_threads[m_current[core]].thread
                                                                : &m_threads[0].thread;
}

template <> void KERNEL::set_thread_ledger(
    uint32_t id, KERNEL::call_ledger_entry *memory, uint32_t capacity,
    uint32_t object, uint32_t kind, uint32_t handler_object,
    uintptr_t handler_entry, uint32_t object_generation) {
  if (id >= m_thread_count || memory == nullptr || capacity == 0 ||
      (uintptr_t)memory >= BOARD::unprivileged_floor())
    BOARD::panic("call ledger must be privileged memory");
  THREAD &thread = m_threads[id].thread;
#if !defined(SHIZUKU_LEAN_LEDGER) && !defined(__riscv)
  if constexpr (requires { ARCH::HAS_OBJECT_PROTECTION; } && ARCH::HAS_OBJECT_PROTECTION) {
    if (id < kMaxReturnThreads) {
      thread.return_records = s_thread_returns[id];
    }
  }
#endif
  thread.ledger = memory;
  thread.ledger_capacity = capacity;
  thread.call_stack = {};
  thread.base_object = object;
  thread.call_stack.base_object_generation = object_generation;
  thread.base_kind = kind;
  thread.base_handler_object = handler_object;
  thread.base_handler_entry = handler_entry;
}

// ---- スレッドの生成 (スレッドモードから呼ぶ C++ API。syscall ではない) ------
template <>
KERNEL::spawn_result KERNEL::spawn(const KERNEL::spawn_request &request) {
  if (request.stack_base == 0 || request.stack_bytes < 256)
    return {kernel_error::NO_MEMORY, 0};
  // 窓で表せない stack の非特権スレッドは作らない (黙って窓なし = stack に触れない状態にしない)。
  if constexpr (ARCH::HAS_OBJECT_PROTECTION) {
    if ((request.protection & PROTECTION_UNPRIVILEGED) != 0 &&
        ARCH::encode_stack_window(request.stack_base, request.stack_base + request.stack_bytes).rlar == 0)
      return {kernel_error::BAD_REQUEST, 0};
  }
#if defined(SHIZUKU_RP2040)
  if (!ARCH::region_range_representable(request.region_base, request.region_limit))
    return {kernel_error::BAD_REQUEST, 0};
#endif
  // ★「空いているか見てから作る」は 2 コアで TOCTOU になる。CAS で枠を予約してから
  //   中身を書く (参照実装はここで二重初期化検査に引っかかり panic していた = I-9 違反)。
  uint32_t index = m_thread_count;
  for (uint32_t candidate = 1; candidate < m_thread_count; ++candidate) {
    if (ARCH::cas32(&m_threads[candidate].thread.state,
                    (uint32_t)THREAD::state_t::UNINITIALIZED,
                    (uint32_t)THREAD::state_t::RESERVED)) {
      index = candidate;
      break;
    }
  }
  if (index == m_thread_count)
    return {kernel_error::NO_THREAD, 0};

  THREAD &thread = m_threads[index].thread;
  thread.context = &m_threads[index].context;
  *thread.context = CONTEXT{};
  thread.call_stack = {};
  if (request.ledger == nullptr || request.ledger_capacity == 0)
    BOARD::panic("spawn without call ledger");
  set_thread_ledger(index, request.ledger,
                    request.ledger_capacity, request.object_id, request.kind,
                    request.parent_handler_object, request.parent_handler_entry,
                    request.object_generation);
  // ★既定は**全コア**。core0 固定を既定にすると「渡す機構が効いている」ことを
  //   確かめられない (決めた通りに動いただけになる)。固定したい相手は明示する。
  thread.affinity = request.affinity == 0
                        ? (uint32_t)((1u << CORE_COUNT) - 1u)
                        : request.affinity;
  ARCH::stack_limit_set(*thread.context,
                        (request.stack_base + 7) & ~(uintptr_t)7);
  if constexpr (ARCH::HAS_OBJECT_PROTECTION) {
    const auto stack_win = ARCH::encode_stack_window(request.stack_base, request.stack_base + request.stack_bytes);
    thread.stack_rbar = stack_win.rbar;
    thread.stack_rlar = stack_win.rlar;
  }
  ARCH::set_priv(*thread.context,
                 (request.protection & PROTECTION_UNPRIVILEGED) == 0);
  ARCH::set_region_window(*thread.context, request.region_base,
                          request.region_limit);
  // 最初の 1 回は「例外から復帰してきた」ように見せかける必要があるので、
  // 例外フレームを自分で組み立てる。戻り口は普通の呼び出しと同じ 1 本
  // (スレッドの入口が return したときは戻り先が無いので、受け取ったカーネル
  //  オブジェクトが終了させる)。
  ARCH::prepare_thread_entry(*thread.context,
                             request.stack_base + request.stack_bytes,
                             request.entry_pc, ARCH::return_stub(),
                             request.argument);
  // ★READY は最後に release で公開する。ここまでの初期化が全部見えてから他コアが
  //   claim できるようにするため (先に公開すると途中初期化のまま走り出せる)。
  ARCH::store_release32(&thread.state, (uint32_t)THREAD::state_t::READY);
  return {kernel_error::OK, index};
}

// 走らせずに枠だけ取る。★spawn と同じ CAS で取るので、他コアと競っても二重取りに
//   ならない。採用する側が RUNNING にするまで RESERVED のまま = 誰にも拾われない。
template <> KERNEL::spawn_result KERNEL::reserve_thread() {
  for (uint32_t candidate = 1; candidate < m_thread_count; ++candidate) {
    if (ARCH::cas32(&m_threads[candidate].thread.state,
                    (uint32_t)THREAD::state_t::UNINITIALIZED,
                    (uint32_t)THREAD::state_t::RESERVED)) {
      m_threads[candidate].thread.context = &m_threads[candidate].context;
      return {kernel_error::OK, candidate};
    }
  }
  return {kernel_error::NO_THREAD, 0};
}

template <> void KERNEL::terminate(uint32_t thread) {
  if (thread >= m_thread_count)
    return;
  ARCH::store_release32(&m_threads[thread].thread.state,
                        (uint32_t)THREAD::state_t::TERMINATED);
}

// ★READY からのみ終了させる。**「他人を終わらせてよい唯一の瞬間」を CAS で
//   取る**ための口 (D55)。状態を見てから書く形にすると、見た直後に他コアが
//   claim して走り出せる (見る/書くの間が空く) ので、判定と確定を 1 命令に
//   畳んでいる。
// ★READY を選ぶ理由: do_switch は**文脈の退避が済んでから** READY を書き、
//   m_current は既に別の相手へ移っている。つまりこの系で READY は
//   「どのコアもこの足場を使っていない」と同義。RUNNING (他コアが走行中)、
//   WAIT_GRANT (貸し手として巻き取り待ち)、SUSPENDED (デバッガが止めている)
//   のどれからも取らない。
template <> bool KERNEL::terminate_if_idle(uint32_t thread) {
  if (thread >= m_thread_count || thread == 0)
    return false;
  return ARCH::cas32(&m_threads[thread].thread.state,
                     (uint32_t)THREAD::state_t::READY,
                     (uint32_t)THREAD::state_t::TERMINATED);
}

// 枠を返す。記憶そのものを返すのは貸し主 (オブジェクトランド) の仕事で、
// ここは「もう誰も使っていない」ことにするだけ。
template <> void KERNEL::release(uint32_t thread) {
  if (thread >= m_thread_count || thread == 0)
    return;
  if (m_threads[thread].thread.state !=
      (uint32_t)THREAD::state_t::TERMINATED)
    return;
  // ★状態を空きへ戻す**前に**世代を進める。逆順にすると、空きを見た他コアが
  //   枠を取ってから世代が動くことになり、新しい住人の世代が一瞬古いままになる。
  m_threads[thread].thread.generation++;
  m_threads[thread].thread.ledger = nullptr;
  m_threads[thread].thread.ledger_capacity = 0;
  m_threads[thread].thread.call_stack = {};
  ARCH::store_release32(&m_threads[thread].thread.state,
                        (uint32_t)THREAD::state_t::UNINITIALIZED);
}

// ---- 実行権の受け渡し -------------------------------------------------------
// READY → RUNNING を CAS で取る。これが「2 コアが同じ文脈を走らせない」根。
template <> SHIZUKU_SVC_RAM bool KERNEL::claim(uint32_t thread, kernel_error &error) {
  if (thread >= m_thread_count) {
    error = kernel_error::NOT_READY;
    return false;
  }
  if ((m_threads[thread].thread.affinity & (1u << BOARD::core_num())) == 0) {
    error = kernel_error::BAD_AFFINITY;
    return false;
  }
  if (!ARCH::cas32(&m_threads[thread].thread.state, (uint32_t)THREAD::state_t::READY,
                   (uint32_t)THREAD::state_t::RUNNING)) {
    error = kernel_error::NOT_READY;
    return false;
  }
  return true;
}

// SWITCH = GRANT(target, 無制限)。自分が持っている CPU 予算の全部を渡す: 外側に貸し手が
// 居て期限があればその残りが (do_grant が外側の残りでクランプする)、起点のスレッドや
// kernel object のように予算が無制限なら無制限で渡る。渡した側は WAIT_GRANT で待ち、
// 借り手の YIELD (grant_unwind) か期限切れで戻ってくる。
// ★以前の「借り手が撃つと対象を無視して貸し手へ返す」「起点が撃つと READY に戻るだけの
//   バトン渡し」は無くなった。借り手の SWITCH(C) は C への又貸し (A→B→C) になる。
//   貸し手へ返したいときは SWITCH(貸し手) か YIELD。
template <> SHIZUKU_SVC_RAM kernel_error KERNEL::do_switch(uint32_t target) {
  return do_grant(target, GRANT_UNLIMITED);
}

template <>
SHIZUKU_SVC_RAM kernel_error KERNEL::do_grant(uint32_t target, uint64_t cycles) {
  const uint32_t core = BOARD::core_num();
  const uint32_t current = m_current[core];
  if (target == current)
    return kernel_error::OK; // 自分へ貸すのは何もしないのと同じ
  grant_stack &grants = m_grants[core];
  // FIXME(integrate): 祖先 unwind は設計誤り (未修正で持ち込み)
  // 祖先への貸付け (A → B → A 等) は、祖先まで返却して grant_end=YIELDED 相当として扱い、
  // 新たな貸付けフレームを積まない。
  for (uint32_t index = 0; index < grants.depth; ++index) {
    if (grants.frames[index].lender == target) {
      while (m_current[core] != target && grants.depth > 0) {
        grant_unwind(grant_end::YIELDED);
      }
      return kernel_error::OK;
    }
  }
  if (grants.depth >= grant_stack::MAX_DEPTH)
    return kernel_error::GRANT_DEPTH;
  // ★外側の残りを読む前に、今の刻みで使ったぶんを引いておく。引かずに読むと
  //   「外側はまだ丸ごと残っている」ことになり、又貸しで実質延長できてしまう。
  grant_charge();
  // ★残量は外側の残量でクランプする (I-7)。借りたものを又貸しで延長できない。
  uint64_t budget = cycles;
  if (grants.depth != 0 && grants.frames[grants.depth - 1].remaining < budget)
    budget = grants.frames[grants.depth - 1].remaining;
  // ★タイマが正直に数えられる最小を下回るなら**貸さずに断る**。装填には下限が
  //   あり (装填直後の発火は取りこぼす)、下限へ丸め上げて貸すと借り手は
  //   **貸された以上に走れる** — 外側でクランプしている意味が消える。
  //   ★claim より前に判定する。先に claim すると、断るときに相手を READY へ
  //     戻す後始末が要る (戻し忘れると走れるスレッドが黙って消える)。
  if (budget < ARCH::TIMER_MIN_CYCLES)
    return kernel_error::GRANT_TOO_SMALL;
  kernel_error error = kernel_error::OK;
  if (!claim(target, error))
    return error;
  grants.frames[grants.depth++] = {current, budget};
  // 貸し手は WAIT_GRANT。READY ではないので他コアに拾われず、復帰はこのコアの
  // 巻き取り経路 (期限 or 早期復帰) だけになる。
  m_threads[current].thread.set_state(THREAD::state_t::WAIT_GRANT);
  set_current(core, target);
  arm_timer();
  return kernel_error::OK;
}

// 貸した実行権を 1 段巻き取る。期限切れ (EXPIRED) と早期復帰 (YIELDED) の共通経路。
template <> SHIZUKU_SVC_RAM void KERNEL::grant_unwind(grant_end reason) {
  const uint32_t core = BOARD::core_num();
  grant_stack &grants = m_grants[core];
  if (grants.depth == 0)
    return;
  grant_charge(); // 巻き取る前に、今の刻みで使ったぶんを外側にも負担させる
  const uint32_t borrower = m_current[core];
  const uint32_t lender = grants.frames[grants.depth - 1].lender;
  grants.depth--;
  // 貸し手が待っていた syscall の戻り値を書く (a0 は貸した時点で OK 済み)。
  ARCH::set_result(*m_threads[lender].thread.context->sp, (uintptr_t)kernel_error::OK,
                   (uintptr_t)reason);
  // ★★貸し手が**止められている**なら RUNNING へ戻さない (D54)。戻すと
  //   デバッガの停止が無かったことになる (「止めたのに走り続ける」の経路の
  //   1 つがこれだった)。状態を残しておけば、次に schedule() が候補を選ぶとき
  //   READY でないので拾われない。
  if (!m_threads[lender].thread.is_state(THREAD::state_t::SUSPENDED))
    m_threads[lender].thread.set_state(THREAD::state_t::RUNNING);
  set_current(core, lender);
  // 借り手を返す。走り終えていたらそのまま (終了させたスレッドを生き返らせない)。
  if (m_threads[borrower].thread.is_state(THREAD::state_t::RUNNING))
    ARCH::store_release32(&m_threads[borrower].thread.state,
                          (uint32_t)THREAD::state_t::READY);
  // 外側の残量へ張り替える (空なら止める。使い切っていれば即もう一段巻き取らせる)。
  if (grants.depth == 0) {
    ARCH::timer_cancel();
    m_armed[core] = 0;
  } else if (grants.frames[grants.depth - 1].remaining == 0) {
    grant_unwind(grant_end::EXPIRED);
  } else {
    arm_timer();
  }
}

// 今の刻みで使ったぶんを**全段から**引く。内側が走っている間は外側の時間も
// 減っている (だから又貸しで延長できない) ので、一番内側だけ引くのでは足りない。
// ★何度呼んでも壊れない: 引いたあと m_armed を「まだ引いていない残り」に
//   更新するので、続けて呼べば 2 回目は 0 を引く。
template <> SHIZUKU_SVC_RAM void KERNEL::grant_charge() {
  const uint32_t core = BOARD::core_num();
  grant_stack &grants = m_grants[core];
  if (grants.depth == 0 || m_armed[core] == 0) {
    m_armed[core] = 0;
    return;
  }
  bool wrapped = false;
  const uint32_t left = ARCH::timer_remaining(wrapped);
  // 折り返していた = 刻みは撃ち切った。多めに数える側へ倒す (少なく数えると
  // 貸した実行権が予定より長く握られる)。
  const uint64_t used =
      (wrapped || left == 0) ? m_armed[core] : (uint64_t)(m_armed[core] - left);
  for (uint32_t index = 0; index < grants.depth; ++index) {
    grant_frame &frame = grants.frames[index];
    if (frame.remaining == GRANT_UNLIMITED)
      continue; // 無制限の段は減らない
    frame.remaining = frame.remaining > used ? frame.remaining - used : 0;
  }
  m_armed[core] = wrapped ? 0 : left;
}

// 一番内側の残量をタイマへ装填する。★換算が要らない — 残量もタイマもクロックで
// 数えているので、そのまま載る。µs で持っていたときは装填のたびに clk_sys で
// 割り戻していて、その clk_sys が変わらない保証は無かった。
template <> SHIZUKU_SVC_RAM void KERNEL::arm_timer() {
  const uint32_t core = BOARD::core_num();
  grant_stack &grants = m_grants[core];
  if (grants.depth == 0) {
    ARCH::timer_cancel();
    m_armed[core] = 0;
    return;
  }
  uint64_t chunk = grants.frames[grants.depth - 1].remaining;
  if (chunk == GRANT_UNLIMITED) {
    // 期限なし (SWITCH)。取り上げる相手が居ないのでタイマは要らない。
    ARCH::timer_cancel();
    m_armed[core] = 0;
    return;
  }
  // タイマは幅が有限なので、長い貸しは刻んで継ぎ足す (残量自体は変わらない)。
  if (chunk > ARCH::TIMER_MAX_CYCLES)
    chunk = ARCH::TIMER_MAX_CYCLES;
  // 装填直後に発火する設定は取りこぼすので下限がある。残りがこれ未満のときは
  // わずかに超過して返ることになる (下限ぶんの誤差は仕様として飲む)。
  if (chunk < ARCH::TIMER_MIN_CYCLES)
    chunk = ARCH::TIMER_MIN_CYCLES;
  m_armed[core] = (uint32_t)chunk;
  ARCH::timer_oneshot((uint32_t)chunk);
}

// タイマ例外 (優先度は syscall より下、切替より上)。ここでは**切替を起票するだけ**で、
// 実際の切替は最低優先度の遅延例外で起こす — これが 1 コア内の相互排除を無償で
// 与える規約 (DESIGN §14.5.1)。
template <> void KERNEL::timer_expired() {
  const uint32_t core = BOARD::core_num();
  m_faults.systick = cost_probe::saturating_increment(m_faults.systick); // 診断: SysTick の回数 (全コア合算)
  grant_stack &grants = m_grants[core];
  if (grants.depth == 0) {
    ARCH::timer_cancel(); // 早期復帰と競合した後の遅れて来た発火
    m_armed[core] = 0;
    return;
  }
  // ここへ来た時点で刻みは撃ち切っている (COUNTFLAG が立つので grant_charge が
  // 丸ごと引く)。使い切っていなければ次の刻みを装填するだけ。
  grant_charge();
  if (grants.frames[grants.depth - 1].remaining == 0) {
    ARCH::timer_cancel();
    ARCH::pend_context_switch();
  } else {
    arm_timer(); // 刻みの継ぎ足し
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
// そのスレッドだけ止める。★TERMINATED と違って文脈は生きたままなので、
//   レジスタを見て書き換えてから再開できる。走っているスレッドを止めた場合、
//   切り替えるのは呼び出し側の仕事 (ここは状態を変えるだけ)。
template <> void KERNEL::suspend(uint32_t thread) {
  if (thread >= m_thread_count)
    return;
  ARCH::store_release32(&m_threads[thread].thread.state,
                        (uint32_t)THREAD::state_t::SUSPENDED);
}

// 止めたスレッドを走れる状態へ戻す。★SUSPENDED のものだけ — 終わったスレッドを
//   生き返らせない。
template <> void KERNEL::resume(uint32_t thread) {
  if (thread >= m_thread_count)
    return;
  ARCH::cas32(&m_threads[thread].thread.state,
              (uint32_t)THREAD::state_t::SUSPENDED,
              (uint32_t)THREAD::state_t::READY);
}

// ---- 自己ホスト型デバッグの受け口 ------------------------------------------
// ★フォールトとまったく同じ形にしてある。違うのは「終わらせる」か「止める」かだけ
//   で、切り替えの経路は共有する (経路を増やさない)。
template <> void KERNEL::debug_dispatch(KERNEL::CONTEXT *context) {
  const uint32_t core = BOARD::core_num();
  const uint32_t thread = m_current[core];
  if (thread < m_thread_count && m_threads[thread].thread.is_debug_protected) {
    // デバッガ自身がブレークポイントを踏んだ場合は無視して継続
    // (そうしないとデッドロックや無限ループになる)
    return;
  }

  m_debug.count++;
  m_debug.thread = thread;
  m_debug.pc = ARCH::frame_pc(*context->sp);
  m_debug.reason = ARCH::debug_reason_take();

  // ★1 命令実行の指示は使い切りにする。消さないと、再開した先で延々と止まり続ける。
  ARCH::debug_step(false);

  // ★カーネル自身の中で止まったら、そこから先へ進めない (止めるスレッドが無い)。
  //   デバッガはスレッドモードのコードだけを相手にする。
  if (!ARCH::faulted_in_thread_mode(*context)) {
    BOARD::diag_printf("[DEBUG] event inside the kernel: pc=%08lx dfsr=%08lx\n",
                       (unsigned long)m_debug.pc, (unsigned long)m_debug.reason);
    return; // 何もせず戻る (止めたら系ごと動かなくなる)
  }

  suspend(thread);

  // 借り手として走っていたなら貸し手へ返す。そうでなければ復帰先へ渡す。
  // ★フォールトと同じ判断 — 「このコアで次に誰を走らせるか」は 1 か所で決める。
  if (m_grants[core].depth != 0) {
    grant_unwind(grant_end::EXPIRED);
    return;
  }
  kernel_error error = kernel_error::OK;
  if (m_recovery_thread < m_thread_count && claim(m_recovery_thread, error)) {
    set_current(core, m_recovery_thread);
    return;
  }
  // ★渡す先が無いなら**止めるのをやめる**。フォールトと違ってデバッグ事象は
  //   「続けられる」ので、ここで panic するのは過剰。かといって止めたまま戻ると
  //   同じ場所へ復帰して延々と止まり続ける (生きたまま何も進まない)。
  //   記録は残っているので、止められなかったことは後から分かる。
  resume(thread);
  m_debug.reason |= DEBUG_NOT_STOPPED;
}

// ★★1 命令だけ実行させたい対象を予約する。ここでは MON_STEP を立てない
//   (理由はヘッダのコメント通り)。単に「次にこの文脈へ復帰するときに立てて」
//   と書き置くだけ。
template <> void KERNEL::arm_step(uint32_t thread) {
  ARCH::store_release32(&m_step_target, thread);
}

// ISA 層の CTX_RESTORE から**あらゆる復帰経路**で毎回呼ばれる。復帰しようと
// している文脈がちょうど予約した相手のときだけ、ここで初めて MON_STEP を
// 立てる — こうすると「次の 1 命令」が確実に対象自身の命令になる。
// ★CAS で消費する: 2 コアが同時に別の文脈を復帰させても、この予約を
//   誤って 2 回消費したり、無関係な文脈へ誤爆したりしない。
template <> SHIZUKU_SVC_RAM void KERNEL::consume_pending_step_slow() {
  const uint32_t thread = m_current[BOARD::core_num()];
  if (ARCH::cas32(&m_step_target, thread, NO_STEP_TARGET))
    ARCH::debug_step(true);
}

template <> void KERNEL::fault_dispatch(KERNEL::CONTEXT *context) {
  const uint32_t core = BOARD::core_num();
  const uint32_t thread = m_current[core];
  const uint32_t status = ARCH::fault_status();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_fault_phase(10, thread);
#endif

  // ★触ろうとした先は**消す前に**読む。違反の記録を消すと同時に無効になるので、
  //   順序を逆にすると意味のない値を報告してしまう (実際それで嘘の値が出た)。
  const uintptr_t address = ARCH::fault_address();
  const uintptr_t pc = ARCH::frame_pc(*context->sp);
  m_faults.count++;
  m_faults.pc = pc;
  m_faults.status = status;
  m_faults.thread = thread;
  m_faults.address = address;
  if constexpr (ARCH::IS_ARMV8M) {
    const auto klass = cost_probe::classify_cfsr(status);
    if (klass.memmanage) m_faults.memmanage = cost_probe::saturating_increment(m_faults.memmanage);
    if (klass.busfault) m_faults.busfault = cost_probe::saturating_increment(m_faults.busfault);
    if (klass.usagefault) m_faults.usagefault = cost_probe::saturating_increment(m_faults.usagefault);
    if (klass.hardfault) m_faults.hardfault = cost_probe::saturating_increment(m_faults.hardfault);
  }
  ARCH::fault_status_clear();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_fault_phase(11, m_grants[core].depth);
#endif


  // ★止めても直らないのは「カーネル自身が落ちた」場合だけ。スレッドモードで
  //   落ちたのなら、そのスレッドを止めれば系は続けられる。
  if (!ARCH::faulted_in_thread_mode(*context)) {
    BOARD::diag_printf("[FAULT] kernel itself faulted: pc=%08lx cfsr=%08lx\n",
                       (unsigned long)pc, (unsigned long)status);
    BOARD::panic("fault in kernel context");
  }
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_fault_phase(12, 0);
#endif


  BOARD::diag_printf("[FAULT] thread %lu stopped: pc=%08lx addr=%08lx "
                     "cfsr=%08lx sp=%08lx (系は継続)\n",
                     (unsigned long)thread, (unsigned long)pc,
                     (unsigned long)address,
                     (unsigned long)status, (unsigned long)(uintptr_t)context->sp);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_fault_phase(13, 0);
#endif


  ARCH::store_release32(&m_threads[thread].thread.state,
                        (uint32_t)THREAD::state_t::TERMINATED);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_fault_phase(14, 0);
#endif


  // 借り手として走っていたなら、貸し手へ返すのが自然な復帰先 (貸した側は
  // 「期限が来た」のと同じ形で戻ってくる)。
  if (m_grants[core].depth != 0) {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
    shizuku_selftest_fault_phase(15, 0);
#endif
    grant_unwind(grant_end::EXPIRED);
    return;
  }
  // そうでなければ、あらかじめ教えられている復帰先へ渡す。誰に渡すかは方針なので
  // カーネルは選ばない — 教えられていないなら渡す先が無い。
  kernel_error error = kernel_error::OK;
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_fault_phase(16, m_recovery_thread);
#endif
  if (m_recovery_thread < m_thread_count && claim(m_recovery_thread, error)) {
    set_current(core, m_recovery_thread);
    return;
  }
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_fault_phase(17, m_recovery_thread);
#endif
  BOARD::diag_printf("[FAULT] 渡す先が無い (recovery=%lu)\n",
                     (unsigned long)m_recovery_thread);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  shizuku_selftest_fault_phase(18, m_recovery_thread);
#endif

  BOARD::panic("no thread to run after fault");
}

} // namespace shizuku
