#include "shizuku/kernel.hpp"

namespace shizuku {

// 構成で確定したカーネル実体。静的領域に置く (ヒープではない) のは、将来
// 非特権オブジェクトへ渡す MPU region から構造的に外すため — カーネル簿記が
// ヒープ側にあると region の穴あけが必要になり、PMSAv8 では実現できない。
KERNEL kernel_instance;

template <> void KERNEL::init() {
  for (uintptr_t index = 0; index < THREAD_COUNT; ++index)
    m_threads[index].context = &m_contexts[index];
  for (uintptr_t core = 0; core < CORE_COUNT; ++core)
    m_current[core] = 0;
  m_object_svc_handler = {};
  cpu_manager.init();
  if (auto result = memory_manager.init(); !result)
    BOARD::panic("memory manager init failed");
}

template <> void KERNEL::bootstrap(uintptr_t cookie, void (*entry)()) {
  // ブート実行を「信頼された活性化 = スレッド 0」として採用する。以後この活性化が
  // カーネルプリミティブを撃てる唯一の出発点になり、ここから CALL で配った
  // protection がすべての信頼の根になる (I-2)。
  constexpr uintptr_t STACK_BYTES = 8192;
  auto allocation = memory_manager.kernel_malloc(STACK_BYTES);
  if (!allocation)
    BOARD::panic("thread0 stack allocation failed");
  const uintptr_t base = (uintptr_t)allocation.value();
  const uintptr_t top = (base + STACK_BYTES) & ~(uintptr_t)7;
  const uintptr_t limit = (base + 7) & ~(uintptr_t)7;

  THREAD &thread = m_threads[0];
  thread.cookie = cookie;
  thread.caller_cookie = 0;
  thread.trusted = true;
  thread.call_stack = {};
  thread.set_state(THREAD::state_t::RUNNING);
  ARCH::stack_limit_set(*thread.context, limit);
  ARCH::set_priv(*thread.context, true);
  m_current[BOARD::core_num()] = 0;

  // スレッドスタックへ移って entry を呼ぶ (戻らない)。
  ARCH::enter_thread_mode(top, limit, entry);
}

} // namespace shizuku
