#ifndef RP2040_HPP
#define RP2040_HPP
#ifdef SIO_CPUID_OFFSET
#undef SIO_CPUID_OFFSET
#endif
#ifdef SIO_BASE
#undef SIO_BASE
#endif
#define SIO_BASE (0xd0000000)         // imported from pico_sdk
#define SIO_CPUID_OFFSET (0x00000000) // imported from pico_sdk

#include "cstdint"
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

struct shizuku::types::processors::rp2040::context {

  uint32_t r4, r5, r6, r7, r8, r9, r10, r11, r12;
  void *sp, *lr;
};

class shizuku::types::processors::rp2040::cpu_driver {
public:
  static int
  load_context(const shizuku::types::processors::rp2040::context *context,
               const int return_value = 1);
  static int save_context(shizuku::types::processors::rp2040::context *context);
  static void entry_func(int (*entry)(int argc, char *argv[]),
                         shizuku::types::processors::rp2040::context *context,
                         int argc, char *argv[]);
  static unsigned int get_core_num() {
    return (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET));
  };
};

#endif