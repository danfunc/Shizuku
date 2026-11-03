#ifndef SHIZUKU_LEAN_LEDGER
#define SHIZUKU_LEAN_LEDGER 1
#endif

#include <cassert>
#include <cstdint>
#include <array>
#include <vector>

#include "shizuku/kernel_abi.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/stack_window.hpp"
#include "shizuku/templates/thread.hpp"
#include "shizuku/native_call.hpp"

// ===========================================================================
// Pure function tests
// ===========================================================================

static void test_open_window() {
  using namespace shizuku;

  // Normal open with sufficient space
  auto win1 = open_window(0x20002000u, 0x20000000u);
  assert(win1.is_valid());
  assert(win1.hi == 0x20002000u);
  assert(win1.lo == 0x20001F00u);
  assert(win1.size() == 256u);

  // Unaligned caller_sp rounded down to 16B alignment
  auto win2 = open_window(0x2000200Fu, 0x20000000u);
  assert(win2.is_valid());
  assert(win2.hi == 0x20002000u);
  assert(win2.lo == 0x20001F00u);

  // Remaining space less than 256B but >= 16B
  auto win3 = open_window(0x20000080u, 0x20000000u);
  assert(win3.is_valid());
  assert(win3.hi == 0x20000080u);
  assert(win3.lo == 0x20000000u);
  assert(win3.size() == 128u);

  // Boundary: exactly 16B space
  auto win4 = open_window(0x20000010u, 0x20000000u);
  assert(win4.is_valid());
  assert(win4.hi == 0x20000010u);
  assert(win4.lo == 0x20000000u);
  assert(win4.size() == 16u);

  // Insufficient space: less than 16B
  auto win_invalid1 = open_window(0x2000000Fu, 0x20000000u);
  assert(!win_invalid1.is_valid());

  // Insufficient space: caller_sp <= stack_lo
  auto win_invalid2 = open_window(0x20000000u, 0x20000000u);
  assert(!win_invalid2.is_valid());

  auto win_invalid3 = open_window(0x1FFFF000u, 0x20000000u);
  assert(!win_invalid3.is_valid());
}

static void test_extend_window() {
  using namespace shizuku;

  const uintptr_t stack_lo = 0x20000000u;
  const auto initial = open_window(0x20002000u, stack_lo);
  assert(initial.is_valid());
  assert(initial.lo == 0x20001F00u && initial.hi == 0x20002000u);

  // 1. Fault within valid extend region: stack_lo <= a < win.lo
  auto ext1 = extend_window(initial, 0x20001E80u, stack_lo);
  assert(ext1.is_valid());
  assert(ext1.hi == initial.hi);
  assert(ext1.lo == 0x20001E00u);

  // 2. Fault deeper than 1 step:
  auto ext2 = extend_window(initial, 0x20001A03u, stack_lo);
  assert(ext2.is_valid());
  assert(ext2.hi == initial.hi);
  assert(ext2.lo == 0x20001A00u);

  // 3. Fault deeper: a & ~3 becomes new_lo when smaller than win.lo - K
  auto ext3 = extend_window(initial, 0x20000020u, stack_lo);
  assert(ext3.is_valid());
  assert(ext3.hi == initial.hi);
  assert(ext3.lo == 0x20000020u);

  // 4. Fault near bottom when win.lo - K < stack_lo (clamps to stack_lo):
  stack_window near_bottom{stack_lo + 100u, initial.hi};
  auto ext4 = extend_window(near_bottom, stack_lo + 50u, stack_lo);
  assert(ext4.is_valid());
  assert(ext4.hi == initial.hi);
  assert(ext4.lo == stack_lo);

  // 5. Fault at stack_lo exact boundary:
  auto ext5 = extend_window(initial, stack_lo, stack_lo);
  assert(ext5.is_valid());
  assert(ext5.hi == initial.hi);
  assert(ext5.lo == stack_lo);

  // 6. Rejection: a < stack_lo (true stack overflow)
  auto rej_underflow = extend_window(initial, stack_lo - 4u, stack_lo);
  assert(!rej_underflow.is_valid());

  // 7. Rejection: a >= win.lo (inside already opened window)
  auto rej_inside = extend_window(initial, initial.lo + 4u, stack_lo);
  assert(!rej_inside.is_valid());

  // 8. Rejection: a >= win.hi (caller frame violation)
  auto rej_caller = extend_window(initial, initial.hi + 4u, stack_lo);
  assert(!rej_caller.is_valid());
}

