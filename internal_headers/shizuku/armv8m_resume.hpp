#ifndef SHIZUKU_ARMV8M_RESUME_HPP
#define SHIZUKU_ARMV8M_RESUME_HPP
#include "shizuku/kernel.hpp"

namespace shizuku {

// The only window/step preparation shared by SVC and PendSV. The caller has
// already selected the final activation/thread; intermediate unwind steps do
// not touch the hardware stack window.
// thread / core は呼び出し側がすでに引いた現在スレッドと core_num (SVC は入口で 1 度だけ構築する)。
[[gnu::always_inline]] inline KERNEL::CONTEXT *armv8m_prepare_resume(KERNEL::THREAD &thread,
                                                                     ARCH::core_num core) {
  auto &kernel = kernel_instance;
  auto *context = thread.context;
  kernel.consume_pending_step(core);
  ARCH::apply_grant_window(context->region_base, context->region_limit, core);
  uint32_t stack_limit = thread.stack_rlar;
  if (thread.call_stack.depth != 0) {
    const uintptr_t ceiling = ARCH::record_return_sp(thread.call_stack.top);
    const uintptr_t full_ceiling = (thread.stack_rlar & ~uintptr_t(31)) + 32;
    if ((ceiling & 31u) != 0 || ceiling > full_ceiling ||
        ceiling <= (thread.stack_rbar & ~uintptr_t(31)))
      KERNEL::BOARD::panic("invalid in-place stack ceiling");
    stack_limit = ARCH::shrink_stack_limit(thread.stack_rlar, ceiling);
  }
  // 窓が要るのは非特権が走るときだけ。特権は PRIVDEFENA で窓に制限されない (region 2 が覆わない
  // stack も特権には届く) ので、特権の再開では Region 2 を無効 (ARCH::PRIVILEGED_STACK_*) にする。
  // 無効は影の正規値 {0, 0} なので、特権 → 特権は影と同じで何も書かない。非特権の再開 (今が非特権) は
  // 従来どおり thread の窓 (呼び先なら shrink 済み) を張る — 非特権が絡む遷移は必ず影と比べて張り直す。
  // ★判定は context の特権だけで、前回の再開を覚える状態は持たない (非特権の経路に費用を足さない)。
  //   遷移ごとの Region 2 の扱いは m2m-switch-20261117/report1.md の「4 回目 (W1 の採用)」の表。
  if (ARCH::context_unprivileged(*context)) {
    ARCH::apply_stack_window(thread.stack_rbar, stack_limit, core);
    ARCH::switch_caller_protection(thread.identity_object(), core);
  } else {
    ARCH::apply_stack_window(ARCH::PRIVILEGED_STACK_RBAR, ARCH::PRIVILEGED_STACK_RLAR, core);
  }
  // 入場・復帰で張った窓と、ここまでの貸与窓・stack 窓の書込みを、例外復帰の前に 1 度で待つ。
  ARCH::mpu_sync(core);
  return context;
}

} // namespace shizuku
#endif
