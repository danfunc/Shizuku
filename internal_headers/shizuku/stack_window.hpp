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

// Check if caller SP is inside the current active window [cur_lo, cur_hi].
// Both endpoints are inclusive (cur_lo <= sp <= cur_hi).
// If cur_lo == 0 and cur_hi == 0, returns false.
constexpr bool window_contains_sp(uintptr_t cur_lo, uintptr_t cur_hi,
                                  uintptr_t sp) noexcept {
  if (cur_lo == 0 && cur_hi == 0) {
    return false;
  }
  return cur_lo <= cur_hi && sp >= cur_lo && sp <= cur_hi;
}

// Check if stream request [address, address + bytes) is strictly inside stack window [win_lo, win_hi).
// Address space overflow, empty window, and bytes == 0 are safely rejected.
constexpr bool stream_range_in_window(uintptr_t win_lo, uintptr_t win_hi,
                                      uintptr_t address, uintptr_t bytes) noexcept {
  if (bytes == 0 || address > UINTPTR_MAX - bytes || win_lo >= win_hi) {
    return false;
  }
  return address >= win_lo && (address + bytes) <= win_hi;
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

// RV32 向けのゼロ化: [lo, hi) を 32B (8 語) 単位に展開した sw zero で埋め、分岐は 32B に 1 回。
// 端数 (<32B) は 1 語ずつ。lo, hi は 4B 整列 (hi は 16B 切下げ、lo は hi-K か stack_lo か 4B 切下げの伸長先)。
// asm volatile + memory clobber なので store は消えず並べ替えもされない。RISC-V 以外 (host) は
// 同じ範囲を volatile 4B store で埋める。
[[gnu::always_inline]] inline void zero_window_unrolled32(stack_window win) noexcept {
  if (win.lo >= win.hi) return;
  uintptr_t p = win.lo;
  const uintptr_t hi = win.hi;
#if defined(__riscv)
  const uintptr_t bulk_end = p + ((hi - p) & ~uintptr_t(31));
  if (p < bulk_end) {
    asm volatile(
        "1:\n"
        "  sw zero, 0(%0)\n  sw zero, 4(%0)\n  sw zero, 8(%0)\n  sw zero, 12(%0)\n"
        "  sw zero, 16(%0)\n  sw zero, 20(%0)\n  sw zero, 24(%0)\n  sw zero, 28(%0)\n"
        "  addi %0, %0, 32\n"
        "  bltu %0, %1, 1b\n"
        : "+r"(p)
        : "r"(bulk_end)
        : "memory");
  }
  while (p < hi) {
    asm volatile("sw zero, 0(%0)" : : "r"(p) : "memory");
    p += 4;
  }
#else
  for (; p < hi; p += 4) *reinterpret_cast<volatile uint32_t *>(p) = 0;
#endif
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