static void test_zero_window() {
  using namespace shizuku;

  alignas(16) std::array<uint32_t, 128> buffer;
  buffer.fill(0xDEADBEEFu);

  const uintptr_t buf_start = reinterpret_cast<uintptr_t>(buffer.data());
  const uintptr_t zero_lo = buf_start + 64u;
  const uintptr_t zero_hi = buf_start + 192u;

  stack_window win{zero_lo, zero_hi};
  zero_window(win);

  for (uintptr_t p = buf_start; p < zero_lo; p += 4) {
    assert(*reinterpret_cast<uint32_t *>(p) == 0xDEADBEEFu);
  }
  for (uintptr_t p = zero_lo; p < zero_hi; p += 4) {
    assert(*reinterpret_cast<uint32_t *>(p) == 0u);
  }
  for (uintptr_t p = zero_hi; p < buf_start + sizeof(buffer); p += 4) {
    assert(*reinterpret_cast<uint32_t *>(p) == 0xDEADBEEFu);
  }
}

// ===========================================================================
// Fake Arch & Mock Kernel for Ledger In-Place Stack Window Tests
// ===========================================================================

struct mock_frame {
  uintptr_t pc = 0;
  uintptr_t ra = 0;
  uintptr_t sp = 0;
};

struct mock_context {
  mock_frame frame{};
  mock_frame *sp = &frame;
  uintptr_t user_sp = 0;
  bool privileged = false;
  uintptr_t region_base = 0;
  uintptr_t region_limit = 0;
};

struct test_fake_arch {
  static constexpr bool CALL_IN_PLACE = true;

  static inline std::vector<shizuku::stack_window> zeroed_windows;
  static inline std::vector<shizuku::stack_window> applied_windows;

  static void reset() {
    zeroed_windows.clear();
    applied_windows.clear();
  }

  static void zero_stack_window(shizuku::stack_window win) {
    zeroed_windows.push_back(win);
    shizuku::zero_window(win);
  }

  static void apply_stack_window(mock_context &, uintptr_t lo, uintptr_t hi) {
    applied_windows.push_back({lo, hi});
  }

  static bool call_header_has_room(const mock_context &, uint32_t) { return true; }
  static uintptr_t frame_pc(const mock_frame &f) { return f.pc; }
  static uintptr_t call_return_sp(const mock_context &c) { return c.user_sp; }
  static bool context_privileged(const mock_context &c) { return c.privileged; }
  static void set_entry(mock_frame &f, uintptr_t pc, uintptr_t ra) {
    f.pc = pc;
    f.ra = ra;
  }
  static uintptr_t return_stub() { return 0x1000u; }
  static void set_priv(mock_context &c, bool priv) { c.privileged = priv; }
  static void set_region_window(mock_context &c, uintptr_t b, uintptr_t l) {
    c.region_base = b;
    c.region_limit = l;
  }
  static void switch_callee_protection(mock_context &, mock_frame &, uint32_t, bool) {}
  static mock_frame *call_resume_at(mock_context &c, uintptr_t pc, uintptr_t sp, bool priv,
                                    uintptr_t, uintptr_t, bool) {
    c.frame.pc = pc;
    c.user_sp = sp;
    c.privileged = priv;
    return &c.frame;
  }
};

struct mock_kobj {
  static constexpr uint32_t MAX_DEPTH = 32;

