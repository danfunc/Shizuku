#include "shizuku/u_cost_sram_window.hpp"
#include <cassert>
#include <cstdint>

using namespace shizuku::selftest::u_cost_sram;

namespace {
constexpr range sram{0x20000000, 0x20082000};
constexpr range code{0x20000800, 0x20000860};          // 96B, 32B 境界
constexpr range p_data{0x20001000, 0x20001100};
constexpr range p_stack{0x20001100, 0x20001500};
constexpr range protected_set[] = {p_data, p_stack};
constexpr range far_data{0x20004000, 0x20004100};
constexpr range no_overlap_set[] = {far_data};
constexpr uintptr_t entries[] = {0x20000805, 0x20000808};  // Thumb bit つき

struct mock_arch {
  static constexpr uint32_t REGION_UNUSED = 1;
  static constexpr uint32_t ACCESS_RO_ALL = 3;
  static inline unsigned set_calls = 0;
  static inline unsigned disable_calls = 0;
  static inline uintptr_t set_base = 0;
  static inline uintptr_t set_limit = 0;
  static inline uint32_t set_access = 0;
  static inline bool set_xn = true;
  static void region_set(uint32_t, uintptr_t base, uintptr_t limit,
                         uint32_t access, bool xn, uint32_t) {
    ++set_calls;
    set_base = base;
    set_limit = limit;
    set_access = access;
    set_xn = xn;
  }
  static void region_disable(uint32_t) { ++disable_calls; }
};

static_assert(check_code_window(code, sram, entries, protected_set) == verdict::ok);
static_assert(denied_code_entry(code, sram, 0x20000901));
static_assert(!denied_code_entry(code, sram, 0x20000821));
static_assert(!denied_code_entry(code, sram, 0x10000901));
static_assert(!denied_code_entry(code, sram, ~uintptr_t(0)));

// 窓の形: 空・32B に揃っていない・大きすぎる
static_assert(check_code_window(range{0x20000800, 0x20000800}, sram, entries, protected_set) == verdict::empty_window);
static_assert(check_code_window(range{0x20000800, 0x20000840 - 1}, sram, entries, protected_set) == verdict::misaligned);
static_assert(check_code_window(range{0x20000810, 0x20000860}, sram, entries, protected_set) == verdict::misaligned);
static_assert(check_code_window(range{0x20000800, 0x20000800 + max_code_bytes}, sram,
                                entries, no_overlap_set) == verdict::ok);
static_assert(check_code_window(range{0x20000800, 0x20000800 + max_code_bytes + granule}, sram, entries, protected_set) == verdict::too_large);

// SRAM の外 (flash や MMIO を非特権の実行窓にしない)
static_assert(check_code_window(range{0x10000000, 0x10000060}, sram, entries, protected_set) == verdict::outside_sram);
static_assert(check_code_window(range{0x20081FE0, 0x20082020}, sram, entries, protected_set) == verdict::outside_sram);
static_assert(check_code_window(range{0xD0000000, 0xD0000060}, sram, entries, protected_set) == verdict::outside_sram);

// 関数が窓の外 (窓の外の SRAM を実行させない)
constexpr uintptr_t entry_below[] = {0x200007F9, 0x20000808};
constexpr uintptr_t entry_at_end[] = {0x20000805, 0x20000861};  // end の 1 つ先 (排他的)
static_assert(check_code_window(code, sram, entry_below, protected_set) == verdict::entry_outside_window);
static_assert(check_code_window(code, sram, entry_at_end, protected_set) == verdict::entry_outside_window);
static_assert(code_address(0x20000805) == 0x20000804);

// 窓が U の stack・データ窓に触れたら止める (PMSAv8 は重なりを許さない。RO+X が RW データを覆わない)
static_assert(check_code_window(range{0x20000FC0, 0x20001020}, sram, entries, protected_set) == verdict::entry_outside_window);
constexpr uintptr_t entries_over[] = {0x20000FC1, 0x20000FE0};
static_assert(check_code_window(range{0x20000FC0, 0x20001020}, sram, entries_over, protected_set) == verdict::overlaps_protected);
constexpr range abutting_data{0x20000860, 0x20000900};  // 窓の直後から始まるデータ: 重ならない
constexpr range abutting_set[] = {abutting_data};
static_assert(check_code_window(code, sram, entries, abutting_set) == verdict::ok);
constexpr range inside_data{0x20000840, 0x20000900};
constexpr range inside_set[] = {inside_data};
static_assert(check_code_window(code, sram, entries, inside_set) == verdict::overlaps_protected);

// MPU に渡す limit は最後の 32B ブロックを指す (下位 5bit は region_set が落とす)
static_assert((region_limit(code) & ~uintptr_t(0x1F)) == 0x20000840);
static_assert(overlaps(range{0, 32}, range{31, 64}) && !overlaps(range{0, 32}, range{32, 64}));
}  // namespace

int main() {
  // 実行時にも同じ判定が通る (static_assert の最適化落ちを避ける)
  assert(check_code_window(code, sram, entries, protected_set) == verdict::ok);
  assert(check_code_window(code, sram, entry_below, protected_set) == verdict::entry_outside_window);
  assert(check_code_window(range{0x10000000, 0x10000060}, sram, entries, protected_set) == verdict::outside_sram);
  assert(denied_code_entry(code, sram, 0x20000901));
  assert(!denied_code_entry(code, sram, 0x20000821));
  mock_arch::set_calls = mock_arch::disable_calls = 0;
  const auto noop_sync = [] {};
  {
    scoped_code_window<mock_arch, decltype(noop_sync)> scope(code, noop_sync);
    assert(mock_arch::set_calls == 1);
    assert(mock_arch::disable_calls == 0);
    assert(mock_arch::set_base == code.begin);
    assert(mock_arch::set_limit == code.end - 1);
    assert(mock_arch::set_access == mock_arch::ACCESS_RO_ALL);
    assert(!mock_arch::set_xn);
  }
  assert(mock_arch::disable_calls == 1);
  return 0;
}
