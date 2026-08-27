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
  m_step_target = NO_STEP_TARGET;
  m_object_svc_handler = 0;
  m_recovery_thread = 0;
  m_faults = {};
  m_debug = {};
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
  thread.current_object = 1; // ROOT_OBJECT (1)
  thread.set_state(THREAD::state_t::RUNNING);
  ARCH::stack_limit_set(*thread.context, limit);
  ARCH::set_priv(*thread.context, true);
  m_current[BOARD::core_num()] = 0;

  // スレッドスタックへ移って entry を呼ぶ (戻らない)。以後このスレッドは
  // 「フレームを 1 枚も積んでいない」= ハンドラの枠の外なので、撃った svc は
  // 他のオブジェクトと同じくオブジェクトランドのハンドラへ届く。
  ARCH::enter_thread_mode(top, limit, entry);
}

// 2 本目以降のコアが自分で呼ぶ。★このコアの例外結線と保護を**自分で**張ってから
// スレッドモードへ移る (優先度・MPU・SysTick は per-core banked)。
template <>
void KERNEL::bootstrap_secondary(uint32_t thread, void (*entry)(),
                                 uintptr_t stack_base, uintptr_t stack_bytes) {
  if (m_threads == nullptr || thread >= m_thread_count)
    BOARD::panic("secondary core has no thread to run");
  if (stack_base == 0 || stack_bytes < 256)
    BOARD::panic("secondary core stack not provided");
  cpu_manager.init(); // このコアの優先度・MPU・SysTick
  const uintptr_t top = (stack_base + stack_bytes) & ~(uintptr_t)7;
  const uintptr_t limit = (stack_base + 7) & ~(uintptr_t)7;

  THREAD &adopted = m_threads[thread].thread;
  adopted.context = &m_threads[thread].context;
  adopted.call_stack = {};
  adopted.current_object = 1; // ROOT_OBJECT (1)
  // ★このコアでしか走らせない。他コアが拾うと、今この CPU が走らせている文脈を
  //   別コアが同時に走らせることになる (claim の CAS は「READY を取る」ための
  //   ものであって、既に走っているものは守れない)。
  adopted.affinity = 1u << BOARD::core_num();
  adopted.set_state(THREAD::state_t::RUNNING);
  ARCH::stack_limit_set(*adopted.context, limit);
  ARCH::set_priv(*adopted.context, true);
  m_current[BOARD::core_num()] = thread;

  ARCH::enter_thread_mode(top, limit, entry);
}

} // namespace shizuku
