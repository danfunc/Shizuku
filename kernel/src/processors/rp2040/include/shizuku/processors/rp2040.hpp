#ifndef RP2040_HPP
#define RP2040_HPP
#ifdef SIO_CPUID_OFFSET
#define BEFORE_CPUID_OFFSET SIO_CPUID_OFFSET
#undef SIO_CPUID_OFFSET
#endif
#ifdef SIO_BASE
#define BEFORE_SIO_BASE SIO_BASE
#undef SIO_BASE
#endif
#ifndef SIO_BASE
#define SIO_BASE (0xd0000000) // imported from pico_sdk
#endif
#ifndef SIO_CPUID_OFFSET
#define SIO_CPUID_OFFSET (0x00000000) // imported from pico_sdk
#endif

#include "cstdint"
#include "cstdlib"
#include "initializer_list"
#include "shizuku/platform.hpp"
namespace shizuku {
namespace types {
struct object;
namespace processors {
namespace rp2040 {
struct context;
class cpu_driver;
} // namespace rp2040
} // namespace processors
} // namespace types
} // namespace shizuku

class shizuku::types::processors::rp2040::cpu_driver {
public:
  static int
  load_context(const shizuku::types::processors::rp2040::context *context,
               const int return_value = 1);
  static int save_context(shizuku::types::processors::rp2040::context *context);
  static void entry_func(int (*entry)(int argc, char *argv[]),
                         shizuku::types::processors::rp2040::context *context,
                         int argc, char *argv[]);
  static inline unsigned int get_core_num() {
    return (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET));
  };
};
constexpr size_t size = 4 * 1024;

struct shizuku::types::processors::rp2040::context {
  void auto_exit_request();
  uint32_t r4, r5, r6, r7, r8, r9, r10, r11, r12;
  void *sp, *lr;
  void *const stack_start_p;
  uint32_t r0, r1, r2, r3;
  inline context(int (*entry)(size_t, size_t, size_t, size_t), size_t arg1,
                 size_t arg2, size_t arg3, size_t arg4)
      : stack_start_p{
            (void *)((int)((uint32_t)((int)malloc(size) + size)) & ~0xfL)} {
    if (::shizuku::types::processors::rp2040::cpu_driver::save_context(this) ==
        0) {
      r4 = (uint32_t)entry;
      r5 = (uint32_t)arg1;
      r6 = (uint32_t)arg2;
      r7 = (uint32_t)arg3;
      r8 = (uint32_t)arg4;
      sp = stack_start_p;
      return;
    } else {
      asm volatile("mov r0,r5");
      asm volatile("mov r1,r6");
      asm volatile("mov r2,r7");
      asm volatile("mov r3,r8");
      // 引数を設定
      asm volatile("mov r7,#0");
      asm volatile("mov r9,r7");
      asm volatile("mov r10,r7");
      asm volatile("mov r11,r7");
      asm volatile("mov r12,r7");
      asm volatile("blx r4");
      while (1) {
        printf("context_exited");
      }
    }
  };
  context() : stack_start_p{nullptr} {};
  ~context() {
    if (stack_start_p != nullptr) {
      free(stack_start_p);
    }
  }
};

#undef SIO_BASE
#undef SIO_CPUID_OFFSET

#endif