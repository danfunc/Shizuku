#include "shizuku/armv8m_abi.hpp"
extern "C" __attribute__((noinline)) uintptr_t r7_alloca_probe(uintptr_t n, uintptr_t a4) {
  volatile unsigned char *p = static_cast<volatile unsigned char *>(__builtin_alloca((n & 63u) + 16u));
  p[0] = 37;
  auto r = shizuku::armv8m_abi::syscall(4, 7, 0, 19, a4);
  return r.value + p[0];
}
extern "C" __attribute__((noinline)) uintptr_t r7_vla_probe(uintptr_t n, uintptr_t a4) {
  volatile unsigned char v[(n & 63u) + 16u];
  v[0] = 41;
  auto r = shizuku::armv8m_abi::syscall(4, 8, 0, 23, a4);
  return r.value + v[0];
}
