#ifndef SHIZUKU_ARCHS_DUMMY_HPP
#define SHIZUKU_ARCHS_DUMMY_HPP
#include <cstdint>
#include "shizuku/concepts/arch.hpp"

namespace shizuku {
namespace archs {

// concept の検査用・ホストコンパイル検査用のダミー arch。
// 実ハードでは動かない (意味を持つ値を返さない)。concept を増やしたときに
// 「要件を足したが実装を忘れた」を最短で検出するための当て板。
class dummy {
public:
  struct exception_frame_t {
    uintptr_t slot[8];
  };
  struct context_t {
    exception_frame_t *sp = nullptr;
  };
  using method_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                 uintptr_t);
  struct syscall_result {
    uintptr_t error;
    uintptr_t value;
  };
  static constexpr uint32_t CALL_HEADROOM = 0;

  static uint32_t exc_frame_bytes(const context_t &) {
    return (uint32_t)sizeof(exception_frame_t);
  }
  static uintptr_t psp_after_return(const context_t &context) {
    return (uintptr_t)context.sp + sizeof(exception_frame_t);
  }
  static void normalize_frame(exception_frame_t &) {}
  static uintptr_t arg(const exception_frame_t &frame, unsigned index) {
    return frame.slot[index < 8 ? index : 0];
  }
  static void set_args(exception_frame_t &frame, const uintptr_t *args) {
    for (unsigned i = 0; i < 4; ++i)
      frame.slot[i] = args[i];
  }
  static void set_result(exception_frame_t &frame, uintptr_t error,
                         uintptr_t value) {
    frame.slot[0] = error;
    frame.slot[1] = value;
  }
  static void set_entry(exception_frame_t &frame, uintptr_t pc, uintptr_t lr) {
    frame.slot[6] = pc;
    frame.slot[5] = lr;
  }
  static void set_handler_info(context_t &, uintptr_t, uintptr_t) {}
  static uintptr_t return_stub() { return 0; }
  template <uintptr_t NUMBER> static uintptr_t object_exit_stub() { return 0; }
  template <auto FUNCTION> static uintptr_t handler_entry() { return 0; }
  static bool current_priv() { return true; }
  static void set_priv(context_t &, bool) {}
  static void stack_limit_set(context_t &, uintptr_t) {}
  static uintptr_t stack_limit(const context_t &) { return 0; }
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
  static syscall_result syscall(uintptr_t, uintptr_t = 0, uintptr_t = 0,
                                uintptr_t = 0, uintptr_t = 0) {
    return {0, 0};
  }
  [[noreturn]] static void enter_thread_mode(uintptr_t, uintptr_t,
                                             void (*)()) {
    while (true) {
    }
  }
};

static_assert(shizuku::concepts::arch_requires<dummy>,
              "dummy arch must satisfy the arch concept");

} // namespace archs
} // namespace shizuku
#endif // SHIZUKU_ARCHS_DUMMY_HPP
