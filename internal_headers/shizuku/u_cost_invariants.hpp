#pragma once
#include <cstdint>

namespace shizuku::selftest {

inline void account_selftest_check(uint32_t &passed, uint32_t &failed, bool ok) {
  if (ok) ++passed;
  else ++failed;
}

struct u_cost_eval_input_t {
  bool counter_enabled = false;
  uint32_t base_cycles = 0;
  uint32_t measure_cycles = 0;
  uint32_t final_chain_value = 0;
  uint32_t accumulated_error = 0;
  uint32_t verify_value = 0;
  uint32_t verify_error = 0;
  uint32_t verify_outer_error = 0;
  uint32_t verify_outer_value = 0;
  uint32_t desc_create_error = 0;
  uint32_t desc_create_outer_error = 0;
  uint32_t expected_rounds = 2048;
  uint32_t outer_error = 0;
  uint32_t outer_value = 0;
  uint32_t executed_rounds = 0;
  uint32_t baseline_outer_error = 0;
  uint32_t baseline_outer_value = 0;
  uint32_t baseline_executed_rounds = 0;
  uint32_t warmup_outer_error = 0;
  uint32_t warmup_rounds = 0;
};

inline bool evaluate_u_cost_invariants(const u_cost_eval_input_t &in) {
  if (!in.counter_enabled) return false;
  if (in.base_cycles == 0) return false;
  if (in.measure_cycles <= in.base_cycles) return false;
  if (in.final_chain_value != in.expected_rounds) return false;
  if (in.accumulated_error != 0) return false;
  if (in.verify_error != 0) return false;
  if (in.verify_outer_error != 0 || in.verify_outer_value != 1) return false;
  if (in.verify_value != 42) return false;
  if (in.desc_create_error != 0 || in.desc_create_outer_error != 0) return false;
  if (in.outer_error != 0 || in.outer_value != in.expected_rounds) return false;
  if (in.executed_rounds != in.expected_rounds) return false;
  if (in.baseline_outer_error != 0 || in.baseline_outer_value != in.expected_rounds ||
      in.baseline_executed_rounds != in.expected_rounds) return false;
  if (in.warmup_outer_error != 0 || in.warmup_rounds != 32) return false;
  return true;
}

struct u_cost_fp_invariants_input_t {
  uint32_t expected_fpca = 0;
  uint32_t priv_meth_base = 0;
  uint32_t u_meth_base_before = 0;
  uint32_t u_meth_base_after = 0;
  uint32_t priv_meth_meas = 0;
  uint32_t u_meth_meas_before = 0;
  uint32_t u_meth_meas_after = 0;
  uint32_t priv_desc_base = 0;
  uint32_t u_desc_base_before = 0;
  uint32_t u_desc_base_after = 0;
  uint32_t priv_desc_meas = 0;
  uint32_t u_desc_meas_before = 0;
  uint32_t u_desc_meas_after = 0;
};

inline bool evaluate_u_cost_fp_invariants(const u_cost_fp_invariants_input_t &in) {
  const auto exp = in.expected_fpca;
  if (in.priv_meth_base != exp || in.u_meth_base_before != exp || in.u_meth_base_after != exp) return false;
  if (in.priv_meth_meas != exp || in.u_meth_meas_before != exp || in.u_meth_meas_after != exp) return false;
  if (in.priv_desc_base != exp || in.u_desc_base_before != exp || in.u_desc_base_after != exp) return false;
  if (in.priv_desc_meas != exp || in.u_desc_meas_before != exp || in.u_desc_meas_after != exp) return false;
  return true;
}

} // namespace shizuku::selftest
