#include <cassert>
#include <cstdint>
#ifdef NDEBUG
#error "This test requires assert()"
#endif
#include "arch/armv8m/armv8m.hpp"

// Link placeholders only: the ARM assembly veneer is not executed on host.
extern "C" void shizuku_armv8m_call_entry() {}
extern "C" void shizuku_armv8m_return_stub() {}

using A = shizuku::archs::armv8m<8, 4>;

int main() {
  alignas(32) uint8_t stack[512]{};
  const uintptr_t base = reinterpret_cast<uintptr_t>(stack);
  const uintptr_t top = base + sizeof(stack);
  constexpr uintptr_t entry = 0x10001235;
  constexpr uintptr_t argument = 0x12345678;
  A::context_t c{};
  c.control = A::CONTROL_UNPRIV_PSP;
  c.psplim = 0x12340000;
  A::prepare_thread_entry(c, top, entry, A::return_stub(), argument);
  assert(c.sp == reinterpret_cast<A::exception_frame_t *>(top - sizeof(*c.sp)));
  assert(c.exc_return == 0xfffffffdu && c.sp->xPSR == (1u << 24));
  assert(c.sp->r0 == argument && c.sp->r1 == 0 && c.sp->r2 == 0 &&
         c.sp->r3 == 0 && c.sp->r12 == 0);
  assert(c.control == A::CONTROL_UNPRIV_PSP && c.psplim == 0x12340000);
  const uintptr_t after = A::psp_after_return(c);
  assert(after == top && (after & 7u) == 0);
  static_assert(A::HANDLER_STACK_ARG_BYTES == 8);
  const uintptr_t slot = after - A::HANDLER_STACK_ARG_BYTES;
  assert(slot >= base && slot + sizeof(uint32_t) <= top && (slot & 7u) == 0);
  const auto window = A::encode_stack_window(base, top);
  assert((window.rbar & ~31u) == (static_cast<uint32_t>(base) & ~31u));
  assert((window.rlar & ~31u) == (static_cast<uint32_t>(top - 1) & ~31u));
  assert(c.sp->pc == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&shizuku_armv8m_call_entry)));
  assert(c.sp->lr == entry);

  A::prepare_thread_entry(c, top, entry, 0x10005679, argument);
  assert(c.sp->pc == entry && c.sp->lr == 0x10005679);
}
