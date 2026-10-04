#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"

namespace shizuku {
template <> bool KERNEL_OBJECT::destroy_poll();
template <> bool KERNEL_OBJECT::ensure_destroy_runtime();
template <> uint32_t KERNEL_OBJECT::spawn_system_thread(uintptr_t, uint32_t, bool);
template <> void KERNEL_OBJECT::release_object_memory(uint32_t);

namespace {
// A distinct stack is essential: the requester's context may be among victims.
void destroy_worker() {
  for (;;) {
    kernel_object_instance.destroy_poll();
    // Return a borrowed grant (including self-destroy's grant), or hand an
    // unborrowed lane to its reserved idle. Never spin forever as KERNEL_OBJECT.
    KERNEL::ARCH::syscall((uintptr_t)primitive::SWITCH,
                          kernel_instance.recovery_thread());
  }
}
void destroy_idle() {
  for (;;) KERNEL::ARCH::syscall((uintptr_t)object_api::YIELD);
}
}

template <> void KERNEL_OBJECT::set_destroy_hook(uint32_t object,
                                                destroy_hook hook) {
  table_lock();
  if (object < OBJECT_COUNT && !m_objects[object].closing)
    m_destroy_hooks[object] = hook;
  table_unlock();
}

// Caller owns the bookkeeping lock. Publication follows ALL policy metadata.
template <> uint32_t KERNEL_OBJECT::spawn_system_thread(uintptr_t entry,
                                                        uint32_t core,
                                                        bool worker) {
  const uintptr_t stack = arena_allocate(m_objects_arena, THREAD_STACK_BYTES,
                                         KERNEL_OBJECT_ID);
  if (!stack) return UINT32_MAX;
  KERNEL::spawn_request request{};
  request.entry_pc = entry;
  request.stack_base = stack;
  request.stack_bytes = THREAD_STACK_BYTES;
  request.affinity = worker ? 0 : 1u << core;
  request.object_id = worker ? KERNEL_OBJECT_ID : ROOT_OBJECT;
  request.kind = (uint32_t)(worker ? object_kind::KERNEL_OBJECT : object_kind::PLAIN);
  request.parent_handler_object = KERNEL_OBJECT_ID;
  request.parent_handler_entry = handler_entry();
  request.publish = false;
  const auto result = kernel_instance.spawn(request);
  if (result.error != kernel_error::OK) {
    arena_release(m_objects_arena, stack);
    return UINT32_MAX;
  }
  const uint32_t t = result.thread;
  m_thread_stack[t] = stack;
  m_thread_object[t] = (uint16_t)request.object_id;
  m_shadow[t].depth = 0;
  m_wake_at[t] = 0;
  m_budget[t] = worker ? 0 : DEFAULT_BUDGET_CYCLES;
  m_kill_pending[t] = 0;
  kernel_instance.set_thread_debug_protected(t, true);
  return t;
}

template <> bool KERNEL_OBJECT::ensure_destroy_runtime() {
  // Called with table_lock held. Partial allocation is retained for retry.
  if (m_destroy_runtime) return true;
  for (uint32_t core = 0; core < KERNEL::CORE_COUNT; ++core) {
    if (m_destroy_fallback[core] != UINT32_MAX) continue;
    const uint32_t t = spawn_system_thread((uintptr_t)destroy_idle, core, false);
    if (t == UINT32_MAX) return false;
    ARCH::store_release32(&m_destroy_fallback[core], t);
    kernel_instance.set_core_recovery(core, t);
    kernel_instance.publish_thread(t);
  }
  if (m_destroy_worker == UINT32_MAX) {
    const uint32_t t = spawn_system_thread((uintptr_t)destroy_worker,
                                           KERNEL::BOARD::core_num(), true);
    if (t == UINT32_MAX) return false;
    ARCH::store_release32(&m_destroy_worker, t);
    kernel_instance.publish_thread(t);
  }
  m_destroy_runtime = m_destroy_worker != UINT32_MAX;
  return m_destroy_runtime;
}

template <> uintptr_t KERNEL_OBJECT::destroy_object(uintptr_t object,
                                                   object_error &error) {
  const uintptr_t caller = current_object(kernel_instance.current_thread_id());
  table_lock();
  auto fail = [&](object_error e) -> uintptr_t { error = e; table_unlock(); return 0; };
  if (object >= OBJECT_COUNT || !m_objects[object].created)
    return fail(object_error::BAD_OBJECT);
  if (object == ROOT_OBJECT || object == KERNEL_OBJECT_ID ||
      (caller != object && caller != ROOT_OBJECT && caller != KERNEL_OBJECT_ID))
    return fail(object_error::NOT_PRIVILEGED);
  if (m_destroy_target != NO_OBJECT || m_destroy_serial == UINT32_MAX)
    return fail(object_error::DESTROY_BUSY);
  // Raw method/stream pointers and IRQ registrations cannot be revoked by an
  // MPU context switch. Composition must supply a quiescence contract, even
  // if that contract is a no-op for an object with no exported references.
  if (!m_destroy_hooks[object]) return fail(object_error::DESTROY_BUSY);
  for (uint32_t i = 0; i < OBJECT_COUNT; ++i)
    if (i != object && m_objects[i].created &&
        m_objects[i].parent_handler_object == object)
      return fail(object_error::DESTROY_BUSY);
  if (!ensure_destroy_runtime()) return fail(object_error::NO_THREAD);
  const uint32_t ticket = ++m_destroy_serial;
  ARCH::store_release32(&m_objects[object].closing, 1);
  m_destroy_ticket[object] = ticket;
  m_destroy_result[object] = 0;
  ARCH::store_release32(&m_destroy_target, (uint32_t)object);
  table_unlock();
  // Transfer execution time to the separate context immediately when possible.
  // If it is busy or the caller has insufficient time, the ticket remains queued
  // for schedule(). Self-destruction can remove this very grant's lender.
  ARCH::syscall((uintptr_t)primitive::GRANT, m_destroy_worker, DEFAULT_BUDGET_CYCLES);
  return ticket;
}

template <> uintptr_t KERNEL_OBJECT::destroy_status(uintptr_t ticket,
                                                   object_error &error) {
  table_lock();
  for (uint32_t i = 0; ticket && i < OBJECT_COUNT; ++i)
    if (m_destroy_ticket[i] == ticket) {
      const auto result = m_destroy_result[i];
      table_unlock();
      return result;
    }
  table_unlock();
  error = object_error::BAD_OBJECT;
  return 0;
}

template <> void KERNEL_OBJECT::release_object_memory(uint32_t object) {
  // Linear walk despite coalescing: advance past the resulting block, not a
  // right-hand header that arena_release may have absorbed.
  for (uintptr_t p = m_objects_arena.base;
       p < m_objects_arena.base + m_objects_arena.bytes;) {
    auto *b = (block *)p;
    if (b->used && b->owner == object) {
      uintptr_t merged = p;
      if (b->prev_bytes && !((block *)(p - b->prev_bytes))->used)
        merged -= b->prev_bytes;
      arena_release(m_objects_arena, p + sizeof(block));
      p = merged + ((block *)merged)->bytes;
    } else {
      p += b->bytes;
    }
  }
}

template <> bool KERNEL_OBJECT::destroy_poll() {
  static_assert(THREAD_COUNT <= 32);
  if (kernel_instance.current_thread_id() != ARCH::load_acquire32(&m_destroy_worker)) return false;
  const uint32_t target = ARCH::load_acquire32(&m_destroy_target);
  if (target == NO_OBJECT) return false;
  if (!kernel_instance.pause_tasks(1000)) return false;
  // Other cores have reached PLAIN execution with their interrupts masked.
  // No bookkeeping lock, handler frame or grant transition is in flight.
  table_lock();
  auto defer = [&]() { table_unlock(); kernel_instance.resume_tasks(); return false; };
  // A handler may have been finishing a CREATE when closing was published.
  for (uint32_t i = 0; i < OBJECT_COUNT; ++i)
    if (i != target && m_objects[i].created &&
        m_objects[i].parent_handler_object == target) return defer();
  uint32_t victims = 0;
  for (uint32_t t = 0; t < THREAD_COUNT; ++t) {
    if (kernel_instance.thread_state(t) == KERNEL::THREAD::state_t::UNINITIALIZED)
      continue;
    bool uses = m_thread_object[t] == target;
    const auto &shadow = m_shadow[t];
    for (uint32_t d = 0; d < shadow.depth; ++d)
      uses |= shadow.object[d] == target || shadow.caller[d] == target;
    if (!uses) continue;
    if (kernel_instance.thread_debug_protected(t)) return defer();
    victims |= 1u << t;
  }
  // Retire execution before invoking a hook that can revoke code or callbacks.
  // Pin stacks across deferred DMA/hook completion so schedule() cannot collect
  // them while external users still hold pointers into the object's resources.
  if (!kernel_instance.destroy_contexts(victims, m_destroy_fallback)) return defer();
  ARCH::store_release32(&m_destroy_victims, victims);
  // Drop DMA links before reclaiming descriptors or backing buffers. Hardware
  // can finish while CPUs are parked; a busy link defers to a later iteration.
  auto owns_address = [&](uintptr_t address) {
    for (uintptr_t p = m_objects_arena.base;
         p < m_objects_arena.base + m_objects_arena.bytes;) {
      const auto *b = (const block *)p;
      if (b->used && b->owner == target && address >= p + sizeof(block) &&
          address < p + b->bytes) return true;
      p += b->bytes;
    }
    return false;
  };
  bool remove[STREAM_COUNT]{};
  bool affected[STREAM_COUNT]{};
  for (uint32_t s = 0; s < STREAM_COUNT; ++s) {
    auto *desc = m_streams[s];
    remove[s] = desc && (m_stream_owner[s] == target ||
                        owns_address((uintptr_t)desc) || owns_address((uintptr_t)desc->base));
    affected[s] = desc && (remove[s] ||
                           desc->producer == target || desc->consumer == target);
  }
  for (auto &link : m_connections)
    if (link.active != CONNECTION_FREE && (affected[link.src] || affected[link.dst]))
      ARCH::store_release32(&link.active, CONNECTION_CLOSING);
  pump_connections();
  for (const auto &link : m_connections)
    if (link.active != CONNECTION_FREE && (affected[link.src] || affected[link.dst]))
      return defer();
  if (!m_destroy_hooks[target](target)) return defer();
  // No saved CPU context now refers to a victim's stack. The hook has certified
  // that external users will not resume with raw pointers into reclaimed memory.
  for (uint32_t s = 0; s < STREAM_COUNT; ++s) {
    auto *desc = m_streams[s];
    if (!desc) continue;
    if (remove[s]) {
      m_streams[s] = nullptr;
      m_stream_owner[s] = NO_OBJECT;
    } else {
      if (desc->producer == target) desc->producer = stream::NO_OWNER;
      if (desc->consumer == target) desc->consumer = stream::NO_OWNER;
    }
  }
  for (uint32_t t = 0; t < THREAD_COUNT; ++t) {
    if (!(victims & (1u << t))) continue;
    if (m_thread_stack[t]) arena_release(m_objects_arena, m_thread_stack[t]);
    m_thread_stack[t] = 0;
    m_shadow[t].depth = 0;
    m_thread_object[t] = ROOT_OBJECT;
    m_wake_at[t] = 0;
    m_budget[t] = DEFAULT_BUDGET_CYCLES;
    m_kill_pending[t] = 0;
    kernel_instance.release(t);
  }
  release_object_memory(target);
  m_objects[target] = {};
  m_object_name[target] = nullptr;
  m_destroy_hooks[target] = nullptr;
  m_destroy_result[target] = 1;
  ARCH::store_release32(&m_destroy_victims, 0);
  ARCH::store_release32(&m_destroy_target, NO_OBJECT);
  table_unlock();
  kernel_instance.resume_tasks();
  return true;
}
} // namespace shizuku
