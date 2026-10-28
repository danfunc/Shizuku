// Register isolation through the actual ARMv8-M caller converter.
#include <cstddef>
#include <cstdint>
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/armv8m_abi.hpp"
#include "shizuku/method_entry.hpp"

alignas(32) uint8_t leak_caller_data[256];
alignas(32) uint8_t leak_callee_data[256];

extern "C" {
uintptr_t leak_caller_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
uintptr_t leak_callee_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
}

#if defined(__ARM_ARCH_8M_MAIN__)
[[gnu::always_inline]] static inline void leak_dirty_fp() {
  asm volatile(
      "movw r3, #0xbeef\n" "movt r3, #0xcafe\n"
      "vmov s0, r3\n" "vmov s1, r3\n" "vmov s2, r3\n" "vmov s3, r3\n"
      "vmov s4, r3\n" "vmov s5, r3\n" "vmov s6, r3\n" "vmov s7, r3\n"
      "vmov s8, r3\n" "vmov s9, r3\n" "vmov s10, r3\n" "vmov s11, r3\n"
      "vmov s12, r3\n" "vmov s13, r3\n" "vmov s14, r3\n" "vmov s15, r3\n"
      "mov.w r3, #0x400000\n" "vmsr fpscr, r3\n"
      : : : "r3", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
              "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15", "vfpcc", "memory");
}
static float leak_float_result(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  leak_dirty_fp();
  return 1.5f;
}
static double leak_double_result(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  leak_dirty_fp();
  return 1.5;
}
// The test-only veneer records registers before any compiler prologue. It
// changes no callee-saved registers and tail-branches to an ordinary function.
extern "C" __attribute__((noinline, used)) uintptr_t leak_callee_body(
    uintptr_t command, uintptr_t, uintptr_t, uintptr_t) {
  if (command == 77) {
    auto *data = reinterpret_cast<volatile uint32_t *>(leak_callee_data);
    const auto f = shizuku::armv8m_abi::syscall(
        3, 1, shizuku::method_entry<shizuku::KERNEL::ARCH, &leak_float_result>());
    const auto d = shizuku::armv8m_abi::syscall(
        3, 2, shizuku::method_entry<shizuku::KERNEL::ARCH, &leak_double_result>());
    data[12] = f.error;
    data[13] = d.error;
    return f.error | d.error;
  }
  asm volatile(
      "mrs r3, control\n"
      "tst r3, #4\n"
      "beq 1f\n"
      "movw r3, #0xbeef\n"
      "movt r3, #0xcafe\n"
      "vmov s0, r3\n" "vmov s1, r3\n" "vmov s2, r3\n" "vmov s3, r3\n"
      "vmov s4, r3\n" "vmov s5, r3\n" "vmov s6, r3\n" "vmov s7, r3\n"
      "vmov s8, r3\n" "vmov s9, r3\n" "vmov s10, r3\n" "vmov s11, r3\n"
      "vmov s12, r3\n" "vmov s13, r3\n" "vmov s14, r3\n" "vmov s15, r3\n"
      "mov.w r3, #0x400000\n"
      "vmsr fpscr, r3\n"
      "1:\n"
      "movw r1, #1\n" "movt r1, #0xcafe\n"
      "movw r2, #2\n" "movt r2, #0xcafe\n"
      "movw r3, #3\n" "movt r3, #0xcafe\n"
      "movw r12, #12\n" "movt r12, #0xcafe\n"
      : : : "r1", "r2", "r3", "r12", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
              "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15", "vfpcc", "memory");
  return 42;
}

__attribute__((naked)) uintptr_t leak_callee_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  asm volatile(
      "push {r0-r3}\n"
      "ldr r0, =leak_callee_data\n"
      "stmia r0, {r4-r11}\n"
      "movw r1, #0xbeef\n"
      "movt r1, #0xcafe\n"
      "str r1, [r0, #36]\n"
      "mrs r1, control\n"
      "str r1, [r0, #40]\n"
      "tst r1, #4\n"
      "beq 1f\n"
      "add r0, #64\n"
      "vstmia r0, {s0-s31}\n"
      "vmrs r1, fpscr\n"
      "str r1, [r0, #-20]\n"
      "1: pop {r0-r3}\n"
      "b leak_callee_body\n");
}

