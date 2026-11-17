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
  // 実験 (skip_priv_stack_window_enabled、既定 off): 前後とも特権なら stack 窓を張り替えない。
  //   特権は PRIVDEFENA で窓に制限されない (region 2 が覆わない stack も特権には届く) ので、特権が
  //   走っている間は region 2 の中身は何にも効かない。窓が要るのは非特権が走るときだけで、
  //   非特権の再開 (今が非特権) と、非特権から特権へ移った最初の再開 (直前が非特権) は必ず張る。
  //   遷移ごとの Region 2 の扱いは m2m-switch-20261117/report1.md の「3 回目 (W1)」の表。
  if constexpr (skip_priv_stack_window_enabled) {
    if (!ARCH::stack_window_skippable(!ARCH::context_unprivileged(*context), core))
      ARCH::apply_stack_window(thread.stack_rbar, stack_limit, core);
  } else {
    ARCH::apply_stack_window(thread.stack_rbar, stack_limit, core);
  }
  if (ARCH::context_unprivileged(*context))
    ARCH::switch_caller_protection(thread.identity_object(), core);
  // 入場・復帰で張った窓と、ここまでの貸与窓・stack 窓の書込みを、例外復帰の前に 1 度で待つ。
  ARCH::mpu_sync(core);
  return context;
}

} // namespace shizuku
#endif
