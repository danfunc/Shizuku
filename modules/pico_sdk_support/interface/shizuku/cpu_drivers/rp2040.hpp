#ifndef SHIZUKU_CPUDRIVER_RP2040_HPP
#define SHIZUKU_CPUDRIVER_RP2040_HPP
#include "initializer_list"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif
uint32_t rp2040_syscall(uint32_t arg0, uint32_t arg1, uint32_t arg2,
                        uint32_t arg3);
#ifdef __cplusplus
}
#endif

namespace shizuku {
namespace cpu_drivers {
class rp2040 {
public:
  void init();
  static unsigned int get_core_num() { return get_core_num(); }
  struct context {
    const void* stack_start;
    void *sp, (*lr)(void), (*pc)(void);
    
  };
  using method = uint32_t (*)(uint32_t arg0, uint32_t arg1, uint32_t arg2,
                              uint32_t arg3);

  __always_inline static uint32_t syscall(uint32_t arg0, uint32_t arg1,
                                          uint32_t arg2, uint32_t arg3) {
    return rp2040_syscall(arg0, arg1, arg2, arg3);
  };
  int corenum;
private:
  static uint32_t svc_handler(uint32_t arg0, uint32_t arg1, uint32_t arg2,
                              uint32_t arg3);
};
} // namespace cpu_drivers
} // namespace shizuku

#endif // SHIZUKU_CPUDRIVER_RP2040_HPP