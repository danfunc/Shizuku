#include "shizuku/processors/rp2040.hpp"

namespace rp2040 = shizuku::types::processors::rp2040;
using context = rp2040::context;

context *rp2040::cpu_driver::get_current_context() {
  return current_task.context;
}