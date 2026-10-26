// ===========================================================================
//  Host unit test for ARMv6-M in-place native templates & payload trait (S1)
// ===========================================================================
#include "arm_inplace_fixture.hpp"

#include "arch/armv6m/armv6m.hpp"

// ---------------------------------------------------------------------------
// 1. ARMv6-M Payload trait size & offset static checks
// ---------------------------------------------------------------------------
namespace test_armv6m_payload {
using armv6m_raw = shizuku::archs::armv6m<8, 2>;

struct context_with_arm_payload : armv6m_raw::context_t {
  // S2 で本番 context_t が宣言する形を、arch 自身の inplace_payload_t で先取りして試す。
  using ledger_payload_t = armv6m_raw::inplace_payload_t;
};

static_assert(sizeof(armv6m_raw::inplace_payload_t) == 32, "r4-r11 = 8 words");
static_assert(std::is_same_v<shizuku::templates::ledger_payload_of<armv6m_raw::context_t>,
                             shizuku::empty_ledger_payload>,
              "S1: production ARMv6-M must not enable the payload yet");
static_assert(std::is_same_v<shizuku::templates::ledger_payload_of<context_with_arm_payload>,
                             armv6m_raw::inplace_payload_t>);

using thread_armv6m_empty = shizuku::templates::thread<armv6m_raw::context_t>;
using thread_armv6m_with_payload = shizuku::templates::thread<context_with_arm_payload>;

#if defined(SHIZUKU_LEAN_LEDGER)
static_assert(sizeof(thread_armv6m_empty::call_ledger_entry) == (sizeof(uintptr_t) == 4 ? 40 : 64),
              "LEAN ARMv6-M call_ledger_entry with empty payload must not increase in size");
static_assert(offsetof(thread_armv6m_with_payload::call_ledger_entry, payload) == (sizeof(uintptr_t) == 4 ? 40 : 60),
              "Payload offset must immediately follow reserved field");
static_assert(sizeof(thread_armv6m_with_payload::call_ledger_entry) == (sizeof(uintptr_t) == 4 ? 72 : 96),
              "LEAN call_ledger_entry with ARM payload size (40 + 32 on 32-bit)");
#else
static_assert(sizeof(thread_armv6m_empty::call_ledger_entry) == (sizeof(uintptr_t) == 4 ? 16 : 24),
              "Non-LEAN ARMv6-M call_ledger_entry with empty payload must not increase in size");
static_assert(offsetof(thread_armv6m_with_payload::call_ledger_entry, payload) == (sizeof(uintptr_t) == 4 ? 16 : 20),
              "Payload offset must immediately follow unwind_floor");
static_assert(sizeof(thread_armv6m_with_payload::call_ledger_entry) == (sizeof(uintptr_t) == 4 ? 48 : 56),
              "Non-LEAN call_ledger_entry with ARM payload size (16 + 32 on 32-bit)");
#endif
} // namespace test_armv6m_payload

// ---------------------------------------------------------------------------
// 2. Production arch invariants
// ---------------------------------------------------------------------------
void test_armv6m_production_invariants() {
  using armv6m_raw = shizuku::archs::armv6m<8, 2>;

  // Production ARMv6-M must NOT have CALL_IN_PLACE == true
  assert(!shizuku::arch_call_in_place<armv6m_raw>());
}

// ---------------------------------------------------------------------------
// 3. Pure frame and context operations on ARMv6-M
// ---------------------------------------------------------------------------
void test_armv6m_pure_operations() {
  using armv6m_t = shizuku::archs::armv6m<8, 2>;
  armv6m_t::exception_frame_t frame{};
  armv6m_t::context_t ctx{};

  // set_method_argument sets r0
  armv6m_t::set_method_argument(frame, 0x12345678u);
  assert(frame.r0 == 0x12345678u);

  // move_descriptor_arguments moves r2 to r0 (D2(a) ARM 1-argument)
  frame.r2 = 0x9ABCDEF0u;
  armv6m_t::move_descriptor_arguments(frame);
  assert(frame.r0 == 0x9ABCDEF0u);

  // context_privileged reads control register bit 0
  ctx.control = armv6m_t::CONTROL_PRIV_PSP;
  assert(armv6m_t::context_privileged(ctx));
  ctx.control = armv6m_t::CONTROL_UNPRIV_PSP;
  assert(!armv6m_t::context_privileged(ctx));

  // frame_pc reads pc
  frame.pc = 0x10008000u;
  assert(armv6m_t::frame_pc(frame) == 0x10008000u);

  printf("  [ARMv6-M] PASS: armv6m_pure_operations\n");
}

int main() {
  printf("Starting ARMv6-M in-place native templates & payload trait host test...\n");
  test_armv6m_production_invariants();
  test_armv6m_pure_operations();

  using adapter = test_inplace_adapter<shizuku::archs::armv6m<8, 2>>;
  run_inplace_method_call_success<adapter>("ARMv6-M");
  run_inplace_descriptor_call_success<adapter>("ARMv6-M");
  run_inplace_method_failures_ledger_invariant<adapter>("ARMv6-M");
  run_inplace_descriptor_failures_ledger_invariant<adapter>("ARMv6-M");
  run_inplace_lean_push_pop<adapter>("ARMv6-M");
  run_inplace_exit_validation<adapter>("ARMv6-M");

  printf("All ARMv6-M in-place host tests PASSED successfully!\n");
  return 0;
}
