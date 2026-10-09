#include <cstdint>
#include "boards/esp32_c5/esp32_c5.hpp"
#include "chips/esp32_c5/systimer.hpp"
#include "chips/esp32_c5/trap_api.hpp"

extern "C" {
extern char _bss_end;
}
// trap/timer の SoC 側 API (chips/esp32_c5/trap_api.hpp) を、元の名前のまま使う。
using namespace shizuku::chips::esp32_c5;
namespace c5 = shizuku::ports::c5;

namespace shizuku::boards {
void esp32_c5::init(uint32_t core) {
  if (core != 0) stop(0xE007);
  install_traps();
  interrupt_init();
}
uint64_t esp32_c5::time_us() { return c5::us_from_ticks(systimer_now()); }
uint32_t esp32_c5::cycles_per_us() { return c5::TICKS_PER_US; }
uintptr_t esp32_c5::unprivileged_floor() {
  // Stage-2 privileged-only pilot: BSS bookkeeping is below this boundary.
  // This is not a PMP security boundary. U-mode return is refused above.
  return reinterpret_cast<uintptr_t>(&_bss_end);
}
[[noreturn]] void esp32_c5::panic(const char *) { stop(0xE008); }
} // namespace shizuku::boards
