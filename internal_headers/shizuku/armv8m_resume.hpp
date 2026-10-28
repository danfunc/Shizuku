#ifndef SHIZUKU_ARMV8M_RESUME_HPP
#define SHIZUKU_ARMV8M_RESUME_HPP
#include "shizuku/kernel.hpp"

namespace shizuku {

// The only window/step preparation shared by SVC and PendSV. The caller has
// already selected the final activation/thread; intermediate unwind steps do
// not touch the hardware stack window.
[[gnu::always_inline]] inline KERNEL::CONTEXT *armv8m_prepare_resume() {
  auto &kernel = kernel_instance;
  auto &thread = kernel.current_thread();
  auto *context = thread.context;
  kernel.consume_pending_step();
  ARCH::apply_grant_window(context->region_base, context->region_limit);
  uint32_t stack_limit = thread.stack_rlar;
  if (thread.call_stack.depth != 0) {
    const uintptr_t ceiling = thread.ledger[thread.call_stack.depth - 1].ret_sp;
    const uintptr_t full_ceiling = (thread.stack_rlar & ~uintptr_t(31)) + 32;
    if ((ceiling & 31u) != 0 || ceiling > full_ceiling ||
        ceiling <= (thread.stack_rbar & ~uintptr_t(31)))
      KERNEL::BOARD::panic("invalid in-place stack ceiling");
    stack_limit = ARCH::shrink_stack_limit(thread.stack_rlar, ceiling);
  }
  ARCH::apply_stack_window(thread.stack_rbar, stack_limit);
  if (ARCH::context_unprivileged(*context))
    ARCH::switch_caller_protection(thread.identity_object());
  // 入場・復帰で張った窓と、ここまでの貸与窓・stack 窓の書込みを、例外復帰の前に 1 度で待つ。
  ARCH::mpu_sync();
  return context;
}

} // namespace shizuku
#endif
