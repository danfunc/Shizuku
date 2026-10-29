#ifndef SHIZUKU_ARMV8M_RESUME_HPP
#define SHIZUKU_ARMV8M_RESUME_HPP
#include "shizuku/kernel.hpp"

namespace shizuku {

// The only window/step preparation shared by SVC and PendSV. The caller has
// already selected the final activation/thread; intermediate unwind steps do
// not touch the hardware stack window.
//   thread / core は呼び出し側がすでに引いた現在スレッドと core_slot() の値 (SVC は入口で 1 度だけ引く。
//   例外ハンドラは自分を取った core で最後まで走るので、値は途中で変わらない)。
[[gnu::always_inline]] inline KERNEL::CONTEXT *armv8m_prepare_resume(KERNEL::THREAD &thread,
                                                                     uint32_t core) {
  auto &kernel = kernel_instance;
  auto *context = thread.context;
  kernel.consume_pending_step();
  ARCH::apply_grant_window(context->region_base, context->region_limit, core);
  uint32_t stack_limit = thread.stack_rlar;
  if (thread.call_stack.depth != 0) {
    const uintptr_t ceiling = thread.ledger[thread.call_stack.depth - 1].ret_sp;
    const uintptr_t full_ceiling = (thread.stack_rlar & ~uintptr_t(31)) + 32;
    if ((ceiling & 31u) != 0 || ceiling > full_ceiling ||
        ceiling <= (thread.stack_rbar & ~uintptr_t(31)))
      KERNEL::BOARD::panic("invalid in-place stack ceiling");
    stack_limit = ARCH::shrink_stack_limit(thread.stack_rlar, ceiling);
  }
  ARCH::apply_stack_window(thread.stack_rbar, stack_limit, core);
  if (ARCH::context_unprivileged(*context))
    ARCH::switch_caller_protection(thread.identity_object(), core);
  // 入場・復帰で張った窓と、ここまでの貸与窓・stack 窓の書込みを、例外復帰の前に 1 度で待つ。
  ARCH::mpu_sync(core);
  return context;
}

[[gnu::always_inline]] inline KERNEL::CONTEXT *armv8m_prepare_resume() {
  return armv8m_prepare_resume(kernel_instance.current_thread(), ARCH::core_slot());
}

} // namespace shizuku
#endif
