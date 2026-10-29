#ifndef SHIZUKU_U_COST_SRAM_WINDOW_HPP
#define SHIZUKU_U_COST_SRAM_WINDOW_HPP

// RP2350 U→U 計測 (rp2350_u_cost.cpp) の U 側コードを SRAM に置くときの窓の検査。
// 窓は MPU の 32B 粒度に揃った [begin, end)。非特権に開ける SRAM はこの 1 本だけで、
// 読み+実行のみ (書込み不可)。range 検査と開閉scopeはhost試験で縛る
// (tests/arm_inplace/u_cost_sram_window_test.cpp)。

#include <cstdint>

namespace shizuku::selftest::u_cost_sram {

inline constexpr uintptr_t granule = 32;           // PMSAv8 の region 粒度
inline constexpr uintptr_t max_code_bytes = 8192;  // 試験コードが膨らんで窓が広がるのを止める

struct range {
  uintptr_t begin;
  uintptr_t end;  // 排他的
};

constexpr bool empty(range r) { return r.end <= r.begin; }
constexpr bool aligned(range r) { return r.begin % granule == 0 && r.end % granule == 0; }
constexpr bool contains(range outer, uintptr_t address) {
  return address >= outer.begin && address < outer.end;
}
constexpr bool covers(range outer, range inner) {
  return !empty(inner) && inner.begin >= outer.begin && inner.end <= outer.end;
}
constexpr bool overlaps(range a, range b) {
  return !empty(a) && !empty(b) && a.begin < b.end && b.begin < a.end;
}
// Thumb 関数のアドレスは bit0 が 1。命令の置き場所としては落として比べる。
constexpr uintptr_t code_address(uintptr_t function_address) { return function_address & ~uintptr_t(1); }
// region_set に渡す limit (内包。下位 5bit は落とされる)。
constexpr uintptr_t region_limit(range r) { return r.end - 1; }
// A negative probe target must be SRAM code outside the only executable window.
constexpr bool denied_code_entry(range code, range sram, uintptr_t function_address) {
  const uintptr_t address = code_address(function_address);
  if (address > ~uintptr_t(0) - 2) return false;
  return covers(sram, {address, address + 2}) && !contains(code, address);
}

// Keeps the RP2350 core-global executable window open only for a lexical probe interval.
template <class Arch, class Sync>
class scoped_code_window {
 public:
  explicit scoped_code_window(range code, Sync sync) : sync_(sync) {
    if constexpr (requires { Arch::REGION_UNUSED; Arch::ACCESS_RO_ALL;
                             Arch::region_set(0, 0, 0, 0, false, 0); }) {
      Arch::region_set(Arch::REGION_UNUSED, code.begin, region_limit(code),
                       Arch::ACCESS_RO_ALL, false, 0);
      sync_();
      active_ = true;
    }
  }
  scoped_code_window(const scoped_code_window &) = delete;
  scoped_code_window &operator=(const scoped_code_window &) = delete;
  ~scoped_code_window() {
    if constexpr (requires { Arch::REGION_UNUSED; Arch::region_disable(0); }) {
      if (active_) {
        Arch::region_disable(Arch::REGION_UNUSED);
        sync_();
      }
    }
  }

 private:
  bool active_ = false;
  Sync sync_;
};

enum class verdict : uint32_t {
  ok = 0,
  empty_window,
  misaligned,
  too_large,
  outside_sram,
  entry_outside_window,
  overlaps_protected,
};

// code        : begin/end マーカーが示す窓
// sram        : 窓が居てよい SRAM 全体
// entries     : 窓の中に居るべき関数のアドレス (Thumb bit つきでよい)
// protected_  : 窓と重ならないこと (U の stack・データ窓。PMSAv8 は有効 region の重なりを許さない)
template <unsigned EntryCount, unsigned ProtectedCount>
constexpr verdict check_code_window(range code, range sram,
                                    const uintptr_t (&entries)[EntryCount],
                                    const range (&protected_)[ProtectedCount]) {
  if (empty(code)) return verdict::empty_window;
  if (!aligned(code)) return verdict::misaligned;
  if (code.end - code.begin > max_code_bytes) return verdict::too_large;
  if (!covers(sram, code)) return verdict::outside_sram;
  for (unsigned i = 0; i < EntryCount; ++i)
    if (!contains(code, code_address(entries[i]))) return verdict::entry_outside_window;
  for (unsigned i = 0; i < ProtectedCount; ++i)
    if (overlaps(code, protected_[i])) return verdict::overlaps_protected;
  return verdict::ok;
}

}  // namespace shizuku::selftest::u_cost_sram

#endif  // SHIZUKU_U_COST_SRAM_WINDOW_HPP
