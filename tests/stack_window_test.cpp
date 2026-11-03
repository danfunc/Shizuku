#ifndef SHIZUKU_LEAN_LEDGER
#define SHIZUKU_LEAN_LEDGER 1
#endif

#include <cassert>
#include <cstdint>
#include <array>
#include <type_traits>
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

  // 仕様変更 (段 C-0): U 復帰時に resume frame (128B) が caller の ret_sp-128 に
  // 作られるため、最小窓幅がそれより小さいとゼロ化済みの外に frame の未設定語が
  // 残りうる。このため kStackWindowMin = 256 (= K) に引き上げられ、
  // hi - stack_lo < 256 の場合は無効 (NO_STACK) となる。

  // Remaining space less than 256B (e.g. 128B): now invalid
  auto win3 = open_window(0x20000080u, 0x20000000u);
  assert(!win3.is_valid());

  // Remaining space 16B: now invalid (previously minimum boundary)
  auto win4 = open_window(0x20000010u, 0x20000000u);
  assert(!win4.is_valid());

  // Boundary: exactly 256B space is valid
  auto win_boundary = open_window(0x20000100u, 0x20000000u);
  assert(win_boundary.is_valid());
  assert(win_boundary.hi == 0x20000100u);
  assert(win_boundary.lo == 0x20000000u);
  assert(win_boundary.size() == 256u);

  // Just below boundary: 240B space is invalid
  auto win_below_boundary = open_window(0x200000F0u, 0x20000000u);
  assert(!win_below_boundary.is_valid());

  // Insufficient space: less than 16B
  auto win_invalid1 = open_window(0x2000000Fu, 0x20000000u);
  assert(!win_invalid1.is_valid());

  // Insufficient space: caller_sp <= stack_lo
  auto win_invalid2 = open_window(0x20000000u, 0x20000000u);
  assert(!win_invalid2.is_valid());

  auto win_invalid3 = open_window(0x1FFFF000u, 0x20000000u);
  assert(!win_invalid3.is_valid());

  // ARM default: stack_lo == 0 with typical SRAM caller_sp (sufficient space)
  auto win_arm = open_window(0x20010000u, 0u);
  assert(win_arm.is_valid());
  assert(win_arm.hi == 0x20010000u);
  assert(win_arm.lo == 0x2000FF00u);
  assert(win_arm.size() == 256u);

  // ARM default: stack_lo == 0 but caller_sp < kStackWindowMin (256B) is invalid
  auto win_arm_too_small = open_window(0x80u, 0u);
  assert(!win_arm_too_small.is_valid());
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

  // 8. Rejection: a >= win.hi (caller frame violation) - exact boundary and above
  auto rej_caller_exact = extend_window(initial, initial.hi, stack_lo);
  assert(!rej_caller_exact.is_valid());

  auto rej_caller = extend_window(initial, initial.hi + 4u, stack_lo);
  assert(!rej_caller.is_valid());

  auto rej_caller_far = extend_window(initial, initial.hi + 0x1000u, stack_lo);
  assert(!rej_caller_far.is_valid());
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
  static constexpr bool CALL_HEADER_ON_STACK = true;
  static constexpr bool HAS_STACK_WINDOW = true;
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
  // Write sentinel in caller's frame (>= caller_sp, outside callee window)
  *reinterpret_cast<volatile uint32_t *>(context.user_sp) = 0xC001CAFEu;
  *reinterpret_cast<volatile uint32_t *>(context.user_sp + 16u) = 0xC001BEEFu;

  mock_kobj::callee_view view_a{};
  view_a.id = 2; // Object A
  view_a.instance_generation = 1;

  // 1. First call to A
  auto err1 = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_a, 0x42001000u, mock_kernel::LEDGER_CALL);
  assert(err1 == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 1);
  assert(thread.window_at(0).is_valid());
  const auto outer_win = thread.window_at(0);
  assert(outer_win.hi == (context.user_sp & ~uintptr_t(15)));
  assert(outer_win.lo == outer_win.hi - shizuku::kStackWindowStep);
  assert(thread.stk.cur_win_lo == outer_win.lo);
  assert(thread.stk.cur_win_hi == outer_win.hi);

  // Callee A writes patterns into its window and consumes 128B stack frame
  *reinterpret_cast<volatile uint32_t *>(outer_win.hi - 32u) = 0xA1A1A1A1u; // In A1 frame
  *reinterpret_cast<volatile uint32_t *>(outer_win.lo + 16u) = 0xA1A10001u;
  context.user_sp = outer_win.hi - 128u;
  context.frame.pc = 0x42001020u;

  // 2. Second call to A (nested A -> A)
  auto err2 = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_a, 0x42001000u, mock_kernel::LEDGER_CALL);
  assert(err2 == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 2);
  assert(thread.window_at(1).is_valid());
  const auto inner_win = thread.window_at(1);
  assert(inner_win.hi == (context.user_sp & ~uintptr_t(15)));
  assert(inner_win.lo == inner_win.hi - shizuku::kStackWindowStep);
  // (a) Stacks stack up without overlap: inner window top matches inner caller_sp,
  // strictly below outer window top, leaving outer activation frame intact
  assert(inner_win.hi < outer_win.hi);
  assert(inner_win.hi <= outer_win.hi - 128u);
  assert(thread.stk.cur_win_lo == inner_win.lo);
  assert(thread.stk.cur_win_hi == inner_win.hi);

  // Inner A writes patterns into its own window
  *reinterpret_cast<volatile uint32_t *>(inner_win.hi - 16u) = 0xA2A2A2A2u;
  *reinterpret_cast<volatile uint32_t *>(inner_win.lo + 16u) = 0xA2A20002u;

  // 3. Return from inner A (pop_in_place)
  mock_frame *res_frame = nullptr;
  bool ok_pop1 = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop1);
  assert(thread.call_stack.depth == 1);
  assert(test_fake_arch::zeroed_windows.size() == 1);
  // (c) Opened window of inner A was completely zeroed in real memory
  assert(test_fake_arch::zeroed_windows[0].lo == inner_win.lo);
  assert(test_fake_arch::zeroed_windows[0].hi == inner_win.hi);
  for (uintptr_t p = inner_win.lo; p < inner_win.hi; p += 4) {
    assert(*reinterpret_cast<const volatile uint32_t *>(p) == 0);
  }
  // Caller A1 frame words outside inner window are preserved intact
  assert(*reinterpret_cast<const volatile uint32_t *>(outer_win.hi - 32u) == 0xA1A1A1A1u);
  // Resumed to outer A's window
  assert(thread.stk.cur_win_lo == outer_win.lo);
  assert(thread.stk.cur_win_hi == outer_win.hi);

  // 4. Return from outer A (pop_in_place)
  bool ok_pop2 = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop2);
  assert(thread.call_stack.depth == 0);
  assert(test_fake_arch::zeroed_windows.size() == 2);
  // Outer window zeroed in real memory
  assert(test_fake_arch::zeroed_windows[1].lo == outer_win.lo);
  assert(test_fake_arch::zeroed_windows[1].hi == outer_win.hi);
  for (uintptr_t p = outer_win.lo; p < outer_win.hi; p += 4) {
    assert(*reinterpret_cast<const volatile uint32_t *>(p) == 0);
  }
  // Base caller sentinels outside outer window are preserved intact
  assert(*reinterpret_cast<const volatile uint32_t *>(stack_lo + 0x3000u) == 0xC001CAFEu);
  assert(*reinterpret_cast<const volatile uint32_t *>(stack_lo + 0x3010u) == 0xC001BEEFu);
  // Restored to thread base stack
  assert(thread.stk.cur_win_lo == stack_lo && thread.stk.cur_win_hi == stack_hi);
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
  *reinterpret_cast<volatile uint32_t *>(context.user_sp) = 0xC001CAFEu;
  mock_kobj::callee_view view_a{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  auto err1 = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_a, 0x42001000u, mock_kernel::LEDGER_CALL);
  assert(err1 == shizuku::kernel_error::OK);

  // A1 writes into its window and consumes 160B frame
  const auto win0 = thread.window_at(0);
  *reinterpret_cast<volatile uint32_t *>(win0.hi - 32u) = 0xA1A1A1A1u;
  *reinterpret_cast<volatile uint32_t *>(win0.lo + 16u) = 0xA1A10001u;

  // 2. Call B (object 3)
  context.user_sp = win0.hi - 160u;
  mock_kobj::callee_view view_b{3, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  auto err2 = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_b, 0x42002000u, mock_kernel::LEDGER_CALL);
  assert(err2 == shizuku::kernel_error::OK);

  // B writes into its window and consumes 160B frame
  const auto win1 = thread.window_at(1);
  *reinterpret_cast<volatile uint32_t *>(win1.hi - 32u) = 0xBBBBBBBBu;
  *reinterpret_cast<volatile uint32_t *>(win1.lo + 16u) = 0xBBBB0001u;

  // 3. Call A again (A -> B -> A)
  context.user_sp = win1.hi - 160u;
  auto err3 = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_a, 0x42001000u, mock_kernel::LEDGER_CALL);
  assert(err3 == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 3);

  // (a) Stacks stack up without overlap: win2.hi < win1.hi < win0.hi
  const auto win2 = thread.window_at(2);
  assert(win2.hi < win1.hi);
  assert(win1.hi < win0.hi);
  assert(win2.hi <= win1.hi - 160u);

  // Simulate stack fault in inner A: extend window
  const uintptr_t fault_addr = win2.lo - 100u;
  const auto extended = shizuku::extend_window(win2, fault_addr, stack_lo);
  assert(extended.is_valid());
  assert(extended.lo < win2.lo);
  thread.set_window_at(2, extended);
  thread.stk.cur_win_lo = extended.lo;

  // Inner A writes patterns into the extended range and window
  *reinterpret_cast<volatile uint32_t *>(extended.lo + 4u) = 0xE7E7E7E7u;
  *reinterpret_cast<volatile uint32_t *>(extended.hi - 16u) = 0xA2A2A2A2u;

  // Pop inner A
  mock_frame *res_frame = nullptr;
  bool ok_pop1 = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop1);
  assert(thread.call_stack.depth == 2);
  // (b) Verifying that the extended range was passed to zero_window and zeroed in real memory
  assert(test_fake_arch::zeroed_windows.size() == 1);
  assert(test_fake_arch::zeroed_windows[0].lo == extended.lo);
  assert(test_fake_arch::zeroed_windows[0].hi == extended.hi);
  for (uintptr_t p = extended.lo; p < extended.hi; p += 4) {
    assert(*reinterpret_cast<const volatile uint32_t *>(p) == 0);
  }
  // (c) Outer frame B is preserved intact
  assert(*reinterpret_cast<const volatile uint32_t *>(win1.hi - 32u) == 0xBBBBBBBBu);

  // Pop B
  bool ok_pop2 = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop2);
  assert(thread.call_stack.depth == 1);
  assert(test_fake_arch::zeroed_windows.size() == 2);
  for (uintptr_t p = test_fake_arch::zeroed_windows[1].lo; p < test_fake_arch::zeroed_windows[1].hi; p += 4) {
    assert(*reinterpret_cast<const volatile uint32_t *>(p) == 0);
  }
  // Outer frame A1 is preserved intact
  assert(*reinterpret_cast<const volatile uint32_t *>(win0.hi - 32u) == 0xA1A1A1A1u);

  // Pop outer A
  bool ok_pop3 = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop3);
  assert(thread.call_stack.depth == 0);
  assert(test_fake_arch::zeroed_windows.size() == 3);
  for (uintptr_t p = test_fake_arch::zeroed_windows[2].lo; p < test_fake_arch::zeroed_windows[2].hi; p += 4) {
    assert(*reinterpret_cast<const volatile uint32_t *>(p) == 0);
  }
  // Caller frame is preserved intact
  assert(*reinterpret_cast<const volatile uint32_t *>(stack_lo + 0x3000u) == 0xC001CAFEu);
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
  *reinterpret_cast<volatile uint32_t *>(context.user_sp) = 0xC001CAFEu;
  mock_kobj::callee_view view{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));

  // Activation 0 writes to its window and consumes 160B
  const auto win0 = thread.window_at(0);
  *reinterpret_cast<volatile uint32_t *>(win0.hi - 32u) = 0x11111111u;
  context.user_sp = win0.hi - 160u;
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));

  // Activation 1 writes to its window and consumes 160B
  const auto win1 = thread.window_at(1);
  *reinterpret_cast<volatile uint32_t *>(win1.hi - 32u) = 0x22222222u;
  *reinterpret_cast<volatile uint32_t *>(win1.lo + 16u) = 0x22220001u;
  context.user_sp = win1.hi - 160u;
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));

  // Activation 2 writes to its window
  const auto win2 = thread.window_at(2);
  *reinterpret_cast<volatile uint32_t *>(win2.hi - 32u) = 0x33333333u;
  *reinterpret_cast<volatile uint32_t *>(win2.lo + 16u) = 0x33330001u;
  assert(thread.call_stack.depth == 3);

  // Unwind 2 frames at once (FORWARD_CHILD_EXIT / count = 2)
  mock_frame *res_frame = nullptr;
  bool ok_unwind = shizuku::native::unwind_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, 2, true);
  assert(ok_unwind);
  assert(thread.call_stack.depth == 1);
  // Both unwound frames were zeroed
  assert(test_fake_arch::zeroed_windows.size() == 2);
  // (d) Real memory for both unwound activations is completely zeroed
  for (uintptr_t p = win2.lo; p < win2.hi; p += 4) {
    assert(*reinterpret_cast<const volatile uint32_t *>(p) == 0);
  }
  for (uintptr_t p = win1.lo; p < win1.hi; p += 4) {
    assert(*reinterpret_cast<const volatile uint32_t *>(p) == 0);
  }
  // Remaining activation 0 frame and base caller frame are preserved intact
  assert(*reinterpret_cast<const volatile uint32_t *>(win0.hi - 32u) == 0x11111111u);
  assert(*reinterpret_cast<const volatile uint32_t *>(stack_lo + 0x3000u) == 0xC001CAFEu);

  // Caller window restored to activation 0
  assert(thread.stk.cur_win_lo == win0.lo);
  assert(thread.stk.cur_win_hi == win0.hi);
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
  thread.clear_window_at(0); // Dispatch activation has empty window
  thread.call_stack.depth = 1;

  context.user_sp = stack_hi - 0x200u;
  mock_kobj::callee_view view{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  auto err = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view, 0x1000u, 1);
  assert(err == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 2);
  assert(thread.window_at(1).is_valid());

  // Pop Call activation (returning to Dispatch activation)
  mock_frame *res_frame = nullptr;
  bool ok_pop = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, res_frame, true, false);
  assert(ok_pop);
  assert(thread.call_stack.depth == 1);
  // cur_win must NOT be {0, 0}; it must fall back to base thread stack [stack_lo, stack_hi)
  assert(thread.stk.cur_win_lo == stack_lo);
  assert(thread.stk.cur_win_hi == stack_hi);

  // Case 2: Call A (depth 1) -> Dispatch (depth 2, win={}) -> Call B (depth 3)
  thread.call_stack.depth = 0;
  context.user_sp = stack_hi - 0x100u;
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));
  const auto win_call_a = thread.window_at(0);
  assert(win_call_a.is_valid());

  // Dispatch activation pushed at depth 1
  thread.ledger[1].via = mock_kernel::LEDGER_DISPATCH;
  thread.clear_window_at(1);
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
  assert(thread.stk.cur_win_lo == win_call_a.lo);
  assert(thread.stk.cur_win_hi == win_call_a.hi);
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
  assert(thread.stk.cur_win_lo == stack_lo);
  assert(thread.stk.cur_win_hi == stack_hi);

  // Enter Call activation: win = [0x20003E00, 0x20003F00)
  context.user_sp = stack_hi - 0x100u;
  mock_kobj::callee_view view{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  assert((shizuku::native::enter_in_place<mock_kernel, mock_kobj>(kernel, thread, context, view, 0x1000u, 1) == shizuku::kernel_error::OK));
  assert(thread.call_stack.depth == 1);
  const auto initial_win = thread.window_at(0);
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

// M-mode callee does not open stack window, does not apply PMP, and zero_window is NOT called on return,
// protecting caller's trap frame (at ret_sp - 128) from being corrupted.
void test_m_mode_callee_no_window_and_frame_protected() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_context context{};
  shizuku::templates::thread<mock_context> thread{};
  shizuku::templates::thread<mock_context>::call_ledger_entry ledger_mem[8]{};
  thread.ledger = ledger_mem;
  thread.ledger_capacity = 8;
  thread.base_object = 1;

  // U-stack bounds
  const uintptr_t u_stack_lo = 0x20000000u;
  const uintptr_t u_stack_hi = 0x20004000u;
  thread.set_user_stack(u_stack_lo, u_stack_hi);

  // Kernel stack for M caller
  alignas(16) static uint8_t dummy_kernel_stack[0x1000];
  const uintptr_t ret_sp = reinterpret_cast<uintptr_t>(dummy_kernel_stack + 0x800);
  context.user_sp = ret_sp;
  context.privileged = true;

  // Simulate live trap frame words around ret_sp - 128
  volatile uint32_t *trap_frame_mepc = reinterpret_cast<volatile uint32_t *>(ret_sp - 128u);
  volatile uint32_t *trap_frame_ra   = reinterpret_cast<volatile uint32_t *>(ret_sp - 64u);
  volatile uint32_t *trap_frame_word = reinterpret_cast<volatile uint32_t *>(ret_sp - 4u);
  *trap_frame_mepc = 0x42001234u;
  *trap_frame_ra   = 0x42005678u;
  *trap_frame_word = 0xC001CAFEu;

  // M-mode callee
  mock_kobj::callee_view view_m{2, 0, 0, shizuku::object_kind::PLAIN, true /* privileged */, 0, 0, 1};

  // 1. Enter M-mode callee (M -> M)
  auto err = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_m, 0x42002000u, mock_kernel::LEDGER_CALL);
  assert(err == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 1);
  assert(!thread.window_at(0).is_valid());
  assert(test_fake_arch::applied_windows.empty());

  // 2. Return from M-mode callee (M -> M)
  mock_frame *frame_ptr = nullptr;
  mock_kobj kobj{};
  bool pop_ok = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, frame_ptr, true, false);
  assert(pop_ok);
  assert(thread.call_stack.depth == 0);

  // (1) Zeroing function must NOT be called for M-mode callee
  assert(test_fake_arch::zeroed_windows.empty());
  // (2) PMP apply must NOT be called for M-mode caller
  assert(test_fake_arch::applied_windows.empty());

  // (3) Live trap frame words must remain completely uncorrupted
  assert(*trap_frame_mepc == 0x42001234u);
  assert(*trap_frame_ra   == 0x42005678u);
  assert(*trap_frame_word == 0xC001CAFEu);

  // 3. M caller calling U callee (M -> U) where caller ret_sp is on kernel stack (outside U stack)
  mock_kobj::callee_view view_u{3, 0, 0, shizuku::object_kind::PLAIN, false /* unprivileged */, 0, 0, 1};
  context.user_sp = ret_sp; // Kernel stack, outside [u_stack_lo, u_stack_hi)
  auto err_m_to_u = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_u, 0x42003000u, mock_kernel::LEDGER_CALL);
  // Must be rejected with NO_STACK to protect kernel stack from being exposed as U stack window
  assert(err_m_to_u == shizuku::kernel_error::NO_STACK);
  assert(thread.call_stack.depth == 0);
}

