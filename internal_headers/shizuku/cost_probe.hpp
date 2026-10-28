#ifndef SHIZUKU_COST_PROBE_HPP
#define SHIZUKU_COST_PROBE_HPP

#include <cstdint>

namespace shizuku::cost_probe {

struct xip_snapshot { uint32_t hit, access; };
struct xip_delta { uint32_t hit, access, miss; bool valid, saturated; };

constexpr xip_delta difference(xip_snapshot before, xip_snapshot after) {
  const bool saturated = before.hit == UINT32_MAX || before.access == UINT32_MAX ||
                         after.hit == UINT32_MAX || after.access == UINT32_MAX;
  if (saturated || after.hit < before.hit || after.access < before.access)
    return {0, 0, 0, false, saturated};
  const uint32_t hit = after.hit - before.hit;
  const uint32_t access = after.access - before.access;
  if (hit > access) return {hit, access, 0, false, false};
  return {hit, access, access - hit, true, false};
}

// API numbers 0..33 and an out-of-range bucket. PendSV is intentionally not
// attributed to an API: a pend can be unrelated or coalesced.
inline constexpr unsigned api_count = 34;
inline constexpr unsigned unknown_api = api_count;
inline constexpr unsigned bucket_count = api_count + 1;
constexpr unsigned bucket(uint32_t api) { return api < api_count ? api : unknown_api; }
enum class route { direct, generic };

struct route_snapshot {
  uint32_t direct[bucket_count]{};
  uint32_t generic[bucket_count]{};
  uint32_t pendsv_unknown{};
};
struct delta_value { uint32_t value; bool valid; };
constexpr delta_value difference(uint32_t before, uint32_t after) {
  return before == UINT32_MAX || after == UINT32_MAX || after < before
      ? delta_value{0, false} : delta_value{after - before, true};
}

// Separate slots per core; only the enabled specialization owns storage.
// Aligned 32-bit volatile accesses are individually observable, not an atomic
// transaction across buckets. This is diagnostic, not synchronization.
template<bool Enabled> struct routes {
  static inline void record(unsigned, route, uint32_t) {}
  static inline void pendsv(unsigned) {}
  static inline route_snapshot snapshot(unsigned) { return {}; }
};
template<> struct routes<true> {
  struct counters {
    volatile uint32_t direct[bucket_count];
    volatile uint32_t generic[bucket_count];
    volatile uint32_t pendsv_unknown;
  };
  static inline counters core[2] = {};
  static inline void increment(volatile uint32_t &v) {
    const uint32_t old = v;
    if (old != UINT32_MAX) v = old + 1;
  }
  static inline void record(unsigned core_index, route r, uint32_t api) {
    auto &c = core[core_index & 1u];
    const unsigned index = bucket(api);
    increment(r == route::direct ? c.direct[index] : c.generic[index]);
  }
  static inline void pendsv(unsigned core_index) {
    increment(core[core_index & 1u].pendsv_unknown);
  }
  static inline route_snapshot snapshot(unsigned core_index) {
    const auto &c = core[core_index & 1u];
    route_snapshot result{};
    for (unsigned i = 0; i < bucket_count; ++i) {
      result.direct[i] = c.direct[i];
      result.generic[i] = c.generic[i];
    }
    result.pendsv_unknown = c.pendsv_unknown;
    return result;
  }
};

// Which exception classes a CFSR value implicates (ARMv7-M/v8-M layout:
// [7:0] MMFSR, [15:8] BFSR, [31:16] UFSR). CFSR == 0 means the fault arrived
// without a configurable-fault cause, so it is counted as a HardFault.
struct fault_class { bool memmanage, busfault, usagefault, hardfault; };
constexpr fault_class classify_cfsr(uint32_t cfsr) {
  return {(cfsr & 0x000000FFu) != 0, (cfsr & 0x0000FF00u) != 0,
          (cfsr & 0xFFFF0000u) != 0, cfsr == 0};
}
constexpr uint32_t saturating_increment(uint32_t value) {
  return value == UINT32_MAX ? value : value + 1;
}

} // namespace shizuku::cost_probe
#endif
