#include "shizuku/kernel.hpp"
#include "shizuku.hpp"
#include "shizuku/check_config.hpp"
#include "shizuku/object.hpp"
namespace shizuku {
types::kernel kernel;
int kernel_init_method(size_t callee_object_id, size_t caller_object_id,
                       size_t arg1, size_t arg2) {
  while (1) {
  }
}
void context_switch() { kernel.context_switch(); }
void types::kernel::context_switch() {
  cpu_manager[types::cpu_manager::get_core_num()].context_switch();
}

} // namespace shizuku
