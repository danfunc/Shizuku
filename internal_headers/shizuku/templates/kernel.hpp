#ifndef SHIZUKU_KERNEL_HPP
#define SHIZUKU_KERNEL_HPP
#include "shizuku/templates/table.hpp"
namespace shizuku {
namespace templates {
template <typename CPU_MANAGER_T, typename MEMORY_MANAGER_T, typename OBJECT_T,
          void (*SYSTEM_OBJECT_ENTRY)()>
class kernel {
public:
  using CPU_MANAGER = CPU_MANAGER_T;
  using MEMORY_MANAGER = MEMORY_MANAGER_T;
  using OBJECT = OBJECT_T;
  using THREAD = OBJECT::THREAD;
  CPU_MANAGER cpu_manager;
  MEMORY_MANAGER memory_manager;
  static_table<OBJECT *, 128> object_table;


  void init();
};
} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_KERNEL_HPP