  struct callee_view {
    uint32_t id = 0;
    uint32_t parent_handler_object = 0;
    uintptr_t parent_handler_entry = 0;
    shizuku::object_kind kind = shizuku::object_kind::PLAIN;
    bool privileged = false;
    uintptr_t region_base = 0;
    uintptr_t region_limit = 0;
    uint32_t instance_generation = 1;
  };

  shizuku::native::return_region_window return_region(uint32_t) const {
    return {0, 0};
  }
};

struct mock_kernel {
  using ARCH = test_fake_arch;
  using CONTEXT = mock_context;
  using FRAME = mock_frame;
  using THREAD = shizuku::templates::thread<mock_context>;

  static constexpr uint8_t LEDGER_CALL = 1;
  static constexpr uint8_t LEDGER_DISPATCH = 2;
  static constexpr uint8_t LEDGER_DESCRIPTOR_CALL = 3;

  uint32_t svc_handler_object() const { return 0xFFFFu; }
};

// ===========================================================================
// Test A -> A recursion
// ===========================================================================
static void test_ledger_nesting_a_a() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  mock_kernel::THREAD thread{};
  mock_context context{};

  shizuku::templates::thread<mock_context>::call_ledger_entry ledger_mem[8]{};
  thread.ledger = ledger_mem;
  thread.ledger_capacity = 8;
  thread.base_object = 1;

  // Thread U-stack range: [0x20000000, 0x20004000)
  alignas(16) static uint8_t dummy_user_stack[0x4000];
  const uintptr_t stack_lo = reinterpret_cast<uintptr_t>(dummy_user_stack);
  const uintptr_t stack_hi = stack_lo + sizeof(dummy_user_stack);
  thread.set_user_stack(stack_lo, stack_hi);

  // Initial caller state
  context.user_sp = stack_lo + 0x3000u;
  context.frame.pc = 0x42000010u;

  mock_kobj::callee_view view_a{};
  view_a.id = 2; // Object A
  view_a.instance_generation = 1;

  // 1. First call to A
  auto err1 = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_a, 0x42001000u, mock_kernel::LEDGER_CALL);
  assert(err1 == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 1);
  assert(thread.win[0].is_valid());
  const auto outer_win = thread.win[0];
  assert(outer_win.hi == (context.user_sp & ~uintptr_t(15)));
  assert(outer_win.lo == outer_win.hi - shizuku::kStackWindowStep);
  assert(thread.cur_win_lo == outer_win.lo);
  assert(thread.cur_win_hi == outer_win.hi);

  // Callee A consumes 128B stack and re-enters A (A -> A)
  context.user_sp = outer_win.hi - 128u;
  context.frame.pc = 0x42001020u;

  // 2. Second call to A (nested A -> A)
  auto err2 = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_a, 0x42001000u, mock_kernel::LEDGER_CALL);
  assert(err2 == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 2);
  assert(thread.win[1].is_valid());
  const auto inner_win = thread.win[1];
  assert(inner_win.hi == (context.user_sp & ~uintptr_t(15)));
  assert(inner_win.lo == inner_win.hi - shizuku::kStackWindowStep);
  assert(inner_win.hi < outer_win.hi); // Nested below
  assert(thread.cur_win_lo == inner_win.lo);
  assert(thread.cur_win_hi == inner_win.hi);

  // 3. Return from inner A (pop_in_place)
  mock_frame *res_frame = nullptr;
  bool ok_pop1 = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop1);
  assert(thread.call_stack.depth == 1);
  assert(test_fake_arch::zeroed_windows.size() == 1);
  // Opened window of inner A was zeroed
  assert(test_fake_arch::zeroed_windows[0].lo == inner_win.lo);
  assert(test_fake_arch::zeroed_windows[0].hi == inner_win.hi);
  // Resumed to outer A's window
  assert(thread.cur_win_lo == outer_win.lo);
  assert(thread.cur_win_hi == outer_win.hi);

  // 4. Return from outer A (pop_in_place)
  bool ok_pop2 = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop2);
  assert(thread.call_stack.depth == 0);
  assert(test_fake_arch::zeroed_windows.size() == 2);
  // Outer window zeroed
  assert(test_fake_arch::zeroed_windows[1].lo == outer_win.lo);
  assert(test_fake_arch::zeroed_windows[1].hi == outer_win.hi);
  // Restored to thread base stack
  assert(thread.cur_win_lo == stack_lo && thread.cur_win_hi == stack_hi);
}

