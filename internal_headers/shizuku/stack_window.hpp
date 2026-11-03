#ifndef SHIZUKU_STACK_WINDOW_HPP
#define SHIZUKU_STACK_WINDOW_HPP

#include <cstdint>

namespace shizuku {

// Stack window constants
static constexpr uintptr_t kStackWindowStep = 256;
static constexpr uintptr_t kStackWindowMin = 256;

// Stack window range [lo, hi)
struct stack_window {
  uintptr_t lo = 0;
  uintptr_t hi = 0;

  constexpr bool is_valid() const noexcept {
    return lo < hi;
  }
  constexpr bool empty() const noexcept {
    return lo >= hi;
  }
  constexpr uintptr_t size() const noexcept {
    return (hi > lo) ? (hi - lo) : 0;
  }
};

// Open initial window [max(hi - K, stack_lo), hi)
// hi = caller_sp rounded down to 16B alignment.
// If room is insufficient (< kStackWindowMin or hi <= stack_lo), returns invalid {0, 0}.
constexpr stack_window open_window(uintptr_t caller_sp, uintptr_t stack_lo,
                                   uintptr_t min_bytes = kStackWindowMin) noexcept {
  const uintptr_t hi = caller_sp & ~uintptr_t(15);
  if (hi <= stack_lo || (hi - stack_lo) < min_bytes) {
    return {0, 0};
  }
  const uintptr_t lo = ((hi - stack_lo) < kStackWindowStep) ? stack_lo : (hi - kStackWindowStep);
  return {lo, hi};
}

// Extend existing window when access fault occurs at fault_addr (mtval = a).
// Permitted only when stack_lo <= fault_addr < win.lo.
// Rejects fault_addr < stack_lo or fault_addr >= win.hi (returns invalid {0, 0}).
// new_lo = max(stack_lo, min(win.lo - K, a & ~3))
constexpr stack_window extend_window(stack_window win, uintptr_t fault_addr,
                                     uintptr_t stack_lo) noexcept {
  if (!win.is_valid() || fault_addr < stack_lo || fault_addr >= win.lo || fault_addr >= win.hi) {
    return {0, 0};
  }
  const uintptr_t step_lo = (win.lo >= kStackWindowStep) ? (win.lo - kStackWindowStep) : 0;
  const uintptr_t aligned_a = fault_addr & ~uintptr_t(3);
  const uintptr_t cand = (aligned_a < step_lo) ? aligned_a : step_lo;
  const uintptr_t new_lo = (cand < stack_lo) ? stack_lo : cand;
  return {new_lo, win.hi};
}

// Zero window memory with volatile 4-byte stores from lo to hi.
[[gnu::always_inline]] inline void zero_window(stack_window win) noexcept {
  if (win.lo >= win.hi) return;
  auto *ptr = reinterpret_cast<volatile uint32_t *>(win.lo);
  const auto *end = reinterpret_cast<volatile uint32_t *>(win.hi);
  while (ptr < end) {
    *ptr++ = 0;
  }
}

// Architecture customization hook for zeroing window.
template <class A>
[[gnu::always_inline]] inline void zero_window_of(stack_window win) noexcept {
  if constexpr (requires { A::zero_stack_window(win); }) {
    A::zero_stack_window(win);
  } else {
    zero_window(win);
  }
}

} // namespace shizuku

#endif // SHIZUKU_STACK_WINDOW_HPP
