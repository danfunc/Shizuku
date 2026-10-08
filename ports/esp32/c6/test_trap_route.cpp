#include <cassert>
#include <initializer_list>

#define SHIZUKU_C6_SKELETON_PERMITTED 1
#include "internal_headers/shizuku/archs/rv32_c6.hpp"
#include "ports/esp32/c6/trap_route.hpp"
using namespace shizuku::ports::c6;
using frame_t = shizuku::archs::rv32_c6::exception_frame_t;
int main() {
  volatile uint32_t depth = 0;
  unsigned ecalls = 0, irqs = 0, faults = 0;
  for (uint32_t cause : {8u, 11u, 2u, 0x8000000bu, 0x80000008u}) {
    frame_t f{}; f.mcause = cause; f.mepc = 0x1000; f.mtval = 0x123456;
    route_trap(f, depth, [&] { ++ecalls; assert(depth == 0); },
      [&] { ++irqs; assert(depth == 1); },
      [&] { ++faults; assert(depth == 0); });
    assert(f.mepc == (cause == 8 || cause == 11 ? 0x1004u : 0x1000u));
    assert(f.mcause == cause && f.mtval == 0x123456);
    assert(depth == 0);
  }
  assert(ecalls == 2 && irqs == 2 && faults == 1);
  frame_t outer{}; outer.mcause = 0x80000001;
  route_trap(outer, depth, [] { assert(false); }, [&] {
    assert(depth == 1);
    frame_t inner{}; inner.mcause = 0x80000002;
    route_trap(inner, depth, [] { assert(false); }, [&] { assert(depth == 2); }, [] { assert(false); });
    assert(depth == 1);
    inner.mcause = 11;
    route_trap(inner, depth, [&] { assert(depth == 1); }, [] { assert(false); }, [] { assert(false); });
    inner.mcause = 5;
    route_trap(inner, depth, [] { assert(false); }, [] { assert(false); }, [&] { assert(depth == 1); });
    assert(depth == 1);
  }, [] { assert(false); });
  assert(depth == 0);
}
