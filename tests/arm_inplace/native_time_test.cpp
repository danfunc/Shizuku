#include <cstdio>
#include <initializer_list>
#include "shizuku/native_time.hpp"
#include "shizuku/svc_direct.hpp"
#include "arch/riscv/rv32.hpp"

// Each ARM ISA's C-linkage trap declarations stay in its own translation unit.
int native_time_armv8m_checks();
int native_time_armv6m_checks();

struct Clock {
  static inline bool available = true;
  static inline unsigned reads = 0;
  static bool try_time_us(uint64_t &out) {
    ++reads;
    out = 0x12345678ABCDEF01ull; // even a failed board may have touched out
    return available;
  }
};
struct Mode {
  static constexpr uint32_t CALL_HEADROOM = 0;
  static constexpr bool HAS_CYCLE_COUNTER = false;
  static constexpr uint32_t TIMER_MAX_CYCLES = UINT32_MAX;
  static constexpr uint32_t TIMER_MIN_CYCLES = 1;
};
using RV = shizuku::archs::rv32<16, 4, Mode>;
static int errors;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++errors; } } while (0)
int main() {
  errors += native_time_armv8m_checks();
  errors += native_time_armv6m_checks();
  for (bool user : {false, true}) {
    RV::exception_frame_t f{};
    f.mstatus = user ? 0 : RV32_MSTATUS_MPP;
    Clock::available = true;
    shizuku::native::get_time<Clock, RV>(f);
    CHECK(f.a0 == 0 && f.a1 == 0xABCDEF01 && f.a2 == 0x12345678);
    CHECK(f.mcause == (user ? RV32_TAG_U_TIME : RV32_TAG_TIME));
    RV::context_t context{}; context.sp = &f;
    RV::scrub_on_switch(context);
    CHECK(f.mcause == (user ? RV32_TAG_U_TIME : RV32_TAG_TIME));
    Clock::available = false;
    shizuku::native::get_time<Clock, RV>(f);
    CHECK(f.a0 == 24 && f.a1 == 0 && f.a2 == 0);
    RV::set_time_result(f, shizuku::object_error::OK, 0x12345678ABCDEF01ull);
    RV::set_result(f, 7, 9);
    CHECK(f.a0 == 7 && f.a1 == 9);
    CHECK(f.mcause == (user ? RV32_TAG_U_SCRUB : RV32_TAG_ECALL_SCRUB));
    RV::set_time_result(f, shizuku::object_error::OK, 0x12345678ABCDEF01ull);
    RV::set_unprivileged_resume_metadata(f, 0x20001000, 0x40801000, false);
    CHECK(f.mcause == RV32_TAG_U_ECALL); // normal method return never retains TIME
  }
  CHECK(Clock::reads == 4);
  static_assert(sizeof(RV::syscall_result) == 2 * sizeof(uintptr_t));
  static_assert(sizeof(shizuku::rv32_abi::call_result) == 2 * sizeof(uintptr_t));
  struct Result { uintptr_t value, value_high; } r{0xABCDEF01, 0x12345678};
  CHECK(shizuku::native::time_value(r) == 0x12345678ABCDEF01ull);
  const auto *a = shizuku::svc_attribute_for(34);
  CHECK(a && a->mode == shizuku::svc_mode::handler && !a->from_isr);
  CHECK(shizuku::svc_permission_allows(34, false));
  CHECK(shizuku::svc_direct_of(34) == shizuku::svc_direct_kind::GET_TIME);
  return errors ? 1 : 0;
}
