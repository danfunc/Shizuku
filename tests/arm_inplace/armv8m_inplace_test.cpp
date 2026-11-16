// ===========================================================================
//  Host unit test for ARMv8-M in-place native templates (STACK 方式の記録)
// ===========================================================================
//  旧版は LEAN の台帳項目 (compact entry、payload、20B / 32B / 64B の寸法) と旧 call-header 方式の寸法も
//  検査していた。どちらも廃止し、呼出しの記録は caller の stack 枠の 32B (stack_call_record) だけになった。
#include "arm_inplace_fixture.hpp"

#include "arch/armv8m/armv8m.hpp"
#include "arch/riscv/rv32.hpp"
#include "shizuku/method_entry.hpp"

extern "C" void shizuku_armv8m_call_entry() {}
extern "C" void shizuku_armv8m_return_stub() {}

static uintptr_t registered_word(uintptr_t a, uintptr_t, uintptr_t, uintptr_t) {
  return a + 1;
}

// ---------------------------------------------------------------------------
// 1. 記録の寸法 (ARMv8-M と RV32)。32bit のときだけ 32B ちょうど。
// ---------------------------------------------------------------------------
namespace test_armv8m_record {
using armv8m_raw = shizuku::archs::armv8m<8, 4>;

struct mock_int_mode {
  static bool in_interrupt() { return false; }
  static bool current_priv() { return true; }
};
using rv32_raw = shizuku::archs::rv32<16, 4, mock_int_mode>;
using thread_armv8m = shizuku::templates::thread<armv8m_raw::context_t>;
using thread_rv32 = shizuku::templates::thread<rv32_raw::context_t>;

static_assert(sizeof(uintptr_t) != 4 || sizeof(thread_armv8m::call_ledger_entry) == 32,
              "STACK call record (ARMv8-M) must be 32 bytes");
static_assert(sizeof(uintptr_t) != 4 || sizeof(thread_rv32::call_ledger_entry) == 32,
              "STACK call record (RV32) must be 32 bytes");
static_assert(armv8m_raw::CALL_RECORD_BYTES == sizeof(thread_armv8m::call_ledger_entry) || sizeof(uintptr_t) != 4,
              "ARMv8-M record slot must hold exactly one record on 32-bit");
} // namespace test_armv8m_record

// ---------------------------------------------------------------------------
// 2. Production arch invariants
// ---------------------------------------------------------------------------
void test_armv8m_production_invariants() {
  using armv8m_raw = shizuku::archs::armv8m<8, 4>;
  using test_rv32_raw = shizuku::archs::rv32<16, 4, test_armv8m_record::mock_int_mode>;

  assert(shizuku::arch_call_in_place<armv8m_raw>());
  const auto address = shizuku::method_entry<armv8m_raw, &registered_word>();
  using entry = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
  assert(reinterpret_cast<entry>(address)(41, 0, 0, 0, 0) == 42);

  // RISC-V in-place call is enabled in production
  assert(shizuku::arch_call_in_place<test_rv32_raw>());
}

// ---------------------------------------------------------------------------
// 3. RV32 6-argument descriptor contrast test
// ---------------------------------------------------------------------------
void test_rv32_descriptor_contrast() {
  using test_rv32_raw = shizuku::archs::rv32<16, 4, test_armv8m_record::mock_int_mode>;
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
  printf("Starting ARMv8-M in-place native templates host test (STACK records)...\n");
  test_armv8m_production_invariants();

  using adapter = test_inplace_adapter<shizuku::archs::armv8m<8, 4>>;
  run_inplace_method_call_success<adapter>("ARMv8-M");
  run_inplace_descriptor_call_success<adapter>("ARMv8-M");
  test_rv32_descriptor_contrast();
  run_inplace_method_failures_ledger_invariant<adapter>("ARMv8-M");
  run_inplace_descriptor_failures_ledger_invariant<adapter>("ARMv8-M");
  run_inplace_stack_push_pop<adapter>("ARMv8-M");
  run_inplace_return_window_current_row<adapter>("ARMv8-M");
  run_inplace_exit_validation<adapter>("ARMv8-M");

  printf("All ARMv8-M in-place host tests PASSED successfully!\n");
  return 0;
}
