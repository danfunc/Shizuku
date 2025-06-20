#ifndef SHIZUKU_CPUDRIVER_RP2040_HPP
#define SHIZUKU_CPUDRIVER_RP2040_HPP
#include "initializer_list"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "shizuku/abis/rp2040_abi.hpp"

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
  struct exception_stack_t {
    uint32_t r0, r1, r2, r3, r12;
    abis::rp2040::method_t lr, pc;
    uintptr_t xPSR;
  };
  static void trap();
  struct context_t {
    const void *sp_start;
    void *sp;
    context_t(void *stack_start,abis::rp2040::method_t entry, uint32_t arg0 = 0 , uint32_t arg1 = 0,
            uint32_t arg2 =0 , uint32_t arg3=0,uint32_t r12=0)
        : sp_start(stack_start) {
          // 初期の割り込みスタックを設定している
          exception_stack_t *initial_stack_ptr = (exception_stack_t*)((uintptr_t)stack_start - sizeof(exception_stack_t));
          initial_stack_ptr->r0 = arg0;
          initial_stack_ptr->r1 = arg1;
          initial_stack_ptr->r2 = arg2;
          initial_stack_ptr->r3 = arg3;
          initial_stack_ptr->r12 = r12;
          initial_stack_ptr->pc = entry;
          initial_stack_ptr->lr = (abis::rp2040::method_t)trap;
          initial_stack_ptr->xPSR = (1 << 24); // Thumb bit set
          };
  };
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