#ifndef SHIZUKU_BOARDS_RP2040_PICO_W_HPP
#define SHIZUKU_BOARDS_RP2040_PICO_W_HPP
#include <cstdint>
#include "chips/rp2040/chip.hpp"
#include "shizuku/concepts/board.hpp"
namespace shizuku::boards {
class rp2040_pico_w {
public:
  static constexpr uint32_t CORE_COUNT=shizuku::chips::rp2040::chip::kCoreCount;
  static void init(uint32_t); static void protection_init(); static void launch_core(void(*)());
  static void park_other_cores(); static void resume_other_cores(); static void diag_mute(bool);
  static int dma_claim(); static void dma_copy(int,const void*,void*,uint32_t); static bool dma_busy(int); static void dma_release(int);
  static uint32_t core_num(){return shizuku::chips::rp2040::chip::core_id();} static uint64_t time_us();
  static uint32_t cycles_per_us(); static uintptr_t unprivileged_floor();
  static void diag_printf(const char*,...) __attribute__((format(printf,1,2)));
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  static void boot_trace_stage(uint32_t);
#endif
  [[noreturn]] static void panic(const char*);
};
static_assert(shizuku::concepts::board_requires<rp2040_pico_w>);
}
#endif
