#ifndef SHIZUKU_CONFIG_HPP
#define SHIZUKU_CONFIG_HPP

#include <cstdint>
#include <cstdlib>
#include <cstdarg>

#include "arch/armv8m/armv8m.hpp"
#include "shizuku/concepts/board.hpp"
#include "chips/rp2350/chip.hpp"
#include "shizuku/memory_managers/freestanding.hpp"
#include "shizuku/templates/cpu_manager.hpp"
#include "shizuku/templates/kernel.hpp"
#include "shizuku/templates/kernel_object.hpp"

namespace shizuku::boards {
class arm_legacy_host_board {
 public:
  static constexpr uint32_t CORE_COUNT = 2;
  static void init(uint32_t) {}
  static uint32_t core_num() { return 0; }
  static void launch_core(void (*)()) {}
  static void park_other_cores() {}
  static void resume_other_cores() {}
  static void diag_mute(bool) {}
  static int dma_claim() { std::abort(); }
  static void dma_copy(int, const void *, void *, uint32_t) { std::abort(); }
  static bool dma_busy(int) { std::abort(); }
  static void dma_release(int) { std::abort(); }
  static uint64_t time_us() { return 0; }
  static uint32_t cycles_per_us() { return 1; }
  static uintptr_t unprivileged_floor() { return UINTPTR_MAX; }
  [[noreturn]] static void panic(const char *) { std::abort(); }
  static void diag_printf(const char *, ...) {}
};
}  // namespace shizuku::boards

namespace shizuku::archs {
struct armv8m_legacy_host : armv8m<8, 4> {
  static constexpr bool LEGACY_FRAME_HOST_TEST = true;
  static inline uintptr_t host_stack_limit = 0;

  static bool debug_enabled() { return false; }
  static void debug_step(bool) {}
  static uint32_t debug_reason_take() { return 0; }

  static void stack_limit_set(context_t &context, uintptr_t limit) {
    host_stack_limit = limit;
    armv8m<8, 4>::stack_limit_set(context, limit);
  }
  static uintptr_t stack_limit(const context_t &) { return host_stack_limit; }
  template <class Frame>
  static void switch_callee_protection(context_t &, Frame &, uint32_t, bool) {}
  static void switch_caller_protection(uint32_t) {}
};
}  // namespace shizuku::archs

namespace shizuku {
using ARCH = archs::armv8m_legacy_host;
using CHIP = chips::rp2350::chip;
using BOARD = boards::arm_legacy_host_board;
using CPU_MANAGER = templates::cpu_manager<ARCH, CHIP, BOARD, 2>;
using MEMORY_MANAGER = memory_managers::freestanding;
using KERNEL = templates::kernel<CPU_MANAGER, MEMORY_MANAGER>;
using KERNEL_OBJECT = templates::kernel_object<KERNEL, 72, 16, 32, 32>;
}  // namespace shizuku

#endif  // SHIZUKU_CONFIG_HPP
