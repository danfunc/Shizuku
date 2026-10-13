#include "armv6m.hpp"

#include <cassert>

using arch = shizuku::archs::armv6m<8, 2>;

int main() {
  constexpr uintptr_t base = 0x10084000u;
  // The failing flash-stream payload cannot be granted exactly. A page-aligned
  // 512-byte payload crosses a flash programming page and is MPU-representable.
  assert(!arch::region_range_representable(base, base + 300));
  assert(arch::region_range_representable(base, base + 512));
  assert(!arch::region_range_representable(base + 1, base + 512));

  arch::context_t context{};
  assert(arch::grant_set(context, base, base + 512));
  assert(context.region_base == base && context.region_limit == base + 512);
  // Rejected grants must preserve the prior window.
  assert(!arch::grant_set(context, base, base + 300));
  assert(context.region_base == base && context.region_limit == base + 512);
  assert(!arch::grant_set(context, base, base));
  assert(!arch::grant_set(context, base + 512, base));
  assert(context.region_base == base && context.region_limit == base + 512);

  // Subregion-aligned windows are accepted, while one byte beyond that boundary
  // is rejected. Revocation (limit zero) remains possible.
  assert(arch::region_range_representable(base + 256, base + 2048));
  assert(!arch::region_range_representable(base + 256, base + 2047));
  assert(arch::set_region_window(context, 0, 0));
  assert(context.region_base == 0 && context.region_limit == 0);

  // Reproduce the return-stub dead zone using the RP2040 ABI header size.
  // The old actor admission permits a frame at dispatch_reserve. Its return
  // dispatch subtracts another 112 bytes and fails the same reserve check.
  constexpr uintptr_t limit = 0x20008000u;
  constexpr uint32_t header_bytes = 68;
  constexpr uintptr_t total = (header_bytes + 32u + 8u + 7u) & ~uintptr_t{7};
  const uintptr_t reserve = header_bytes + 32u + 8u +
                            arch::EXC_FRAME_MAX_BYTES +
                            arch::FAULT_CONTEXT_BYTES + arch::CALL_HEADROOM;
  assert(total == 112);
  context.stack_limit_addr = limit;
  context.sp = reinterpret_cast<arch::exception_frame_t *>(limit + total + reserve);
  assert(!arch::call_entry_has_return_room(context, header_bytes));
  // Admission at the new boundary guarantees that the return frame is still
  // above the ordinary dispatch reserve; one aligned word pair below is denied.
  context.sp = reinterpret_cast<arch::exception_frame_t *>(limit + 2 * total + reserve);
  assert(arch::call_entry_has_return_room(context, header_bytes));
  const uintptr_t actor_frame = reinterpret_cast<uintptr_t>(context.sp) - total;
  assert(actor_frame - total >= limit + reserve);
  context.sp = reinterpret_cast<arch::exception_frame_t *>(limit + 2 * total + reserve - 8);
  assert(!arch::call_entry_has_return_room(context, header_bytes));
  context.stack_limit_addr = 0;
  assert(arch::call_entry_has_return_room(context, header_bytes));
}
