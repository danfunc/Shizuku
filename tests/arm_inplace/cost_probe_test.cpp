#include "shizuku/cost_probe.hpp"
#include <cassert>
#include <cstdint>

using namespace shizuku::cost_probe;

int main() {
  static_assert(bucket(0) == 0 && bucket(33) == 33);
  static_assert(bucket(34) == unknown_api && bucket(UINT32_MAX) == unknown_api);
  constexpr auto normal = difference(xip_snapshot{100, 200}, xip_snapshot{106, 210});
  static_assert(normal.valid && !normal.saturated && normal.hit == 6 &&
                normal.access == 10 && normal.miss == 4);
  static_assert(!difference(xip_snapshot{UINT32_MAX, 0}, xip_snapshot{UINT32_MAX, 1}).valid);
  static_assert(difference(xip_snapshot{0, 0}, xip_snapshot{0, UINT32_MAX}).saturated);
  static_assert(!difference(xip_snapshot{2, 3}, xip_snapshot{1, 4}).valid);
  static_assert(!difference(xip_snapshot{2, 4}, xip_snapshot{3, 3}).valid);
  static_assert(!difference(xip_snapshot{0, 0}, xip_snapshot{2, 1}).valid);
  static_assert(difference(9u, 12u).valid && difference(9u, 12u).value == 3);
  static_assert(!difference(12u, 9u).valid && !difference(0u, UINT32_MAX).valid);

  // fault classification and saturation used by the [FAULTS] counters
  static_assert(classify_cfsr(0x00000002u).memmanage && !classify_cfsr(0x00000002u).busfault);
  static_assert(classify_cfsr(0x00000100u).busfault && !classify_cfsr(0x00000100u).memmanage);
  static_assert(classify_cfsr(0x00010000u).usagefault && !classify_cfsr(0x00010000u).hardfault);
  static_assert(classify_cfsr(0x00000000u).hardfault && !classify_cfsr(0x00000000u).memmanage);
  static_assert(classify_cfsr(0x00020082u).memmanage && classify_cfsr(0x00020082u).usagefault &&
                !classify_cfsr(0x00020082u).busfault);
  static_assert(saturating_increment(7u) == 8u && saturating_increment(UINT32_MAX) == UINT32_MAX);

  routes<false>::record(0, route::direct, 4);
  routes<false>::pendsv(0);
  const auto disabled = routes<false>::snapshot(0);
  assert(disabled.direct[4] == 0 && disabled.pendsv_unknown == 0);

  const auto before = routes<true>::snapshot(0);
  routes<true>::record(0, route::direct, 4);
  routes<true>::record(0, route::generic, 33);
  routes<true>::record(0, route::generic, 1000);
  routes<true>::pendsv(0);
  routes<true>::record(1, route::direct, 4);
  const auto after = routes<true>::snapshot(0);
  assert(difference(before.direct[4], after.direct[4]).value == 1);
  assert(difference(before.generic[33], after.generic[33]).value == 1);
  assert(difference(before.generic[unknown_api], after.generic[unknown_api]).value == 1);
  assert(difference(before.pendsv_unknown, after.pendsv_unknown).value == 1);
  assert(after.direct[33] == 0 && after.generic[4] == 0);
  assert(routes<true>::snapshot(1).direct[4] == 1);
  routes<true>::core[0].direct[4] = UINT32_MAX;
  routes<true>::record(0, route::direct, 4);
  assert(routes<true>::snapshot(0).direct[4] == UINT32_MAX);
  assert(!difference(after.direct[4], routes<true>::snapshot(0).direct[4]).valid);
}