// Compiler owns saving live values across the converter's r4-r11 clobbers.
// Inspect final firmware disassembly as well as the selftest result.
__attribute__((noinline)) static uintptr_t leak_active_call(uintptr_t child) {
  auto *data = reinterpret_cast<volatile uint32_t *>(leak_caller_data);
  for (unsigned i = 0; i < 32; ++i) data[20 + i] = 0x3f800100u + i;
  const uint32_t *seed = reinterpret_cast<const uint32_t *>(leak_caller_data) + 20;
  uint32_t old_fpscr;
  asm volatile("vmrs %0, fpscr" : "=r"(old_fpscr));
  const uint32_t fp_canary = 0x00400000u;
  asm volatile("vmsr fpscr, %0" : : "r"(fp_canary) : "vfpcc");
  asm volatile("vldmia %0, {s0-s31}" : : "r"(seed) :
      "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
      "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
      "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
      "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31", "memory");
  uintptr_t v4 = 0xdead0004u, v5 = 0xdead0005u;
  uintptr_t v6 = 0xdead0006u, v7 = 0xdead0007u;
  uintptr_t v8 = 0xdead0008u, v9 = 0xdead0009u;
  uintptr_t v10 = 0xdead000au, v11 = 0xdead000bu;
  asm volatile("" : "+r"(v4), "+r"(v5), "+r"(v6), "+r"(v7),
                    "+r"(v8), "+r"(v9), "+r"(v10), "+r"(v11));
  const auto result = shizuku::armv8m_abi::syscall<false, true>(
      4, child, 0, 99, reinterpret_cast<uintptr_t>(leak_caller_data + 216));
  asm volatile("" : "+r"(v4), "+r"(v5), "+r"(v6), "+r"(v7),
                    "+r"(v8), "+r"(v9), "+r"(v10), "+r"(v11));
  uint32_t *out = reinterpret_cast<uint32_t *>(leak_caller_data) + 20;
  asm volatile("vstmia %0, {s0-s31}" : : "r"(out) : "memory");
  uint32_t result_fpscr;
  asm volatile("vmrs %0, fpscr" : "=r"(result_fpscr));
  asm volatile("vmsr fpscr, %0" : : "r"(old_fpscr) : "vfpcc");
  data[1] = v4; data[2] = v5; data[3] = v6; data[4] = v7;
  data[5] = v8; data[6] = v9; data[7] = v10; data[8] = v11;
  data[9] = result.error; data[10] = result.value;
  data[13] = result_fpscr;
  return result.value;
}

uintptr_t leak_caller_worker(uintptr_t cmd, uintptr_t, uintptr_t, uintptr_t) {
  auto *data = reinterpret_cast<volatile uint32_t *>(leak_caller_data);
  if (cmd == 0) {
    auto r = shizuku::armv8m_abi::syscall(1, data[0], data[16], 2);
    data[17] = r.error;
    return r.error;
  }
  if (cmd == 3) {
    const auto r = shizuku::armv8m_abi::syscall(4, data[0], 0, 77);
    data[9] = r.error;
    return r.value;
  }
  if (cmd == 4 || cmd == 5) {
    uint32_t control_before, control_after;
    asm volatile("mrs %0, control" : "=r"(control_before));
    data[11] = control_before;
    for (unsigned i = 20; i <= 36; ++i) data[i] = 0xffffffffu;
    const auto r = shizuku::armv8m_abi::syscall<false, false, true>(
        4, data[0], cmd - 3, 0, reinterpret_cast<uintptr_t>(leak_caller_data + 80));
    data[9] = r.error;
    data[10] = r.value;
    asm volatile("mrs %0, control" : "=r"(control_after));
    data[12] = control_after;
    return r.value;
  }
  uint32_t before, after;
  asm volatile("mrs %0, control" : "=r"(before));
  data[11] = before;
  if (cmd == 1) {
    const auto r = shizuku::armv8m_abi::syscall(4, data[0], 0, 99);
    data[9] = r.error; data[10] = r.value;
  } else {
    (void)leak_active_call(data[0]);
  }
  asm volatile("mrs %0, control" : "=r"(after));
  data[12] = after;
  return data[10];
}
#else
extern "C" {
uintptr_t leak_caller_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
uintptr_t leak_callee_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
}
#endif

namespace shizuku::selftest {
namespace {
using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;
void record_check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) { ++passed; BOARD::diag_printf("[SELFTEST] PASS %s\n", name); }
  else { ++failed; record_fail(name, got, want);
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got, want); }
}
}

