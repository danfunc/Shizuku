#include "shizuku/kernel.hpp"

namespace shizuku {
template <> void KERNEL::grant_charge();
template <> void KERNEL::arm_timer();
template <> void KERNEL::task_attach(uint32_t thread, uint32_t core) {
  auto &mask = m_threads[thread].thread.task_mask;
  uint32_t old;
  do { old = ARCH::load_acquire32(&mask); }
  while (!ARCH::cas32(&mask, old, old | (1u << core)));
}

template <> void KERNEL::task_detach(uint32_t thread, uint32_t core) {
  if (m_current[core] == thread)
    return;
  const auto &task = cpu_manager.execution_task(core);
  for (uint32_t i = 0; i < task.depth; ++i)
    if (task.frames[i].lender == thread &&
        task.frames[i].generation == m_threads[thread].thread.generation)
      return;
  auto &mask = m_threads[thread].thread.task_mask;
  uint32_t old;
  do { old = ARCH::load_acquire32(&mask); }
  while (!ARCH::cas32(&mask, old, old & ~(1u << core)));
}

template <> void KERNEL::resume_tasks() {
  const uint32_t self = BOARD::core_num();
  if (ARCH::load_acquire32(&m_pause_owner) != self + 1)
    return;
  for (uint32_t core = 0; core < CORE_COUNT; ++core)
    ARCH::store_release32(&cpu_manager.execution_task(core).move_blocked, 0);
  // A late acknowledgement from a cancelled epoch cannot satisfy a new one.
  for (uint32_t core = 0; core < CORE_COUNT; ++core)
    if (core != self)
      while (ARCH::load_acquire32(&cpu_manager.execution_task(core).paused) == m_pause_epoch) {}
  const bool restore = m_pause_irq_saved;
  const uint32_t irq = m_pause_irq;
  if (restore) arm_timer();
  m_pause_irq_saved = false;
  ARCH::store_release32(&m_pause_owner, 0);
  if (restore) ARCH::interrupt_restore(irq);
}

template <> bool KERNEL::pause_tasks(uint32_t timeout_us) {
  const uint32_t self = BOARD::core_num();
  if (!ARCH::cas32(&m_pause_owner, 0, self + 1))
    return false;
  if (++m_pause_epoch == 0) ++m_pause_epoch;
  for (uint32_t core = 0; core < CORE_COUNT; ++core) {
    ARCH::store_release32(&cpu_manager.execution_task(core).move_blocked, m_pause_epoch);
    if (core != self && ARCH::load_acquire32(&m_online[core]))
      BOARD::notify_core(core);
  }
  const uint64_t start = BOARD::time_us();
  for (uint32_t core = 0; core < CORE_COUNT; ++core) {
    if (core == self || !ARCH::load_acquire32(&m_online[core]))
      continue;
    while (ARCH::load_acquire32(&cpu_manager.execution_task(core).paused) != m_pause_epoch) {
      if (BOARD::time_us() - start >= timeout_us) {
        resume_tasks();
        return false; // no context or resource has been invalidated
      }
    }
  }
  m_pause_irq = ARCH::interrupt_save();
  m_pause_irq_saved = true;
  grant_charge();
  ARCH::timer_cancel();
  cpu_manager.execution_task(self).armed = 0;
  return true;
}

template <> bool KERNEL::pause_dispatch() {
  const uint32_t core = BOARD::core_num();
  auto &task = cpu_manager.execution_task(core);
  const uint32_t epoch = ARCH::load_acquire32(&task.move_blocked);
  if (!epoch ||
      ARCH::load_acquire32(&m_pause_owner) == core + 1)
    return false;
  // Never park an object handler while it can own shared bookkeeping locks.
  // svc_dispatch re-pends us when the handler returns to PLAIN execution.
  if (current_thread().current_kind != (uint32_t)object_kind::PLAIN)
    return true;
  const uint32_t irq = ARCH::interrupt_save();
  if (ARCH::load_acquire32(&task.move_blocked) != epoch) {
    ARCH::interrupt_restore(irq);
    return true;
  }
  grant_charge();
  ARCH::timer_cancel();
  task.armed = 0;
  ARCH::store_release32(&task.paused, epoch);
  while (ARCH::load_acquire32(&task.move_blocked) == epoch) {}
  // The coordinator may have changed m_current and pruned the grant history.
  arm_timer();
  ARCH::store_release32(&task.paused, 0);
  ARCH::interrupt_restore(irq);
  return true;
}

template <> bool KERNEL::destroy_contexts(uint32_t victims,
                                         const uint32_t *fallbacks) {
  using state_t = THREAD::state_t;
  const uint32_t self = BOARD::core_num();
  if (m_thread_count > 32 || ARCH::load_acquire32(&m_pause_owner) != self + 1)
    return false;
  if (victims & (1u << m_current[self]))
    return false; // caller must run on a separate destruction context
  for (uint32_t core = 0; core < CORE_COUNT; ++core) {
    if (core != self && !ARCH::load_acquire32(&m_online[core])) continue;
    if (core != self &&
        ARCH::load_acquire32(&cpu_manager.execution_task(core).paused) != m_pause_epoch)
      return false;
    if (victims & (1u << m_current[core])) {
      bool returning = false;
      const auto &task = cpu_manager.execution_task(core);
      for (uint32_t i = 0; i < task.depth; ++i) {
        const auto &frame = task.frames[i];
        if (frame.lender < m_thread_count && !(victims & (1u << frame.lender))) {
          const auto &lender = m_threads[frame.lender].thread;
          returning |= lender.generation == frame.generation && lender.is_state(state_t::WAIT_GRANT);
        }
      }
      if (returning) continue;
      const uint32_t fallback = fallbacks[core];
      if (fallback >= m_thread_count || (victims & (1u << fallback)) ||
          !m_threads[fallback].thread.is_state(state_t::READY) ||
          !(m_threads[fallback].thread.affinity & (1u << core)))
        return false;
    }
  }
  uint32_t affected = 0;
  for (uint32_t t = 0; t < m_thread_count; ++t)
    if (victims & (1u << t)) {
      affected |= thread_tasks(t);
      m_threads[t].thread.set_state(state_t::TERMINATED);
    }
  for (uint32_t core = 0; core < CORE_COUNT; ++core) {
    if (!(affected & (1u << core)))
      continue;
    auto &task = cpu_manager.execution_task(core);
    uint32_t out = 0;
    for (uint32_t i = 0; i < task.depth; ++i) {
      const auto frame = task.frames[i];
      if (frame.lender < 32 && (victims & (1u << frame.lender)))
        continue;
      task.frames[out++] = frame;
    }
    task.depth = out;
    if (victims & (1u << m_current[core])) {
      // Returning via the surviving lender preserves its pending GRANT result.
      // No hardware timer operation is performed on a remote core here.
      bool found = false;
      while (task.depth) {
        auto frame = task.frames[--task.depth];
        if (frame.lender >= m_thread_count)
          continue;
        auto &lender = m_threads[frame.lender].thread;
        if (lender.generation != frame.generation)
          continue;
        task_detach(frame.lender, core);
        if (!lender.is_state(state_t::WAIT_GRANT) &&
            !lender.is_state(state_t::SUSPENDED))
          continue;
        ARCH::set_result(*lender.context->sp, (uintptr_t)kernel_error::OK,
                         (uintptr_t)grant_end::EXPIRED);
        if (lender.is_state(state_t::SUSPENDED))
          continue;
        lender.set_state(state_t::RUNNING);
        m_current[core] = frame.lender;
        task_attach(frame.lender, core);
        found = true;
        break;
      }
      if (!found) {
        const uint32_t next = fallbacks[core];
        m_threads[next].thread.set_state(state_t::RUNNING);
        m_current[core] = next;
        task_attach(next, core);
      }
    }
  }
  for (uint32_t t = 0; t < m_thread_count; ++t)
    if (victims & (1u << t)) {
      ARCH::store_release32(&m_threads[t].thread.task_mask, 0);
      // Clear an outstanding debugger reservation before this ID can be reused.
      ARCH::cas32(&m_step_target, t, NO_STEP_TARGET);
    }
  return true;
}
} // namespace shizuku