// ===========================================================================
// Test A -> B -> A nesting & extend
// ===========================================================================
static void test_ledger_nesting_a_b_a() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  mock_kernel::THREAD thread{};
  mock_context context{};

  shizuku::templates::thread<mock_context>::call_ledger_entry ledger_mem[8]{};
  thread.ledger = ledger_mem;
  thread.ledger_capacity = 8;
  thread.base_object = 1;

  alignas(16) static uint8_t dummy_user_stack[0x4000];
  const uintptr_t stack_lo = reinterpret_cast<uintptr_t>(dummy_user_stack);
  const uintptr_t stack_hi = stack_lo + sizeof(dummy_user_stack);
  thread.set_user_stack(stack_lo, stack_hi);

  // 1. Call A (object 2)
  context.user_sp = stack_lo + 0x3000u;
  mock_kobj::callee_view view_a{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  auto err1 = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_a, 0x42001000u, mock_kernel::LEDGER_CALL);
  assert(err1 == shizuku::kernel_error::OK);

  // 2. Call B (object 3)
  context.user_sp = thread.win[0].hi - 160u;
  mock_kobj::callee_view view_b{3, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  auto err2 = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_b, 0x42002000u, mock_kernel::LEDGER_CALL);
  assert(err2 == shizuku::kernel_error::OK);

  // 3. Call A again (A -> B -> A)
  context.user_sp = thread.win[1].hi - 160u;
  auto err3 = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_a, 0x42001000u, mock_kernel::LEDGER_CALL);
  assert(err3 == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 3);

  // Simulate stack fault in inner A: extend window
  const uintptr_t fault_addr = thread.win[2].lo - 100u;
  const auto extended = shizuku::extend_window(thread.win[2], fault_addr, stack_lo);
  assert(extended.is_valid());
  thread.win[2] = extended;
  thread.cur_win_lo = extended.lo;

  // Pop inner A
  mock_frame *res_frame = nullptr;
  bool ok_pop1 = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop1);
  assert(thread.call_stack.depth == 2);
  // Verifying that the extended range was passed to zero_window
  assert(test_fake_arch::zeroed_windows.size() == 1);
  assert(test_fake_arch::zeroed_windows[0].lo == extended.lo);
  assert(test_fake_arch::zeroed_windows[0].hi == extended.hi);

  // Pop B
  bool ok_pop2 = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop2);
  assert(thread.call_stack.depth == 1);
  assert(test_fake_arch::zeroed_windows.size() == 2);

  // Pop outer A
  bool ok_pop3 = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop3);
  assert(thread.call_stack.depth == 0);
  assert(test_fake_arch::zeroed_windows.size() == 3);
}

// ===========================================================================
// Test unwind_in_place (multi-level pop zeroes each opened window)
// ===========================================================================
static void test_ledger_unwind_in_place() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  mock_kernel::THREAD thread{};
  mock_context context{};

  shizuku::templates::thread<mock_context>::call_ledger_entry ledger_mem[8]{};
  thread.ledger = ledger_mem;
  thread.ledger_capacity = 8;
  thread.base_object = 1;

  alignas(16) static uint8_t dummy_user_stack[0x4000];
  const uintptr_t stack_lo = reinterpret_cast<uintptr_t>(dummy_user_stack);
  const uintptr_t stack_hi = stack_lo + sizeof(dummy_user_stack);
  thread.set_user_stack(stack_lo, stack_hi);

  // Push 3 activations
  context.user_sp = stack_lo + 0x3000u;
  mock_kobj::callee_view view{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));
  context.user_sp = thread.win[0].hi - 160u;
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));
  context.user_sp = thread.win[1].hi - 160u;
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));
  assert(thread.call_stack.depth == 3);

  // Unwind 2 frames at once (FORWARD_CHILD_EXIT / count = 2)
  mock_frame *res_frame = nullptr;
  bool ok_unwind = shizuku::native::unwind_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, 2, true);
  assert(ok_unwind);
  assert(thread.call_stack.depth == 1);
  // Both unwound frames were zeroed
  assert(test_fake_arch::zeroed_windows.size() == 2);
  // Caller window restored to activation 0
  assert(thread.cur_win_lo == thread.win[0].lo);
  assert(thread.cur_win_hi == thread.win[0].hi);
}

