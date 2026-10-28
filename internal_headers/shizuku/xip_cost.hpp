#ifndef SHIZUKU_XIP_COST_HPP
#define SHIZUKU_XIP_COST_HPP

#include <cstdint>
#include "shizuku/config.hpp"
#include "shizuku/cost_probe.hpp"

namespace shizuku::selftest {
inline constexpr bool xip_cost_enabled = xip_cost_probe_enabled;
using cost_probe::xip_snapshot;

inline xip_snapshot xip_cost_snapshot() {
  if constexpr (xip_cost_enabled) {
    asm volatile("" ::: "memory");
    const auto hit = *reinterpret_cast<volatile const uint32_t *>(0x400c800cu);
    const auto access = *reinterpret_cast<volatile const uint32_t *>(0x400c8010u);
    asm volatile("" ::: "memory");
    return {hit, access};
  }
  return {};
}

// Reset outside the cycle window so earlier selftests do not leave either
// saturating counter pinned. These global counters are shared with both cores.
inline xip_snapshot xip_cost_start() {
  if constexpr (xip_cost_enabled) {
    *reinterpret_cast<volatile uint32_t *>(0x400c800cu) = 0;
    *reinterpret_cast<volatile uint32_t *>(0x400c8010u) = 0;
    asm volatile("" ::: "memory");
    return xip_cost_snapshot();
  }
  return {};
}

template<class Board>
inline void xip_cost_report(const char *name, xip_snapshot before, xip_snapshot after) {
  if constexpr (xip_cost_enabled) {
    const auto delta = cost_probe::difference(before, after);
    Board::diag_printf("[XIP] %s access=%lu hit=%lu miss=%lu valid=%u saturated=%u\n", name,
                       (unsigned long)delta.access, (unsigned long)delta.hit,
                       (unsigned long)delta.miss, (unsigned)delta.valid,
                       (unsigned)delta.saturated);
    if constexpr (requires { Board::diag_drain(); }) Board::diag_drain();
  }
}
}
#endif