void register_leak_test() {
#if defined(__ARM_ARCH_8M_MAIN__) || defined(SHIZUKU_RP2350)
  constexpr uintptr_t caller = object_id::leak_caller;
  constexpr uintptr_t callee = object_id::leak_callee;
  if constexpr (requires { ARCH::set_object_bounds(0, 0, 0, 0, 0); }) {
    ARCH::set_object_bounds(caller, reinterpret_cast<uintptr_t>(leak_caller_data),
                            reinterpret_cast<uintptr_t>(leak_caller_data) + sizeof(leak_caller_data), 0, 0);
    ARCH::set_object_bounds(callee, reinterpret_cast<uintptr_t>(leak_callee_data),
                            reinterpret_cast<uintptr_t>(leak_callee_data) + sizeof(leak_callee_data), 0, 0);
  }
  auto *c = reinterpret_cast<volatile uint32_t *>(leak_caller_data);
  auto *d = reinterpret_cast<volatile uint32_t *>(leak_callee_data);
  for (unsigned i = 0; i < 64; ++i) { c[i] = 0; d[i] = 0; }
  c[0] = callee;
  c[16] = reinterpret_cast<uintptr_t>(&leak_callee_worker);
  auto created = ARCH::syscall(static_cast<uintptr_t>(object_api::CREATE_OBJECT), caller,
                               reinterpret_cast<uintptr_t>(&leak_caller_worker), OBJECT_UNPRIVILEGED);
  if (created.error) BOARD::panic("register_leak_test: failed to create caller");
  auto setup = ARCH::syscall(static_cast<uintptr_t>(object_api::CALL_METHOD), caller, 0, 0);
  record_check("leak setup", setup.error == 0 && c[17] == 0, setup.error | c[17], 0);

  // Earlier selftests activate FP in the boot thread. A newly spawned context
  // starts with a basic EXC_RETURN and provides a real FPCA=0 precondition.
  const auto run_fresh = [=](uintptr_t command) -> ARCH::syscall_result {
    const auto spawned = ARCH::syscall(static_cast<uintptr_t>(object_api::SPAWN),
                                       caller, 0, command);
    if (spawned.error != 0 || spawned.value == 0)
      return {spawned.error != 0 ? spawned.error : UINTPTR_MAX, 0};
    return ARCH::syscall(static_cast<uintptr_t>(object_api::RUN_FOR), spawned.value,
                         5000u * BOARD::cycles_per_us());
  };
  auto inactive = run_fresh(1);
  record_check("leak inactive FPCA preserved",
               inactive.error == 0 && inactive.value == (uintptr_t)grant_end::YIELDED &&
               c[9] == 0 && c[10] == 42 &&
               d[9] == 0xcafebeefu && (c[11] & 4u) == 0 && (c[12] & 4u) == 0 && (d[10] & 4u) == 0,
               (c[11] | c[12] | d[10]) & 4u, 0);

  const auto exported = ARCH::syscall(static_cast<uintptr_t>(object_api::CALL_METHOD), caller, 0, 3);
  record_check("FP return methods exported", exported.error == 0 && exported.value == 0 &&
               c[9] == 0 && d[12] == 0 && d[13] == 0, exported.error, 0);
  const auto float_call = run_fresh(4);
  bool float_clean = c[20] == 0x3fc00000u && c[36] == 0;
  for (unsigned i = 1; i < 16; ++i) float_clean &= c[20 + i] == 0;
  record_check("FP float return", float_call.error == 0 && c[9] == 0 && c[10] == 0 &&
               (c[11] & 4u) == 0 && (c[12] & 4u) != 0 && float_clean, c[20], 0x3fc00000u);
  const auto double_call = ARCH::syscall(static_cast<uintptr_t>(object_api::CALL_METHOD), caller, 0, 5);
  bool double_clean = c[20] == 0 && c[21] == 0x3ff80000u && c[36] == 0;
  for (unsigned i = 2; i < 16; ++i) double_clean &= c[20 + i] == 0;
  record_check("FP double return", double_call.error == 0 && c[9] == 0 && c[10] == 0 &&
               double_clean, c[21], 0x3ff80000u);

  for (unsigned i = 0; i < 64; ++i) d[i] = 0xffffffffu;
  auto active = ARCH::syscall(static_cast<uintptr_t>(object_api::CALL_METHOD), caller, 0, 2);
  bool gp_zero = true, gp_restored = true, fp_entry_zero = true;
  bool fp_lower_zero = true, fp_upper_restored = true;
  for (unsigned i = 0; i < 8; ++i) {
    gp_zero &= d[i] == 0;
    gp_restored &= c[i + 1] == 0xdead0004u + i;
  }
  for (unsigned i = 0; i < 32; ++i) fp_entry_zero &= d[16 + i] == 0;
  for (unsigned i = 0; i < 16; ++i) {
    fp_lower_zero &= c[20 + i] == 0;
    fp_upper_restored &= c[36 + i] == 0x3f800110u + i;
  }
  record_check("leak A callee entry", active.error == 0 && active.value == 42 &&
               c[9] == 0 && c[10] == 42 && d[9] == 0xcafebeefu &&
               (d[10] & 4u) && gp_zero && fp_entry_zero && d[11] == 0, d[9], 0xcafebeefu);
  record_check("leak B caller registers restored", gp_restored && fp_upper_restored,
               gp_restored | (fp_upper_restored << 1), 3);
  record_check("leak B return scratch zero", c[54] == 0 && c[55] == 0 &&
               c[56] == 0 && c[57] == 0, c[54] | c[55] | c[56] | c[57], 0);
  record_check("leak B return lower FP zero", fp_lower_zero && c[13] == 0,
               c[13], 0);
#endif
}
} // namespace shizuku::selftest
