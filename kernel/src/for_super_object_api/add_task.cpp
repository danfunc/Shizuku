#include <shizuku/kernel.hpp>

namespace shizuku {
namespace types {
api_result<size_t>
kernel_api_for_super_object::add_task(shizuku::thread_shared_ptr &thread,
                                      size_t processing_time, int priority) {
  if (get_current_object_is_super()) {
    return wrapping_kernel.add_task(thread, processing_time, priority);
  } else {
    return error_code::you_are_not_super_object;
  }
}
} // namespace types

} // namespace shizuku