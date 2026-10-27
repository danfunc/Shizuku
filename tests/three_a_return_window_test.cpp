#include <cassert>
#include <cstdint>

#include "shizuku/native_call.hpp"

struct object_rows {
  uintptr_t base[3]{};
  uintptr_t limit[3]{};

  shizuku::native::return_region_window return_region(uintptr_t id) const {
    return id < 3 ? shizuku::native::return_region_window{base[id], limit[id]}
                  : shizuku::native::return_region_window{0, 0};
  }
};

int main() {
  constexpr uint8_t kDispatch = 2;
  constexpr uint8_t kCall = 1;
  object_rows rows{};
  rows.base[1] = 0x20001000u;
  rows.limit[1] = 0x20002000u;

  // Ordinary return uses the caller object's current row, allowing a grant revoke
  // performed while the callee was active to take effect immediately.
  auto window = shizuku::native::resolve_return_region_window(rows, kCall, kDispatch, 1);
  assert(window.base == 0x20001000u && window.limit == 0x20002000u);
  rows.base[1] = 0;
  rows.limit[1] = 0;
  window = shizuku::native::resolve_return_region_window(rows, kCall, kDispatch, 1);
  assert(window.base == 0 && window.limit == 0);

  // A caller activation entered through dispatch stays closed even if its object
  // row is non-empty; the immediately preceding dispatch entry is authoritative.
  rows.base[2] = 0x20002000u;
  rows.limit[2] = 0x20003000u;
  window = shizuku::native::resolve_return_region_window(rows, kDispatch, kDispatch, 2);
  assert(window.base == 0 && window.limit == 0);
  return 0;
}
