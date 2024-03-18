#include "shizuku/kernel.hpp"

namespace shizuku {
namespace types {
bool kernel_api_for_super_object::get_current_object_is_super() {
  return wrapping_kernel.get_current_object_is_super();
}
} // namespace types

} // namespace shizuku
