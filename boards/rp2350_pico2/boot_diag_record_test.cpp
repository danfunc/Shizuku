#include "boards/rp2350_pico2/boot_diag_record.hpp"
#include <cassert>
using namespace shizuku::boards::rp2350_boot_diag;
int main() {
  const auto boot = seal({0, 0, 32, 0, active});
  assert(valid(boot));
  assert(recover(boot, true));
  assert(!recover(boot, false)); // power-on/random RAM must not enter recovery
  auto damaged = boot;
  damaged.pc ^= 4;
  assert(!valid(damaged));
  assert(!recover(damaged, true)); // reject torn/corrupt retained state
  assert(!recover(seal({0, 0, 60, 0, complete}), true));
  assert(recover(seal({0, 0, 32, 0, scratch_only}), true));
  assert(fatal_fault(true, 0xfffffffdu)); // boot has no kernel recovery context yet
  assert(!fatal_fault(false, 0xfffffffdu)); // preserve intentional Thread/PSP fault tests
  assert(fatal_fault(false, 0xfffffff9u)); // Thread/MSP startup fault
  assert(fatal_fault(false, 0xfffffff1u)); // Handler/MSP fault
  const auto crash = seal({0, 0, 32, 0, fault, 0x20001234, 0x82});
  assert(recover(crash, true) && crash.pc == 0x20001234 && crash.cfsr == 0x82);
  assert(readable_frame(0x20000000, 0x20000000, 0x20082000, false));
  assert(readable_frame(0x20081fe0, 0x20000000, 0x20082000, false));
  assert(!readable_frame(0x20081fe4, 0x20000000, 0x20082000, false));
  assert(!readable_frame(0x20000001, 0x20000000, 0x20082000, false));
  assert(!readable_frame(0x20000000, 0x20000000, 0x20082000, true));
}
