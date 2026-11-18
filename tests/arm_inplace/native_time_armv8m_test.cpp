#include <cstdio>
#include "shizuku/native_time.hpp"
#include "arch/armv8m/armv8m.hpp"

namespace {
struct Clock {
  static inline bool available = true;
  static inline unsigned reads = 0;
  static bool try_time_us(uint64_t &out) {
    ++reads;
    out = 0x12345678ABCDEF01ull;
    return available;
  }
};
}

int native_time_armv8m_checks() {
  using A = shizuku::archs::armv8m<8, 4>;
  A::exception_frame_t f{};
  f.r3 = 0x55;
  int errors = 0;
  auto check = [&](bool ok, const char *what) {
    if (!ok) { std::printf("FAIL ARMv8-M: %s\n", what); ++errors; }
  };
  Clock::available = true; Clock::reads = 0;
  shizuku::native::get_time<Clock, A>(f);
  check(f.r0 == 0 && f.r1 == 0xABCDEF01 && f.r2 == 0x12345678, "64-bit success reply");
  check(f.r3 == 0x55, "unrelated input word preserved");
  Clock::available = false;
  shizuku::native::get_time<Clock, A>(f);
  check(f.r0 == 24 && f.r1 == 0 && f.r2 == 0, "unavailable reply clears both value words");
  check(Clock::reads == 2, "one bounded board call per request");
  return errors;
}
