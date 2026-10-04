#ifndef SHIZUKU_CPU_MANAGER_HPP
#define SHIZUKU_CPU_MANAGER_HPP
#include "cstdint"
#include "shizuku/concepts/arch.hpp"
#include "shizuku/concepts/board.hpp"
#include "shizuku/templates/task.hpp"
namespace shizuku {
namespace templates {

// コアの初期化とコア・実行優先度ごとの task 台帳。arch / board を型で受ける
// (docs/03_porting_policy.md D2)。スレッドと実行文脈はカーネル本体が持つ。
template <typename ARCH_T, typename BOARD_T, uintptr_t CORE_COUNT_T,
          uintptr_t TASK_PRIORITY_COUNT_T = 1>
  requires shizuku::concepts::arch_requires<ARCH_T> &&
           shizuku::concepts::board_requires<BOARD_T>
class cpu_manager {
public:
  using ARCH = ARCH_T;
  using BOARD = BOARD_T;
  static constexpr uintptr_t CORE_COUNT = CORE_COUNT_T;
  static constexpr uintptr_t TASK_PRIORITY_COUNT = TASK_PRIORITY_COUNT_T;
  using TASK = task;
  // Bootstrap only, before secondary cores are launched.
  void reset_tasks() { m_tasks = {}; }
  // Existing Cortex-M ports run one thread-mode execution lane per core.
  // A multi-lane port must select the lane AND save/restore its hardware timer
  // on priority entry/exit; merely allocating more slots does not enable it.
  TASK &execution_task(uint32_t core, uint32_t priority = 0) {
    return m_tasks.at(core, priority);
  }
  const TASK &execution_task(uint32_t core, uint32_t priority = 0) const {
    return m_tasks.at(core, priority);
  }
  // 呼び出したコアの例外結線・優先度を初期化する。優先度レジスタ等は per-core
  // banked なので、他コアの初期化はそのコア自身の起動経路で行うこと。
  void init();
private:
  task_table<CORE_COUNT, TASK_PRIORITY_COUNT> m_tasks;
};

} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_CPU_MANAGER_HPP
