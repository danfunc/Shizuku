#ifndef SHIZUKU_TIME_COST_RECORD_HPP
#define SHIZUKU_TIME_COST_RECORD_HPP
#include <cstdint>
// Fixed 80-byte JTAG record, version 1. Totals include the enclosing M->U call;
// subtract totals before division so rounding does not bias net_mean.
extern "C" {
struct shizuku_time_cost_record_t {
  uint32_t magic, version, done, rounds;
  uint32_t counter_enabled, baseline_total, raw_total, baseline_mean;
  uint32_t raw_mean, net_mean, ok, errors;
  uint32_t first_low, first_high, last_low, last_high;
  uint32_t before_low, before_high, after_low, after_high;
};
extern volatile shizuku_time_cost_record_t shizuku_time_cost_record;
}
static_assert(sizeof(shizuku_time_cost_record_t) == 80);
#endif
