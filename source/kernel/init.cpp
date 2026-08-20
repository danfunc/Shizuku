#include "shizuku/kernel.hpp"

namespace shizuku {

// 構成で確定したカーネル実体。静的領域に置く (ヒープではない) のは、将来
// 非特権オブジェクトへ渡す MPU region から構造的に外すため — カーネル簿記が
// ヒープ側にあると region の穴あけが必要になり、PMSAv8 では実現できない。
KERNEL kernel_instance;

template <> void KERNEL::init() {
  // ★スレッド表はここでは作らない。**オブジェクトランドが用意して貸す**ものなので、
  //   set_thread_storage で渡されるまでスレッドは 1 本も存在しない。
  m_threads = nullptr;
  m_thread_count = 0;
  for (uintptr_t core = 0; core < CORE_COUNT; ++core) {
    m_current[core] = 0;
    m_armed[core] = 0;
  }
  m_object_svc_handler = 0;
  m_recovery_thread = 0;
  m_faults = {};
  cpu_manager.init();
  if (auto result = memory_manager.init(); !result)
    BOARD::panic("memory manager init failed");
}

template <> void KERNEL::bootstrap(void (*entry)(), uintptr_t stack_base,
                                   uintptr_t stack_bytes) {
  if (m_threads == nullptr)
    BOARD::panic("thread storage not provided before bootstrap");
  if (stack_base == 0 || stack_bytes < 256)
    BOARD::panic("thread0 stack not provided before bootstrap");
  const uintptr_t base = stack_base;
  const uintptr_t top = (base + stack_bytes) & ~(uintptr_t)7;
  const uintptr_t limit = (base + 7) & ~(uintptr_t)7;

  THREAD &thread = m_threads[0].thread;
  thread.context = &m_threads[0].context;
  thread.call_stack = {};
  thread.set_state(THREAD::state_t::RUNNING);
  ARCH::stack_limit_set(*thread.context, limit);
  ARCH::set_priv(*thread.context, true);
  m_current[BOARD::core_num()] = 0;

  // スレッドスタックへ移って entry を呼ぶ (戻らない)。以後このスレッドは
  // 「フレームを 1 枚も積んでいない」= ハンドラの枠の外なので、撃った svc は
  // 他のオブジェクトと同じくオブジェクトランドのハンドラへ届く。
  ARCH::enter_thread_mode(top, limit, entry);
}

} // namespace shizuku
