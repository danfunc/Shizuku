#include "shizuku/processors/rp2040.hpp"

namespace shizuku {
types::object *types::processors::rp2040::cpu_driver::get_current_object() {
  return current_task.object;
}
} // namespace shizuku