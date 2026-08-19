#ifndef SHIZUKU_ARCHS_DUMMY_HPP
#define SHIZUKU_ARCHS_DUMMY_HPP
#include <cstdint>
#include "shizuku/concepts/arch.hpp"

namespace shizuku {
namespace archs {

// concept の検査用・ホストコンパイル検査用のダミー arch。
// 実ハードでは動かない (意味を持つ値を返さない)。
class dummy {
public:
  struct exception_frame_t {
    uint32_t r0;
  };
  struct context_t {
    exception_frame_t *sp = nullptr;
  };
  using method_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                 uintptr_t);
  static uint32_t exc_frame_bytes(const context_t &) { return 0; }
  static uintptr_t psp_after_return(const context_t &) { return 0; }
  static bool current_priv() { return true; }
  static void set_priv(context_t &, bool) {}
  static void stack_limit_set(context_t &, uintptr_t) {}
  static bool cas32(volatile uint32_t *address, uint32_t expected,
                    uint32_t desired) {
    if (*address != expected)
      return false;
    *address = desired;
    return true;
  }
  static void store_release32(volatile uint32_t *address, uint32_t value) {
    *address = value;
  }
  static uint32_t load_acquire32(volatile uint32_t *address) {
    return *address;
  }
};

static_assert(shizuku::concepts::arch_requires<dummy>,
              "dummy arch must satisfy the arch concept");

} // namespace archs
} // namespace shizuku
#endif // SHIZUKU_ARCHS_DUMMY_HPP
