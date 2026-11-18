#ifndef SHIZUKU_NATIVE_TIME_HPP
#define SHIZUKU_NATIVE_TIME_HPP
#include <cstdint>
#include "shizuku/object_api.hpp"
namespace shizuku::native {
// No memory supplied by the caller is dereferenced. Board owns the bounded
// hardware read; ARCH owns reply placement and the return-to-user policy.
template <class Board, class Arch, class Frame>
[[gnu::always_inline]] inline void get_time(Frame &frame) {
  uint64_t value = 0;
  const bool ok = Board::try_time_us(value);
  Arch::set_time_result(frame, ok ? object_error::OK : object_error::TIME_UNAVAILABLE,
                        ok ? value : uint64_t{0});
}
template <class Result> constexpr uint64_t time_value(const Result &r) {
  return (uint64_t(uint32_t(r.value_high)) << 32) | uint32_t(r.value);
}
}
#endif
