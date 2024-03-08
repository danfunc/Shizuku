#include <shizuku/kernel.hpp>

namespace shizuku {
namespace types {
void kernel_api_for_super_object::add_task(shizuku::thread_shared_ptr &thread,
                                           size_t processing_time,
                                           int priority) {
  if (get_current_object_is_super()) {
    wrapping_kernel.add_task(thread, processing_time, priority);
  }
}
} // namespace types

} // namespace shizuku