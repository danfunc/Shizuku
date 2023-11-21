
#ifndef SHIZUKU_CPU_MANAGER_HPP
#define SHIZUKU_CPU_MANAGER_HPP
#include "set"
#include "shizuku/processor.hpp"
namespace shizuku {
namespace types {
class kernel;
class cpu_manager : shizuku::cpu_driver {
  friend shizuku::types::kernel;
  void context_switch();
  static void context_switch(shizuku::context &current, shizuku::context &next);
};
} // namespace types
} // namespace shizuku

#endif