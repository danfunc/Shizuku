#include <cstdint>
#include "boards/esp32_c6/esp32_c6.hpp"
#include "chips/esp32_c6/systimer.hpp"
#include "chips/esp32_c6/trap_api.hpp"

extern "C" {
extern char _bss_end;
}
// trap/timer の SoC 側 API (chips/esp32_c6/trap_api.hpp) を、元の名前のまま使う。
using namespace shizuku::chips::esp32_c6;
namespace c6 = shizuku::ports::c6;

// ユーザー LED の点滅は board に置かない。特権スレッド (led_blink) が SLEEP_US で周期を作り、
// LED の GPIO は boards/esp32_c6/led_object.hpp が触る (起動時の組立は early_boot.cpp)。

namespace shizuku::boards {
void esp32_c6::init(uint32_t core) {
  if (core != 0) stop(0xE007);
  install_traps();
  interrupt_init();
}
uint64_t esp32_c6::time_us() { return c6::us_from_ticks(systimer_now()); }
bool esp32_c6::try_time_us(uint64_t &out) {
  auto read = [](uint32_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
  };
  *reinterpret_cast<volatile uint32_t *>(c6::SYSTIMER_UNIT0_OP) = c6::UNIT0_OP_UPDATE;
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    if (read(c6::SYSTIMER_UNIT0_OP) & c6::UNIT0_OP_VALUE_VALID) {
      const uint32_t lo = read(c6::SYSTIMER_UNIT0_VALUE_LO);
      const uint32_t hi = read(c6::SYSTIMER_UNIT0_VALUE_HI);
      out = c6::us_from_ticks((static_cast<uint64_t>(hi) << 32) | lo);
      return true;
    }
  }
  return false;
}
uint32_t esp32_c6::cycles_per_us() { return c6::TICKS_PER_US; }
uintptr_t esp32_c6::unprivileged_floor() {
  // Stage-2 privileged-only pilot: BSS bookkeeping is below this boundary.
  // This is not a PMP security boundary. U-mode return is refused above.
  return reinterpret_cast<uintptr_t>(&_bss_end);
}
[[noreturn]] void esp32_c6::panic(const char *) { stop(0xE008); }
} // namespace shizuku::boards
