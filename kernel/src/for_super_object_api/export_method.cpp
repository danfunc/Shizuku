#include "shizuku/kernel.hpp"

namespace shizuku {
namespace types {
api_result<void>
kernel_api_for_super_object::export_method(size_t target_object_id,
                                           shizuku::types::method method,
                                           shizuku::string name) {
  if (this->get_current_object_is_super()) {
    return wrapping_kernel.export_method(target_object_id, method, name);
  } else {
    return error_code::you_are_not_super_object;
  }
}
} // namespace types
} // namespace shizuku