// thread の stack 登録 (spawn / bootstrap が set_stack_bounds を呼ぶ): lo は 8B 切上げ、hi は 8B 切下げ、
// 窓は全域に戻る。
static void test_set_stack_bounds_alignment() {
  mock_kernel::THREAD thread{};
  thread.set_stack_bounds(0x20000003u, 0x20001005u);
  assert(thread.stk.stack_lo == 0x20000008u);
  assert(thread.stk.stack_hi == 0x20001000u);
  assert(thread.stk.cur_win_lo == thread.stk.stack_lo);
  assert(thread.stk.cur_win_hi == thread.stk.stack_hi);
  // 8B 整列済みならそのまま
  thread.set_stack_bounds(0x20002000u, 0x20003000u);
  assert(thread.stk.stack_lo == 0x20002000u && thread.stk.stack_hi == 0x20003000u);
  // 狭すぎて整列後に hi < lo なら空 (hi = lo)
  thread.set_stack_bounds(0x20004001u, 0x20004006u);
  assert(thread.stk.stack_hi >= thread.stk.stack_lo);
}

// M caller が U callee を呼ぶ (M→U)。thread の stack = M が実際に走る stack そのもの。
// 窓は [ret_sp-256, ret_sp) で開き、return でゼロ化され、ret_sp 以上の語は保存される。
// 範囲外の ret_sp は NO_STACK。
static void test_m_caller_u_callee_window() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  mock_context context{};
  mock_kernel::THREAD thread{};
  mock_kernel::THREAD::call_ledger_entry ledger_mem[8]{};
  thread.ledger = ledger_mem;
  thread.ledger_capacity = 8;
  thread.base_object = 1;

  alignas(16) static uint8_t m_thread_stack[0x1000];
  const uintptr_t lo = reinterpret_cast<uintptr_t>(m_thread_stack);
  const uintptr_t hi = lo + sizeof(m_thread_stack);
  thread.set_stack_bounds(lo, hi);
  assert(thread.stk.stack_lo == lo && thread.stk.stack_hi == hi);

  // M caller の sp は thread stack の内側 (trap frame 等がこの直上に載る)
  const uintptr_t ret_sp = lo + 0xC00u;
  context.privileged = true;
  context.user_sp = ret_sp;
  volatile uint32_t *live_above = reinterpret_cast<volatile uint32_t *>(ret_sp);
  volatile uint32_t *live_top = reinterpret_cast<volatile uint32_t *>(ret_sp + 0x20u);
  *live_above = 0x4D43414Cu;
  *live_top = 0x4D544F50u;
  // 窓になる位置には「前の利用者の残り」を置き、return でゼロ化されることを見る
  for (uintptr_t p = ret_sp - 256u; p < ret_sp; p += 4)
    *reinterpret_cast<volatile uint32_t *>(p) = 0xDEADBEEFu;
  // 窓の外 (窓の下) は触られない
  *reinterpret_cast<volatile uint32_t *>(ret_sp - 256u - 4u) = 0xBADBAD01u;

  mock_kobj::callee_view view_u{3, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  auto err = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_u, 0x42003000u, mock_kernel::LEDGER_CALL);
  assert(err == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 1);
  const auto win = thread.window_at(0);
  assert(win.is_valid());
  assert(win.hi == ret_sp);
  assert(win.lo == ret_sp - shizuku::kStackWindowStep);
  assert(thread.stk.cur_win_lo == win.lo && thread.stk.cur_win_hi == win.hi);
  assert(test_fake_arch::applied_windows.size() == 1);
  assert(test_fake_arch::applied_windows[0].lo == win.lo);
  assert(test_fake_arch::applied_windows[0].hi == win.hi);

  mock_frame *frame_ptr = nullptr;
  bool pop_ok = shizuku::native::pop_in_place<mock_kernel, mock_kobj>(
      kobj, thread, context, frame_ptr, true, false);
  assert(pop_ok);
  assert(thread.call_stack.depth == 0);
  // 開いた窓だけがゼロ化された
  assert(test_fake_arch::zeroed_windows.size() == 1);
  assert(test_fake_arch::zeroed_windows[0].lo == win.lo);
  assert(test_fake_arch::zeroed_windows[0].hi == win.hi);
  for (uintptr_t p = win.lo; p < win.hi; p += 4)
    assert(*reinterpret_cast<const volatile uint32_t *>(p) == 0);
  // ret_sp 以上の M の語は保存され、窓の下は触られていない
  assert(*live_above == 0x4D43414Cu);
  assert(*live_top == 0x4D544F50u);
  assert(*reinterpret_cast<const volatile uint32_t *>(ret_sp - 256u - 4u) == 0xBADBAD01u);

  // 範囲外の ret_sp (thread stack の外) は NO_STACK。台帳は書かれない。
  context.privileged = true;
  context.user_sp = lo - 16u;
  auto below = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_u, 0x42003000u, mock_kernel::LEDGER_CALL);
  assert(below == shizuku::kernel_error::NO_STACK);
  context.user_sp = hi + 16u;
  auto above = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_u, 0x42003000u, mock_kernel::LEDGER_CALL);
  assert(above == shizuku::kernel_error::NO_STACK);
  assert(thread.call_stack.depth == 0);
  // 境界ちょうど hi は範囲内 (sp = 領域の上端)
  context.user_sp = hi;
  auto at_hi = shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view_u, 0x42003000u, mock_kernel::LEDGER_CALL);
  assert(at_hi == shizuku::kernel_error::OK);
}

