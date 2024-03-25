#include "shizuku/kernel.hpp"

using namespace shizuku::types;

api_result<shizuku::thread_weak_ptr>
shizuku::types::kernel_api_for_super_object::get_current_thread() {
  if (wrapping_kernel.get_current_object_is_super()) {
    return wrapping_kernel.get_current_thread();
  } else {
    return error_code::you_are_not_super_object;
  }
}