// ===========================================================================
// Test insufficient stack space rejection
// ===========================================================================
static void test_insufficient_stack_rejection() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kernel::THREAD thread{};
  mock_context context{};

  shizuku::templates::thread<mock_context>::call_ledger_entry ledger_mem[8]{};
  thread.ledger = ledger_mem;
  thread.ledger_capacity = 8;

  const uintptr_t stack_lo = 0x20001000u;
  thread.set_user_stack(stack_lo, stack_lo + 0x1000u);

  // user_sp too close to stack_lo (< 16B margin)
  context.user_sp = stack_lo + 8u;
  mock_kobj::callee_view view{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  auto err = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view, 0x1000u, 1);
  assert(err == shizuku::kernel_error::NO_STACK);
  assert(thread.call_stack.depth == 0); // Ledger entry was NOT written
}

// ===========================================================================
// Test depth >= MAX_STACK_WINDOWS rejection (fail-closed, no ledger written)
// ===========================================================================
static void test_max_stack_windows_exceeded_rejection() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kernel::THREAD thread{};
  mock_context context{};

  constexpr uint32_t cap = mock_kernel::THREAD::MAX_STACK_WINDOWS + 4;
  std::vector<shizuku::templates::thread<mock_context>::call_ledger_entry> ledger_mem(cap);
  thread.ledger = ledger_mem.data();
  thread.ledger_capacity = cap;

  const uintptr_t stack_lo = 0x20000000u;
  const uintptr_t stack_hi = stack_lo + 0x10000u;
  thread.set_user_stack(stack_lo, stack_hi);

  thread.call_stack.depth = mock_kernel::THREAD::MAX_STACK_WINDOWS;
  context.user_sp = stack_hi - 0x100u;

  mock_kobj::callee_view view{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  auto err = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view, 0x1000u, 1);
  assert(err == shizuku::kernel_error::NO_STACK);
  assert(thread.call_stack.depth == mock_kernel::THREAD::MAX_STACK_WINDOWS);
}

