
#ifndef SHIZUKU_KERNEL_HPP
#define SHIZUKU_KERNEL_HPP
#include "shizuku/config.hpp"
#include "shizuku/namespaces.hpp"
#include "shizuku/object.hpp"
namespace shizuku {
namespace types {
class kernel {
private:
  shizuku::cpu_driver cpu_driver[shizuku::processor_count];

public:
  kernel(/* args */);
  ~kernel();
  void context_switch(void);
};

} // namespace types
extern types::kernel kernel;
} // namespace shizuku

#endif