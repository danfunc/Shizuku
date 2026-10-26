#include "shizuku/u_cost_invariants.hpp"
#include "shizuku/svc_breakdown_probe.hpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "FAIL: check '%s' failed at %s:%d\n", #cond, __FILE__, __LINE__); \
    abort(); \
  } \
} while(0)

int main() {
  using namespace shizuku::selftest;

  // 1. Healthy valid run
  u_cost_eval_input_t valid_run{
      .counter_enabled = true,
      .base_cycles = 10000,
      .measure_cycles = 3500000,
      .final_chain_value = 2048,
      .accumulated_error = 0,
      .verify_value = 42,
      .verify_error = 0,
      .verify_outer_error = 0,
      .verify_outer_value = 1,
      .desc_create_error = 0,
      .desc_create_outer_error = 0,
      .expected_rounds = 2048,
      .outer_error = 0,
      .outer_value = 2048,
      .executed_rounds = 2048,
      .baseline_outer_error = 0,
      .baseline_outer_value = 2048,
      .baseline_executed_rounds = 2048,
      .warmup_outer_error = 0,
      .warmup_rounds = 32,
  };
  CHECK(evaluate_u_cost_invariants(valid_run) == true);

  // 2. Stopped / disabled counter
  auto bad_counter_disabled = valid_run;
  bad_counter_disabled.counter_enabled = false;
  CHECK(evaluate_u_cost_invariants(bad_counter_disabled) == false);

  // 3. Counter zero (not progressed)
  auto bad_counter_zero = valid_run;
  bad_counter_zero.base_cycles = 0;
  CHECK(evaluate_u_cost_invariants(bad_counter_zero) == false);

  // 4. Measure cycles <= base cycles
  auto bad_measure_less = valid_run;
  bad_measure_less.measure_cycles = bad_measure_less.base_cycles;
  CHECK(evaluate_u_cost_invariants(bad_measure_less) == false);

  // 5. Broken chain value (e.g. 2047 instead of 2048)
  auto bad_chain_broken = valid_run;
  bad_chain_broken.final_chain_value = 2047;
  CHECK(evaluate_u_cost_invariants(bad_chain_broken) == false);

  // 6. Mid-loop error accumulated
  auto bad_accum_err = valid_run;
  bad_accum_err.accumulated_error = 6; // NO_STACK
  CHECK(evaluate_u_cost_invariants(bad_accum_err) == false);

  // 7. Verification value mismatch
  auto bad_verify_val = valid_run;
  bad_verify_val.verify_value = 41;
  CHECK(evaluate_u_cost_invariants(bad_verify_val) == false);

  // 8. Verification error returned
  auto bad_verify_err = valid_run;
  bad_verify_err.verify_error = 19; // NOT_PRIVILEGED
  CHECK(evaluate_u_cost_invariants(bad_verify_err) == false);

  // 9. Descriptor creation failure (inner error)
  auto bad_desc_create = valid_run;
  bad_desc_create.desc_create_error = 20; // NO_DESCRIPTOR
  CHECK(evaluate_u_cost_invariants(bad_desc_create) == false);

  // 10. Outer dispatch error failure
  auto bad_outer_err = valid_run;
  bad_outer_err.outer_error = 1; // KERNEL_REFUSED
  CHECK(evaluate_u_cost_invariants(bad_outer_err) == false);

  // 11. Executed rounds mismatch (e.g. 2040 instead of 2048)
  auto bad_exec_rounds = valid_run;
  bad_exec_rounds.executed_rounds = 2040;
  CHECK(evaluate_u_cost_invariants(bad_exec_rounds) == false);

  // 12. Warmup rounds mismatch (e.g. 0 instead of 32)
  auto bad_warmup_rounds = valid_run;
  bad_warmup_rounds.warmup_rounds = 0;
  CHECK(evaluate_u_cost_invariants(bad_warmup_rounds) == false);

  auto bad_outer_error = valid_run;
  bad_outer_error.outer_error = 1;
  CHECK(evaluate_u_cost_invariants(bad_outer_error) == false);
  auto bad_outer_value = valid_run;
  bad_outer_value.outer_value = 0;
  CHECK(evaluate_u_cost_invariants(bad_outer_value) == false);
  auto bad_baseline_rounds = valid_run;
  bad_baseline_rounds.baseline_executed_rounds = 2047;
  CHECK(evaluate_u_cost_invariants(bad_baseline_rounds) == false);
  auto bad_warmup_outer_error = valid_run;
  bad_warmup_outer_error.warmup_outer_error = 1;
  CHECK(evaluate_u_cost_invariants(bad_warmup_outer_error) == false);
  auto bad_verify_outer = valid_run;
  bad_verify_outer.verify_outer_value = 0;
  CHECK(evaluate_u_cost_invariants(bad_verify_outer) == false);
  auto bad_descriptor_outer = valid_run;
  bad_descriptor_outer.desc_create_outer_error = 1;
  CHECK(evaluate_u_cost_invariants(bad_descriptor_outer) == false);

  // Four corrected checks must contribute four passes on top of prior results.
  uint32_t passed = 290;
  uint32_t failed = 0;
  account_selftest_check(passed, failed, true);
  account_selftest_check(passed, failed, true);
  account_selftest_check(passed, failed, true);
  account_selftest_check(passed, failed, true);
  CHECK(passed == 294 && failed == 0);

  // Probe capture is armed only for the target API and accepts a wrapping timer.
  svc_breakdown_probe_state_t probes{};
  svc_breakdown_probe_state_t other_core{};
  arm_svc_breakdown_probe(probes, 1, 2);
  observe_svc_breakdown_probe(other_core, 0, SVC_BREAKDOWN_API_GET_CURRENT_OBJECT,
                              1, 2, 0xFFFFFFFDu, 0x20001000u, 1);
  CHECK(other_core.hits[0] == 0 && !other_core.invalid && probes.armed);
  observe_svc_breakdown_probe(probes, 0, 4, 1, 2, 0xFFFFFFFDu, 0x20001000u, 0xFFFFFFF0u);
  CHECK(probes.hits[0] == 0 && !probes.invalid && probes.armed);
  observe_svc_breakdown_probe(probes, 0, SVC_BREAKDOWN_API_GET_CURRENT_OBJECT,
                              9, 2, 0xFFFFFFFDu, 0x20001000u, 0xFFFFFFF0u);
  CHECK(probes.hits[0] == 0 && probes.armed);
  observe_svc_breakdown_probe(probes, 0, SVC_BREAKDOWN_API_GET_CURRENT_OBJECT,
                              1, 2, 0xFFFFFFFDu, 0x20001000u, 0xFFFFFFF0u);
  observe_svc_breakdown_probe(probes, 1, 0, 1, 2, 0, 0, 0xFFFFFFF8u);
  observe_svc_breakdown_probe(probes, 2, 0, 1, 2, 0, 0, 0x00000004u);
  observe_svc_breakdown_probe(probes, 3, 0, 1, 2, 0, 0, 0x0000000Cu);
  observe_svc_breakdown_probe(probes, 4, 0, 1, 2, 0, 0, 0x00000014u);
  observe_svc_breakdown_probe(probes, 5, 0, 1, 2, 0, 0, 0x0000001Cu);
  observe_svc_breakdown_probe(probes, 6, 0, 1, 2, 0, 0x20001000u, 0x00000024u);
  CHECK(svc_breakdown_sample_ok(probes, 1));
  disarm_svc_breakdown_probe(probes);
  observe_svc_breakdown_probe(probes, 0, SVC_BREAKDOWN_API_GET_CURRENT_OBJECT,
                              1, 2, 0xFFFFFFFDu, 0x20001000u, 0x00000030u);
  CHECK(probes.hits[0] == 1 && !probes.active && !probes.armed);

  // Wrong owner, duplicate point, and missing point all fail closed.
  probes = {};
  arm_svc_breakdown_probe(probes, 1, 2);
  observe_svc_breakdown_probe(probes, 0, SVC_BREAKDOWN_API_GET_CURRENT_OBJECT,
                              1, 2, 0xFFFFFFFDu, 0x20001000u, 10);
  observe_svc_breakdown_probe(probes, 1, 0, 9, 2, 0, 0, 11);
  CHECK(probes.invalid && !probes.active && probes.next_point == 1 &&
        probes.hits[0] == 1 && probes.hits[1] == 0);
  probes = {};
  arm_svc_breakdown_probe(probes, 1, 2);
  observe_svc_breakdown_probe(probes, 0, SVC_BREAKDOWN_API_GET_CURRENT_OBJECT,
                              1, 2, 0xFFFFFFFDu, 0x20001000u, 20);
  observe_svc_breakdown_probe(probes, 1, 0, 1, 2, 0, 0, 21);
  observe_svc_breakdown_probe(probes, 1, 0, 1, 2, 0, 0, 22);
  CHECK(probes.invalid && !probes.active && probes.next_point == 2 &&
        probes.hits[0] == 1 && probes.hits[1] == 1);
  probes = {};
  arm_svc_breakdown_probe(probes, 1, 2);
  observe_svc_breakdown_probe(probes, 0, SVC_BREAKDOWN_API_GET_CURRENT_OBJECT,
                              1, 2, 0xFFFFFFFDu, 0x20001000u, 30);
  CHECK(!probes.invalid && probes.active && !probes.armed &&
        probes.next_point == 1 && probes.hits[0] == 1 && probes.completed == 0);
  disarm_svc_breakdown_probe(probes);
  CHECK(!probes.active && !probes.armed);
  probes = {};
  arm_svc_breakdown_probe(probes, 1, 2);
  observe_svc_breakdown_probe(probes, 0, SVC_BREAKDOWN_API_GET_CURRENT_OBJECT,
                              1, 2, 0xFFFFFFFDu, 0x20001000u, 40);
  observe_svc_breakdown_probe(probes, 2, 0, 1, 2, 0, 0, 42);
  CHECK(probes.invalid && !probes.active && probes.next_point == 1 &&
        probes.hits[0] == 1 && probes.hits[2] == 0);
  probes = {};
  arm_svc_breakdown_probe(probes, 1, 2);
  observe_svc_breakdown_probe(probes, 0, SVC_BREAKDOWN_API_GET_CURRENT_OBJECT,
                              1, 2, 0xFFFFFFFDu, 0x20001000u, 50);
  observe_svc_breakdown_probe(probes, 1, 0, 1, 2, 0, 0, 51);
  observe_svc_breakdown_probe(probes, 2, 0, 1, 2, 0, 0, 52);
  observe_svc_breakdown_probe(probes, 3, 0, 1, 2, 0, 0, 53);
  observe_svc_breakdown_probe(probes, 4, 0, 1, 2, 0, 0, 54);
  observe_svc_breakdown_probe(probes, 5, 0, 1, 2, 0, 0, 55);
  observe_svc_breakdown_probe(probes, 6, 0, 1, 2, 0, 0x20001004u, 56);
  CHECK(probes.invalid && !probes.active && probes.next_point == 6 &&
        probes.hits[5] == 1 && probes.hits[6] == 0);
  probes = {};
  arm_svc_breakdown_probe(probes, 1, 2);
  observe_svc_breakdown_probe(probes, 0, SVC_BREAKDOWN_API_GET_CURRENT_OBJECT,
                              1, 2, 0xFFFFFFFDu, 0x20001000u, 60);
  observe_svc_breakdown_probe(probes, 1, 0, 1, 2, 0, 0, 60);
  CHECK(probes.invalid && !probes.active && probes.hits[1] == 0);

  printf("[PASS] test_u_cost_invariants: invariants, corrected counters, and SVC probe state verified (always-on CHECK)\n");
  return 0;
}
