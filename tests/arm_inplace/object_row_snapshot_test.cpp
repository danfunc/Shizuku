#include <cassert>
#include <cstdint>
#include "shizuku/templates/kernel_object.hpp"

struct mock_arch {
  static uint32_t load_acquire32(volatile uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
  }
};
struct mock_kernel {
  using ARCH = mock_arch;
  struct call_ledger_entry {};
  static constexpr unsigned CORE_COUNT = 2;
};
using objects = shizuku::templates::kernel_object<mock_kernel, 4, 2, 2, 1>;
static uintptr_t method(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }

namespace shizuku::selftest {
struct dispatcher_test_access {
  static void initialize(objects &o) {
    auto &caller = o.m_objects[1];
    auto &target = o.m_objects[2];
    caller.created = target.created = true;
    caller.creator_object = 0;
    target.creator_object = 1;
    target.methods[0] = method;
    target.region_base = 0x1000;
    target.region_limit = 0x2000;
    target.instance_generation = 7;
  }
  static void sequence(objects &o, unsigned id, uint32_t value) {
    __atomic_store_n(&o.m_objects[id].seq, value, __ATOMIC_SEQ_CST);
  }
  static bool begin(objects &o, unsigned id) { return objects::row_write_begin(o.m_objects[id]); }
  static void end(objects &o, unsigned id) { objects::row_write_end(o.m_objects[id]); }
};
}

int main() {
  objects o{};
  using access = shizuku::selftest::dispatcher_test_access;
  access::initialize(o);
  shizuku::object_error error{};
  objects::method_t entry = nullptr;
  auto result = o.resolve_method_view(2, 0, 1, error, entry);
  assert(result.found && error == shizuku::object_error::OK && entry == method);
  assert(result.view.region_base == 0x1000 && result.view.region_limit == 0x2000);
  assert(result.view.instance_generation == 7);
  // return_region は created と窓の 3 欄だけを seq の組で守る (3A)。
  auto live = o.return_region(2);
  assert(live.base == 0x1000 && live.limit == 0x2000);
  assert(o.return_region(0).base == 0 && o.return_region(0).limit == 0); // created していない行
  assert(o.return_region(4).base == 0 && o.return_region(4).limit == 0); // 範囲外
  assert(access::begin(o, 2)); // 書き手が行を差し替え中 (seq 奇数)
  live = o.return_region(2);
  assert(live.base == 0 && live.limit == 0);
  access::end(o, 2);
  live = o.return_region(2);
  assert(live.base == 0x1000 && live.limit == 0x2000);
  access::sequence(o, 2, 1);
  assert(!access::begin(o, 2));
  result = o.resolve_method_view(2, 0, 1, error, entry);
  assert(!result.found && error == shizuku::object_error::BAD_OBJECT);
  const auto denied = o.return_region(2);
  assert(denied.base == 0 && denied.limit == 0);
  access::sequence(o, 2, 2);
  access::sequence(o, 1, 1);
  result = o.resolve_method_view(2, 0, 1, error, entry);
  assert(!result.found && error == shizuku::object_error::BAD_OBJECT);
  access::sequence(o, 1, 2);
  result = o.resolve_method_view(2, 2, 1, error, entry);
  assert(!result.found && error == shizuku::object_error::BAD_METHOD);
  result = o.resolve_method_view(2, 1, 1, error, entry);
  assert(!result.found && error == shizuku::object_error::UNDECLARED_METHOD);
  result = o.resolve_method_view(2, 0, 3, error, entry);
  assert(!result.found && error == shizuku::object_error::NOT_PRIVILEGED);
  assert(access::begin(o, 2));
  result = o.resolve_method_view(2, 0, 1, error, entry);
  assert(!result.found && error == shizuku::object_error::BAD_OBJECT);
  access::end(o, 2);
  result = o.resolve_method_view(2, 0, 1, error, entry);
  assert(result.found && error == shizuku::object_error::OK);
  access::sequence(o, 2, UINT32_MAX - 1u);
  assert(!access::begin(o, 2));
  result = o.resolve_method_view(2, 0, 1, error, entry);
  assert(!result.found && error == shizuku::object_error::BAD_OBJECT);
}
