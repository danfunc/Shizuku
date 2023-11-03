#include "shizuku.hpp"
#include "shizuku/processors/rp2040.hpp"

int shizuku::types::processors::rp2040::cpu_driver::context_switch(
    context &current, context &next) {
  if (save_context(current) == 0) {
    load_context(1, next);
  }
  return 0;
}
