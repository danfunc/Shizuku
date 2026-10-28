#include <cassert>
#include <cstdint>
#include <type_traits>
#include "shizuku/templates/thread.hpp"

struct compact_context { static constexpr bool COMPACT_LEDGER = true; };
struct standard_context {};
using compact = shizuku::templates::thread<compact_context>::call_ledger_entry;
using standard = shizuku::templates::thread<standard_context>::call_ledger_entry;
static_assert(!std::is_same_v<compact, standard>);
static_assert(sizeof(uintptr_t) != 4 || sizeof(compact) == 20);

int main() {
  compact entry{72, 71, 0x12345678u, 2, 3, 33};
  entry.prev_call = 34;
  entry.ret_privileged = 1;
  entry.generation = UINT32_MAX;
  entry.ret_pc = 0x87654321u;
  entry.ret_sp = 0x76543210u;
  assert(entry.object == 72 && entry.handler_object == 71);
  assert(entry.kind == 2 && entry.via == 3 && entry.unwind_floor == 33);
  assert(entry.prev_call == 34 && entry.ret_privileged == 1);
  entry.ret_privileged = 3;
  assert(entry.ret_privileged > 1);
  assert(entry.handler_entry == 0x12345678u && entry.generation == UINT32_MAX);
  assert(entry.ret_pc == 0x87654321u && entry.ret_sp == 0x76543210u);
}