// 窓機構を持たない arch (HAS_STACK_WINDOW を宣言しない = ARM 相当): 窓の記帳・範囲検査・ゼロ化・
// 適用を一切しない。stack が thread 外でも NO_STACK にならず、thread の大きさも増えない。
struct nowin_frame { uintptr_t pc = 0; uintptr_t ra = 0; };
struct nowin_context {
  static constexpr bool CALL_HEADER_ON_STACK = true; // HAS_STACK_WINDOW は宣言しない
  nowin_frame frame{};
  nowin_frame *sp = &frame;
  uintptr_t user_sp = 0;
  bool privileged = false;
};
struct nowin_arch {
  static constexpr bool CALL_IN_PLACE = true;
  static inline int zero_calls = 0;
  static inline int apply_calls = 0;
  static void zero_stack_window(shizuku::stack_window) { ++zero_calls; }
  static void apply_stack_window(nowin_context &, uintptr_t, uintptr_t) { ++apply_calls; }
  static bool call_header_has_room(const nowin_context &, uint32_t) { return true; }
  static uintptr_t frame_pc(const nowin_frame &f) { return f.pc; }
  static uintptr_t call_return_sp(const nowin_context &c) { return c.user_sp; }
  static bool context_privileged(const nowin_context &c) { return c.privileged; }
  static void set_entry(nowin_frame &f, uintptr_t pc, uintptr_t ra) { f.pc = pc; f.ra = ra; }
  static uintptr_t return_stub() { return 0x1000u; }
  static void set_priv(nowin_context &c, bool p) { c.privileged = p; }
  static void set_region_window(nowin_context &, uintptr_t, uintptr_t) {}
  static void switch_callee_protection(nowin_context &, nowin_frame &, uint32_t, bool) {}
  static nowin_frame *call_resume_at(nowin_context &c, uintptr_t pc, uintptr_t sp, bool priv,
                                     uintptr_t, uintptr_t, bool) {
    c.frame.pc = pc; c.user_sp = sp; c.privileged = priv; return &c.frame;
  }
};
struct nowin_kernel {
  using ARCH = nowin_arch;
  using CONTEXT = nowin_context;
  using FRAME = nowin_frame;
  using THREAD = shizuku::templates::thread<nowin_context>;
  static constexpr uint8_t LEDGER_CALL = 1;
  static constexpr uint8_t LEDGER_DISPATCH = 2;
  static constexpr uint8_t LEDGER_DESCRIPTOR_CALL = 3;
  uint32_t svc_handler_object() const { return 0xFFFFu; }
};