// ===========================================================================
// Test pop_in_place does not copy empty dispatch window to cur_win
// ===========================================================================
static void test_pop_in_place_with_dispatch_caller() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  mock_kernel::THREAD thread{};
  mock_context context{};

  shizuku::templates::thread<mock_context>::call_ledger_entry ledger_mem[8]{};
  thread.ledger = ledger_mem;
  thread.ledger_capacity = 8;
  thread.base_object = 1;

  alignas(16) static uint8_t dummy_user_stack[0x4000];
  const uintptr_t stack_lo = reinterpret_cast<uintptr_t>(dummy_user_stack);
  const uintptr_t stack_hi = stack_lo + sizeof(dummy_user_stack);
  thread.set_user_stack(stack_lo, stack_hi);

  // Case 1: Base (depth 0) -> Dispatch (depth 1, win={}) -> Call (depth 2)
  thread.ledger[0].via = mock_kernel::LEDGER_DISPATCH;
  thread.ledger[0].object = 1;
  thread.win[0] = shizuku::stack_window{}; // Dispatch activation has empty window
  thread.call_stack.depth = 1;

  context.user_sp = stack_hi - 0x200u;
  mock_kobj::callee_view view{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  auto err = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view, 0x1000u, 1);
  assert(err == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 2);
  assert(thread.win[1].is_valid());

  // Pop Call activation (returning to Dispatch activation)
  mock_frame *res_frame = nullptr;
  bool ok_pop = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop);
  assert(thread.call_stack.depth == 1);
  // cur_win must NOT be {0, 0}; it must fall back to base thread stack [stack_lo, stack_hi)
  assert(thread.cur_win_lo == stack_lo);
  assert(thread.cur_win_hi == stack_hi);

  // Case 2: Call A (depth 1) -> Dispatch (depth 2, win={}) -> Call B (depth 3)
  thread.call_stack.depth = 0;
  context.user_sp = stack_hi - 0x100u;
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));
  const auto win_call_a = thread.win[0];
  assert(win_call_a.is_valid());

  // Dispatch activation pushed at depth 1
  thread.ledger[1].via = mock_kernel::LEDGER_DISPATCH;
  thread.win[1] = shizuku::stack_window{};
  thread.call_stack.depth = 2;

  // Call B pushed at depth 2
  context.user_sp = win_call_a.hi - 160u;
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));
  assert(thread.call_stack.depth == 3);

  // Pop Call B (returning to Dispatch activation)
  res_frame = nullptr;
  ok_pop = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop);
  assert(thread.call_stack.depth == 2);
  // cur_win must recover outer Call A window rather than {0, 0}
  assert(thread.cur_win_lo == win_call_a.lo);
  assert(thread.cur_win_hi == win_call_a.hi);
}

// ===========================================================================
// Test window extension semantics during fault
// ===========================================================================
static void test_window_extension_fault_semantics() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kernel::THREAD thread{};
  mock_context context{};

  shizuku::templates::thread<mock_context>::call_ledger_entry ledger_mem[8]{};
  thread.ledger = ledger_mem;
  thread.ledger_capacity = 8;
  thread.base_object = 1;

  const uintptr_t stack_lo = 0x20000000u;
  const uintptr_t stack_hi = 0x20004000u;
  thread.set_user_stack(stack_lo, stack_hi);

  // depth == 0 (base): cannot extend window
  assert(thread.call_stack.depth == 0);
  assert(thread.cur_win_lo == stack_lo);
  assert(thread.cur_win_hi == stack_hi);

  // Enter Call activation: win = [0x20003E00, 0x20003F00)
  context.user_sp = stack_hi - 0x100u;
  mock_kobj::callee_view view{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));
  assert(thread.call_stack.depth == 1);
  const auto initial_win = thread.win[0];
  assert(initial_win.is_valid());

  // Fault in [stack_lo, win.lo): window extends down
  const uintptr_t fault_addr = initial_win.lo - 64u;
  assert(fault_addr >= stack_lo && fault_addr < initial_win.lo);
  const auto extended_win = shizuku::extend_window(initial_win, fault_addr, stack_lo);
  assert(extended_win.is_valid());
  assert(extended_win.lo <= fault_addr);
  assert(extended_win.hi == initial_win.hi);

  // Fault < stack_lo: out of bounds, rejected
  assert(!shizuku::extend_window(initial_win, stack_lo - 4u, stack_lo).is_valid());

  // Fault >= win.hi: out of bounds, rejected
  assert(!shizuku::extend_window(initial_win, initial_win.hi, stack_lo).is_valid());
  assert(!shizuku::extend_window(initial_win, initial_win.hi + 16u, stack_lo).is_valid());

  // Dispatch activation has empty window: extend_window on {} must fail
  const shizuku::stack_window dispatch_win{};
  assert(!dispatch_win.is_valid());
  assert(!shizuku::extend_window(dispatch_win, fault_addr, stack_lo).is_valid());
}

int main() {
  test_open_window();
  test_extend_window();
  test_zero_window();
  test_ledger_nesting_a_a();
  test_ledger_nesting_a_b_a();
  test_ledger_unwind_in_place();
  test_insufficient_stack_rejection();
  test_max_stack_windows_exceeded_rejection();
  test_pop_in_place_with_dispatch_caller();
  test_window_extension_fault_semantics();
  return 0;
}
