#ifndef SHIZUKU_REGISTER_ISOLATION_CONTRACT_HPP
#define SHIZUKU_REGISTER_ISOLATION_CONTRACT_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace shizuku::abi {

// Common contract for register isolation on IPC exit/return:
// When an IPC call returns from callee to caller (via EXIT_METHOD / FORWARD_CHILD_EXIT),
// the kernel guarantees:
// 1. Error slot carries the return error code (0 on success).
// 2. Value slot carries the primary return value (e.g. 42).
// 3. All other caller-visible general purpose registers are scrubbed to 0,
//    preventing callee secrets or kernel internals from leaking to caller.

enum class reg_role : uint8_t {
  RETURN_ERROR,   // r0 / a0
  RETURN_VALUE,   // r1 / a1
  SCRUBBED_ZERO,  // Must be scrubbed to 0 on IPC return
  PASSTHROUGH,    // Scratch registers passed through by kernel (scrubbed by callee in return stub)
  CALLEE_SAVED,   // Preserved by callee across C calls; passed through by kernel on normal return
  PRESERVED_SP,   // Stack pointer (restored to caller's stack)
  PRESERVED_SYS,  // Architecture-reserved base pointers (gp, tp)
  RESTORED_PC,    // Program counter (caller resumption address)
  STATUS_FLAGS,   // PSR / mstatus
};

struct reg_contract {
  std::string_view name;
  reg_role role;
};

// ARMv8-M return register contract:
// Hardware exception return restores r0-r3, r12, lr, pc, xPSR from PSP.
// Kernel ensures r0 = error, r1 = value, and zeroes lr in the frame.
// On normal return, r2, r3, r12 and FP registers pass through (callee return stub zeroes
// scratch r2, r3, r12, s0-s15 and FPSCR flags before EXIT_METHOD, and caller restores FPSCR mode).
// Callee-saved r4-r11 pass through untouched (zeroed only on forced unwind like FORWARD_CHILD_EXIT).
inline constexpr reg_contract armv8m_return_contract[] = {
    {"r0",  reg_role::RETURN_ERROR},
    {"r1",  reg_role::RETURN_VALUE},
    {"r2",  reg_role::PASSTHROUGH},
    {"r3",  reg_role::PASSTHROUGH},
    {"r4",  reg_role::CALLEE_SAVED},
    {"r5",  reg_role::CALLEE_SAVED},
    {"r6",  reg_role::CALLEE_SAVED},
    {"r7",  reg_role::CALLEE_SAVED},
    {"r8",  reg_role::CALLEE_SAVED},
    {"r9",  reg_role::CALLEE_SAVED},
    {"r10", reg_role::CALLEE_SAVED},
    {"r11", reg_role::CALLEE_SAVED},
    {"r12", reg_role::PASSTHROUGH},
    {"sp",  reg_role::PRESERVED_SP},
    {"lr",  reg_role::SCRUBBED_ZERO},
    {"pc",  reg_role::RESTORED_PC},
};

// RISC-V (RV32) return register contract:
// Trap return restores sepc/mepc, sstatus/mstatus, sp.
// Kernel exit path sets a0 = error, a1 = value.
// Kernel scrubs ra, t0-t6, a2-a7, s0-s11 to zero.
// x0 is hardwired 0. gp/tp are preserved system pointers.
inline constexpr reg_contract rv32_return_contract[] = {
    {"zero", reg_role::SCRUBBED_ZERO},
    {"ra",   reg_role::SCRUBBED_ZERO},
    {"sp",   reg_role::PRESERVED_SP},
    {"gp",   reg_role::PRESERVED_SYS},
    {"tp",   reg_role::PRESERVED_SYS},
    {"t0",   reg_role::SCRUBBED_ZERO},
    {"t1",   reg_role::SCRUBBED_ZERO},
    {"t2",   reg_role::SCRUBBED_ZERO},
    {"s0",   reg_role::SCRUBBED_ZERO},
    {"s1",   reg_role::SCRUBBED_ZERO},
    {"a0",   reg_role::RETURN_ERROR},
    {"a1",   reg_role::RETURN_VALUE},
    {"a2",   reg_role::SCRUBBED_ZERO},
    {"a3",   reg_role::SCRUBBED_ZERO},
    {"a4",   reg_role::SCRUBBED_ZERO},
    {"a5",   reg_role::SCRUBBED_ZERO},
    {"a6",   reg_role::SCRUBBED_ZERO},
    {"a7",   reg_role::SCRUBBED_ZERO},
    {"s2",   reg_role::SCRUBBED_ZERO},
    {"s3",   reg_role::SCRUBBED_ZERO},
    {"s4",   reg_role::SCRUBBED_ZERO},
    {"s5",   reg_role::SCRUBBED_ZERO},
    {"s6",   reg_role::SCRUBBED_ZERO},
    {"s7",   reg_role::SCRUBBED_ZERO},
    {"s8",   reg_role::SCRUBBED_ZERO},
    {"s9",   reg_role::SCRUBBED_ZERO},
    {"s10",  reg_role::SCRUBBED_ZERO},
    {"s11",  reg_role::SCRUBBED_ZERO},
    {"t3",   reg_role::SCRUBBED_ZERO},
    {"t4",   reg_role::SCRUBBED_ZERO},
    {"t5",   reg_role::SCRUBBED_ZERO},
    {"t6",   reg_role::SCRUBBED_ZERO},
};

template <size_t N>
constexpr size_t count_scrubbed_zero(const reg_contract (&regs)[N]) {
  size_t count = 0;
  for (size_t i = 0; i < N; ++i) {
    if (regs[i].role == reg_role::SCRUBBED_ZERO) ++count;
  }
  return count;
}

template <size_t N>
constexpr size_t count_return_payload(const reg_contract (&regs)[N]) {
  size_t count = 0;
  for (size_t i = 0; i < N; ++i) {
    if (regs[i].role == reg_role::RETURN_ERROR || regs[i].role == reg_role::RETURN_VALUE)
      ++count;
  }
  return count;
}

template <size_t N>
constexpr size_t count_passthrough(const reg_contract (&regs)[N]) {
  size_t count = 0;
  for (size_t i = 0; i < N; ++i) {
    if (regs[i].role == reg_role::PASSTHROUGH) ++count;
  }
  return count;
}

template <size_t N>
constexpr size_t count_callee_saved(const reg_contract (&regs)[N]) {
  size_t count = 0;
  for (size_t i = 0; i < N; ++i) {
    if (regs[i].role == reg_role::CALLEE_SAVED) ++count;
  }
  return count;
}

static_assert(count_scrubbed_zero(armv8m_return_contract) == 1);
static_assert(count_return_payload(armv8m_return_contract) == 2);
static_assert(count_passthrough(armv8m_return_contract) == 3);
static_assert(count_callee_saved(armv8m_return_contract) == 8);
static_assert(count_scrubbed_zero(rv32_return_contract) == 27);
static_assert(count_return_payload(rv32_return_contract) == 2);

} // namespace shizuku::abi

#endif // SHIZUKU_REGISTER_ISOLATION_CONTRACT_HPP