static void test_gate_false_no_window_no_zero() {
  static_assert(!nowin_kernel::THREAD::HAS_STACK_WINDOW);
  static_assert(nowin_kernel::THREAD::MAX_STACK_WINDOWS == 0);
  static_assert(mock_kernel::THREAD::HAS_STACK_WINDOW);
  // thread のメモリ増分: gate=false は窓の記帳領域 (34 x u16) と stack 範囲 (4 語) を持たない。
  static_assert(sizeof(mock_kernel::THREAD) >=
                sizeof(nowin_kernel::THREAD) + 34u * sizeof(uint16_t) + 4u * sizeof(uintptr_t));
  static_assert(std::is_empty_v<decltype(nowin_kernel::THREAD{}.stk)>);
  static_assert(std::is_empty_v<decltype(nowin_kernel::THREAD{}.win_span)>);

  nowin_arch::zero_calls = nowin_arch::apply_calls = 0;
  nowin_kernel kernel{};
  mock_kobj kobj{};
  nowin_context context{};
  nowin_kernel::THREAD thread{};
  nowin_kernel::THREAD::call_ledger_entry ledger_mem[8]{};
  thread.ledger = ledger_mem;
  thread.ledger_capacity = 8;
  thread.base_object = 1;
  thread.set_stack_bounds(0x20000000u, 0x20001000u); // gate=false では何もしない

  // 深さが 34 を超えても (MAX_STACK_WINDOWS == 0 でも) NO_STACK にならず、ret_sp が任意の値でも通る
  context.user_sp = 0x10u; // 窓機構があれば open_window が失敗する値
  mock_kobj::callee_view view{2, 0, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1};
  auto err = shizuku::native::enter_in_place<nowin_kernel, mock_kobj>(
      kernel, thread, context, view, 0x1000u, 1);
  assert(err == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 1);
  assert(!thread.window_at(0).is_valid());
  assert(nowin_arch::apply_calls == 0);

  nowin_frame *frame_ptr = nullptr;
  mock_kobj kobj2{};
  bool pop_ok = shizuku::native::pop_in_place<nowin_kernel, mock_kobj>(
      kobj2, thread, context, frame_ptr, true, false);
  assert(pop_ok);
  assert(thread.call_stack.depth == 0);
  assert(nowin_arch::zero_calls == 0);
  assert(nowin_arch::apply_calls == 0);
  (void)kobj;
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
  test_m_mode_callee_no_window_and_frame_protected();
  test_set_stack_bounds_alignment();
  test_m_caller_u_callee_window();
  test_gate_false_no_window_no_zero();
  return 0;
}
