#include <shizuku/kernel.hpp>

namespace shizuku {
KERNEL kernel_instance;
template <> void KERNEL::init() {
  cpu_manager.init();
  if (auto result = memory_manager.init(); !result) {
    // ブート時の失敗は続行不能 (I-9 の panic 許可条件: カーネル自身の前提の破れ)。
    panic("memory_manager_uninitialized");
  };
}
} // namespace shizuku
