
#ifndef SHIZUKU_KERNEL_HPP
#define SHIZUKU_KERNEL_HPP
#include "shizuku/config.hpp"
#include "shizuku/namespaces.hpp"
namespace shizuku {
namespace types {
class kernel {
private:
  shizuku::cpu_driver cpu_driver;
  
public:
  kernel(/* args */);
  ~kernel();
  void context_switch(void);
};

kernel::kernel(/* args */) {}

kernel::~kernel() {}

} // namespace types

} // namespace shizuku

#endif