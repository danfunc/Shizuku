#include "shizuku/kernel.hpp"

shizuku::thread_weak_ptr
shizuku::types::kernel_api_for_super_object::get_current_thread() {
  if (wrapping_kernel.get_current_object_is_super()) {
    return wrapping_kernel.get_current_thread();
  } else {
    return shizuku::thread_weak_ptr();
  }
}