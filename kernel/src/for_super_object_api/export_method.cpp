#include "shizuku/kernel.hpp"

namespace shizuku {
namespace types {
void kernel_api_for_super_object::export_method(size_t target_object_id,
                                                shizuku::types::method method,
                                                shizuku::string name) {
  if (this->get_current_object_is_super()) {
  }
}
} // namespace types
} // namespace shizuku