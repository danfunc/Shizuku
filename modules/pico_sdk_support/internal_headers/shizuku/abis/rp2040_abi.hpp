#ifndef SHIZUKU_RP2040_ABI_HPP
#define SHIZUKU_RP2040_ABI_HPP
#include "shizuku/templates/result.hpp"
#include <cstdint>
namespace shizuku {
namespace abis {
class rp2040 {
public:
  using method_t = uintptr_t (*)(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2,
                                 uintptr_t arg3, uintptr_t r12);

  template <shizuku::templates::result<void *> (*func)(
      uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
      uintptr_t r12, uintptr_t syscall_num)>
  void svc_abi_callee_converter(void) {
    uintptr_t arg0, arg1, arg2, arg3, r12;
    asm volatile("mov %[arg0],r0;"
                 "mov %[arg1],r1;"
                 "mov %[arg2],r2;"
                 "mov %[arg3],r3;"
                 "mov %[r12],r12;"
                 : [arg0] "=r"(arg0), [arg1] "=r"(arg1), [arg2] "=r"(arg2),
                   [arg3] "=r"(arg3), [r12] "=r"(r12)
                 :
                 :);
  };

  template <uintptr_t sys_call_num>
    requires(sys_call_num <=
             255) // SVC instruction immediate must be under 256.
  static shizuku::templates::result<void *>
  svc_abi_caller_converter(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2,
                           uintptr_t arg3, uintptr_t r12 = 0) {
    shizuku::result_code return_code;
    void *return_addr;
    asm volatile(
        "mov r0,%[arg0];"
        "mov r1,%[arg1];"
        "mov r2,%[arg2];"
        "mov r3,%[arg3];"
        "mov r12,%[r12];"
        "svc %[sys_call_num];"
        "mov %[return_code],r0;"
        "mov %[return_addr],r1;"
        : [return_code] "=r"(return_code), [return_addr] "=r"(return_addr)
        : [arg0] "r"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2),
          [arg3] "r"(arg3), [r12] "r"(r12), [sys_call_num] "i"(sys_call_num)
        : "r0", "r1", "r2", "r3", "ip", "lr", "memory");
    if (return_code == success) {
      return {return_addr};
    } else {
      return {return_code, (const char *)return_addr};
    }
  };
};

enum svc_number : uint8_t {
  INIT_INVOKE = 255,
};

}; // namespace abis
}; // namespace shizuku
#endif // SHIZUKU_RP2040_ABI_HPP