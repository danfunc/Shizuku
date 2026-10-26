// ===========================================================================
//  Host unit test for ARMv8-M in-place native templates & payload trait (S1)
// ===========================================================================
#include "arm_inplace_fixture.hpp"

#include "arch/armv8m/armv8m.hpp"
#include "arch/riscv/rv32.hpp"

// ---------------------------------------------------------------------------
// 1. Payload trait size & offset static checks (ARMv8-M and RV32)
// ---------------------------------------------------------------------------
namespace test_armv8m_payload {
using armv8m_raw = shizuku::archs::armv8m<8, 4>;

struct context_with_arm_payload : armv8m_raw::context_t {
  // S2 で本番 context_t が宣言する形を、arch 自身の inplace_payload_t で先取りして試す。
  using ledger_payload_t = armv8m_raw::inplace_payload_t;
};

static_assert(sizeof(armv8m_raw::inplace_payload_t) == 32, "r4-r11 = 8 words");
static_assert(std::is_same_v<shizuku::templates::ledger_payload_of<armv8m_raw::context_t>,
                             shizuku::empty_ledger_payload>,
              "S1: production ARMv8-M must not enable the payload yet");
static_assert(std::is_same_v<shizuku::templates::ledger_payload_of<context_with_arm_payload>,
                             armv8m_raw::inplace_payload_t>);

using thread_armv8m_empty = shizuku::templates::thread<armv8m_raw::context_t>;
using thread_armv8m_with_payload = shizuku::templates::thread<context_with_arm_payload>;

struct mock_int_mode {
  static bool in_interrupt() { return false; }
  static bool current_priv() { return true; }
};
using rv32_raw = shizuku::archs::rv32<16, 4, mock_int_mode>;
using thread_rv32_empty = shizuku::templates::thread<rv32_raw::context_t>;

#if defined(SHIZUKU_LEAN_LEDGER)
// ARMv8-M empty payload size check
static_assert(sizeof(thread_armv8m_empty::call_ledger_entry) == (sizeof(uintptr_t) == 4 ? 40 : 64),
              "LEAN ARMv8-M call_ledger_entry with empty payload must not increase in size");
// Real RV32 empty payload size check
static_assert(sizeof(thread_rv32_empty::call_ledger_entry) == (sizeof(uintptr_t) == 4 ? 40 : 64),
              "LEAN RV32 call_ledger_entry with empty payload must not increase in size");
// ARM payload size and offset check
static_assert(offsetof(thread_armv8m_with_payload::call_ledger_entry, payload) == (sizeof(uintptr_t) == 4 ? 40 : 60),
              "Payload offset must immediately follow reserved field");
static_assert(sizeof(thread_armv8m_with_payload::call_ledger_entry) == (sizeof(uintptr_t) == 4 ? 72 : 96),
              "LEAN call_ledger_entry with ARM payload size (40 + 32 on 32-bit)");
#else
static_assert(sizeof(thread_armv8m_empty::call_ledger_entry) == (sizeof(uintptr_t) == 4 ? 16 : 24),
              "Non-LEAN ARMv8-M call_ledger_entry with empty payload must not increase in size");
static_assert(sizeof(thread_rv32_empty::call_ledger_entry) == (sizeof(uintptr_t) == 4 ? 16 : 24),
              "Non-LEAN RV32 call_ledger_entry with empty payload must not increase in size");
static_assert(offsetof(thread_armv8m_with_payload::call_ledger_entry, payload) == (sizeof(uintptr_t) == 4 ? 16 : 20),
              "Payload offset must immediately follow unwind_floor");
static_assert(sizeof(thread_armv8m_with_payload::call_ledger_entry) == (sizeof(uintptr_t) == 4 ? 48 : 56),
              "Non-LEAN call_ledger_entry with ARM payload size (16 + 32 on 32-bit)");
#endif
} // namespace test_armv8m_payload

// ---------------------------------------------------------------------------
// 2. Production arch invariants
// ---------------------------------------------------------------------------
void test_armv8m_production_invariants() {
  using armv8m_raw = shizuku::archs::armv8m<8, 4>;
  using test_rv32_raw = shizuku::archs::rv32<16, 4, test_armv8m_payload::mock_int_mode>;

  // Production ARMv8-M must NOT have CALL_IN_PLACE == true
  assert(!shizuku::arch_call_in_place<armv8m_raw>());

  // RISC-V in-place call is enabled in production
  assert(shizuku::arch_call_in_place<test_rv32_raw>());
}

// ---------------------------------------------------------------------------
// 3. RV32 6-argument descriptor contrast test
// ---------------------------------------------------------------------------
void test_rv32_descriptor_contrast() {
  using test_rv32_raw = shizuku::archs::rv32<16, 4, test_armv8m_payload::mock_int_mode>;
  test_rv32_raw::exception_frame_t rv_frame{};
  rv_frame.a2 = 0x11;
  rv_frame.a3 = 0x12;
  rv_frame.a4 = 0x13;
  rv_frame.a5 = 0x14;
  rv_frame.a6 = 0x15;
  rv_frame.a7 = 0x16;
  test_rv32_raw::move_descriptor_arguments(rv_frame);
  assert(rv_frame.a0 == 0x11);
  assert(rv_frame.a1 == 0x12);
  assert(rv_frame.a2 == 0x13);
  assert(rv_frame.a3 == 0x14);
  assert(rv_frame.a4 == 0x15);
  assert(rv_frame.a5 == 0x16);
  printf("  [ARMv8-M/RV32] PASS: rv32_descriptor_contrast (6 args a2..a7 -> a0..a5)\n");
}

int main() {
  printf("Starting ARMv8-M in-place native templates & payload trait host test...\n");
  test_armv8m_production_invariants();

  using adapter = test_inplace_adapter<shizuku::archs::armv8m<8, 4>>;
  run_inplace_method_call_success<adapter>("ARMv8-M");
  run_inplace_descriptor_call_success<adapter>("ARMv8-M");
  test_rv32_descriptor_contrast();
  run_inplace_method_failures_ledger_invariant<adapter>("ARMv8-M");
  run_inplace_descriptor_failures_ledger_invariant<adapter>("ARMv8-M");
  run_inplace_lean_push_pop<adapter>("ARMv8-M");
  run_inplace_exit_validation<adapter>("ARMv8-M");

  printf("All ARMv8-M in-place host tests PASSED successfully!\n");
  return 0;
}
