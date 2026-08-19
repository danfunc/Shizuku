#ifndef SHIZUKU_CPU_MANAGER_HPP
#define SHIZUKU_CPU_MANAGER_HPP
#include "cstdint"
#include "shizuku/concepts/arch.hpp"
#include "shizuku/concepts/board.hpp"
namespace shizuku {
namespace templates {

// コアと実行文脈の管理。arch (ISA) と board を型パラメータで受ける
// (docs/03_porting_policy.md D2)。Phase 2 でスレッド表・現在スレッド管理・
// 実行権委譲がここに乗る。
template <typename ARCH_T, typename BOARD_T, uintptr_t CORE_COUNT_T,
          typename THREAD_T>
  requires shizuku::concepts::arch_requires<ARCH_T> &&
           shizuku::concepts::board_requires<BOARD_T>
class cpu_manager {
public:
  using ARCH = ARCH_T;
  using BOARD = BOARD_T;
  using THREAD = THREAD_T;
  static constexpr uintptr_t CORE_COUNT = CORE_COUNT_T;
  // 呼び出したコアの例外結線・優先度を初期化する。優先度レジスタ等は per-core
  // banked なので、他コアの初期化はそのコア自身の起動経路で行うこと。
  void init();
};

} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_CPU_MANAGER_HPP
