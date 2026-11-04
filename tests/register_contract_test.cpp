#include <cassert>
#include <cstdio>
#include "shizuku/register_isolation_contract.hpp"

using namespace shizuku::abi;

int main() {
  // ARMv8-M Contract verification
  static_assert(sizeof(armv8m_return_contract) / sizeof(armv8m_return_contract[0]) == 16);
  assert(count_return_payload(armv8m_return_contract) == 2);
  assert(count_scrubbed_zero(armv8m_return_contract) == 1);
  assert(count_passthrough(armv8m_return_contract) == 3);
  assert(count_callee_saved(armv8m_return_contract) == 8);
  assert(armv8m_return_contract[0].name == "r0" && armv8m_return_contract[0].role == reg_role::RETURN_ERROR);
  assert(armv8m_return_contract[1].name == "r1" && armv8m_return_contract[1].role == reg_role::RETURN_VALUE);
  assert(armv8m_return_contract[2].name == "r2" && armv8m_return_contract[2].role == reg_role::PASSTHROUGH);
  assert(armv8m_return_contract[3].name == "r3" && armv8m_return_contract[3].role == reg_role::PASSTHROUGH);
  for (size_t i = 4; i <= 11; ++i) {
    assert(armv8m_return_contract[i].role == reg_role::CALLEE_SAVED);
  }
  assert(armv8m_return_contract[12].name == "r12" && armv8m_return_contract[12].role == reg_role::PASSTHROUGH);
  assert(armv8m_return_contract[13].name == "sp" && armv8m_return_contract[13].role == reg_role::PRESERVED_SP);
  assert(armv8m_return_contract[14].name == "lr" && armv8m_return_contract[14].role == reg_role::SCRUBBED_ZERO);
  assert(armv8m_return_contract[15].name == "pc" && armv8m_return_contract[15].role == reg_role::RESTORED_PC);

  // RV32 Contract verification
  static_assert(sizeof(rv32_return_contract) / sizeof(rv32_return_contract[0]) == 32);
  assert(count_return_payload(rv32_return_contract) == 2);
  assert(count_scrubbed_zero(rv32_return_contract) == 27);
  assert(rv32_return_contract[10].name == "a0" && rv32_return_contract[10].role == reg_role::RETURN_ERROR);
  assert(rv32_return_contract[11].name == "a1" && rv32_return_contract[11].role == reg_role::RETURN_VALUE);
  assert(rv32_return_contract[2].name == "sp" && rv32_return_contract[2].role == reg_role::PRESERVED_SP);
  assert(rv32_return_contract[3].name == "gp" && rv32_return_contract[3].role == reg_role::PRESERVED_SYS);
  assert(rv32_return_contract[4].name == "tp" && rv32_return_contract[4].role == reg_role::PRESERVED_SYS);

  std::printf("register_contract_test: PASS\n");
  return 0;
}
