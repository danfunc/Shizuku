#include "shizuku/cpu_drivers/rp2040.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/templates/result.hpp"
#include "shizuku/templates/table.hpp"
#include "stdio.h"

void dummy(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
           uint32_t arg4) {};

void abi_converter(void) {
  uint32_t arg0, arg1, arg2, arg3, syscall_num;
  asm("mov %[arg0], r0;"
      "mov %1, r1;"
      "mov %2, r2;"
      "mov %3, r3;"
      "mov %4,r12;"
      : [arg0]"=r"(arg0), "=r"(arg1), "=r"(arg2), "=r"(arg3), "=r"(syscall_num));
  dummy(arg0, arg1, arg2, arg3, syscall_num);
}
#include <shizuku/abis/rp2040_abi.hpp>

int main() {
  stdio_init_all();
  abi_converter();
  void *a = (void *)shizuku::abis::rp2040::abi_callee_converter<dummy>;
  ((void(*)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))a)(0,0,0,0,0);
  shizuku::kernel.init();
}