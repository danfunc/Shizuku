#ifndef SHIZUKU_ARMV8M_ABI_HPP
#define SHIZUKU_ARMV8M_ABI_HPP

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace shizuku::armv8m_abi {

// The converter owns this area; the kernel must never use it as return authority.
// ret_sp is the SP at SVC. The original SP and FP saves are at or above ret_sp,
// outside a callee's exclusive REGION_STACK upper bound.
inline constexpr uintptr_t stack_alignment = 32;
inline constexpr uintptr_t scratch_bytes = 32;
inline constexpr uintptr_t fp_saved_bytes = 16 * sizeof(uint32_t);
inline constexpr uintptr_t max_reservation_bytes =
    (fp_saved_bytes + stack_alignment - 1 + scratch_bytes + 7) & ~uintptr_t(7);

constexpr uintptr_t trap_sp(uintptr_t original_sp, bool fp_active) {
  return ((original_sp - (fp_active ? fp_saved_bytes : 0)) &
          ~(stack_alignment - 1)) - scratch_bytes;
}
constexpr uintptr_t fp_saved_begin(uintptr_t original_sp) {
  return original_sp - fp_saved_bytes;
}

struct result {
  uintptr_t error;
  uintptr_t value;
};

// Entries use the one-word soft-float ABI. A normal C++ return ensures that
// its AAPCS epilogue finishes before the common assembly return stub runs.
template <typename T> constexpr bool word_result() {
  if constexpr (std::is_void_v<T>) return true;
  else return std::is_integral_v<T> && sizeof(T) <= sizeof(uintptr_t);
}
template <typename> struct supported_entry : std::false_type {};
template <typename R, typename... A> struct supported_entry<R (*)(A...)>
    : std::bool_constant<(sizeof...(A) <= 5) &&
                         ((std::is_integral_v<A> && sizeof(A) <= sizeof(uintptr_t)) && ...) &&
                         word_result<R>()> {};
template <typename> struct entry_arity;
template <typename R, typename... A> struct entry_arity<R (*)(A...)>
    : std::integral_constant<unsigned, sizeof...(A)> {};
template <typename> struct fp_entry_traits {
  static constexpr bool supported = false;
};
template <typename R, typename... A> struct fp_entry_traits<R (*)(A...)> {
  using result_type = R;
  static constexpr bool supported =
      (std::is_same_v<R, float> || std::is_same_v<R, double>) &&
      sizeof...(A) <= 5 &&
      ((std::is_integral_v<A> && sizeof(A) <= sizeof(uintptr_t)) && ...);
};
template <auto F, std::size_t... I>
[[gnu::always_inline]] inline auto invoke_entry(const uintptr_t (&args)[5],
                                                std::index_sequence<I...>) {
  return F(args[I]...);
}
template <auto F>
uintptr_t wrapper(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                  uintptr_t a4) {
  static_assert(supported_entry<decltype(F)>::value,
                "ARM method entries require at most five integer words and a word or void result");
  const uintptr_t args[] = {a0, a1, a2, a3, a4};
  using indices = std::make_index_sequence<entry_arity<decltype(F)>::value>;
  using R = decltype(invoke_entry<F>(args, indices{}));
  if constexpr (std::is_void_v<R>) { invoke_entry<F>(args, indices{}); return 0; }
  else return invoke_entry<F>(args, indices{});
}

extern "C" void shizuku_armv8m_return_float_stub();
extern "C" void shizuku_armv8m_return_double_stub();

// The target uses softfp; this ordinary body converts its typed FP return to
// the VFP return convention before its AAPCS epilogue returns to the stub.
template <auto F>
__attribute__((pcs("aapcs-vfp"), used))
typename fp_entry_traits<decltype(F)>::result_type
fp_body(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
  static_assert(fp_entry_traits<decltype(F)>::supported);
  const uintptr_t args[] = {a0, a1, a2, a3, a4};
  return invoke_entry<F>(args, std::make_index_sequence<entry_arity<decltype(F)>::value>{});
}

// call_entry has already placed argument five at [sp]. This leaf changes no
// arguments or callee-saved registers and passes control to a normal body.
template <auto F>
__attribute__((naked, used)) void fp_entry() {
  using R = typename fp_entry_traits<decltype(F)>::result_type;
  static_assert(fp_entry_traits<decltype(F)>::supported);
  if constexpr (std::is_same_v<R, float>) {
    asm volatile("ldr lr, =shizuku_armv8m_return_float_stub\n"
                 "ldr r12, =%c0\n"
                 "bx r12\n" : : "i" (&fp_body<F>));
  } else {
    asm volatile("ldr lr, =shizuku_armv8m_return_double_stub\n"
                 "ldr r12, =%c0\n"
                 "bx r12\n" : : "i" (&fp_body<F>));
  }
}

template <auto F> inline uintptr_t entry_address() {
  if constexpr (fp_entry_traits<decltype(F)>::supported)
    return reinterpret_cast<uintptr_t>(&fp_entry<F>);
  else {
    static_assert(supported_entry<decltype(F)>::value);
    return reinterpret_cast<uintptr_t>(&wrapper<F>);
  }
}

// r7 is preserved explicitly in the protected scratch area, so callers using
// Thumb frame pointers (including alloca/VLA) can inline the converter too.
// No high FP register is in the clobber list: compiler-generated VFP saves can
// otherwise execute before the FPCA test. Every FP instruction, including the
// saves, is in this single asm block after the runtime test.
// ObserveScratch is reserved for the selftest: a4 supplies its output buffer.
// GAS removes these instructions entirely from every normal instantiation.
template <bool Service = false, bool ObserveScratch = false, bool ObserveFP = false>
[[gnu::always_inline]] inline result syscall(uintptr_t number, uintptr_t a1 = 0,
                                             uintptr_t a2 = 0, uintptr_t a3 = 0,
                                             uintptr_t a4 = 0) {
  static_assert(!(ObserveScratch && ObserveFP));
  register uintptr_t r0 asm("r0") = number;
  register uintptr_t r1 asm("r1") = a1;
  register uintptr_t r2 asm("r2") = a2;
  register uintptr_t r3 asm("r3") = a3;
  register uintptr_t r12 asm("r12") = a4;
  asm volatile(
      "mov r4, sp\n"
      "mov r6, r7\n"
      "mrs r5, control\n"
      "tst r5, #4\n"
      "beq .Lshizuku_no_fp_save_%=\n"
      "vpush {s16-s31}\n"
      ".Lshizuku_no_fp_save_%=:\n"
      "mov lr, sp\n"
      "bic lr, lr, #31\n"
      "sub lr, lr, #32\n"
      "mov sp, lr\n"
      "str r4, [sp, #0]\n"
      "str r5, [sp, #4]\n"
      "str r6, [sp, #12]\n"
      ".if %c[observe] + %c[observe_fp]\n"
      "str r12, [sp, #8]\n"
      ".endif\n"
      "tst r5, #4\n"
      "beq .Lshizuku_no_fp_clear_%=\n"
      "movs r4, #0\n"
      "vmov s0, r4\n"  "vmov s1, r4\n"
      "vmov s2, r4\n"  "vmov s3, r4\n"
      "vmov s4, r4\n"  "vmov s5, r4\n"
      "vmov s6, r4\n"  "vmov s7, r4\n"
      "vmov s8, r4\n"  "vmov s9, r4\n"
      "vmov s10, r4\n" "vmov s11, r4\n"
      "vmov s12, r4\n" "vmov s13, r4\n"
      "vmov s14, r4\n" "vmov s15, r4\n"
      "vmov s16, r4\n" "vmov s17, r4\n"
      "vmov s18, r4\n" "vmov s19, r4\n"
      "vmov s20, r4\n" "vmov s21, r4\n"
      "vmov s22, r4\n" "vmov s23, r4\n"
      "vmov s24, r4\n" "vmov s25, r4\n"
      "vmov s26, r4\n" "vmov s27, r4\n"
      "vmov s28, r4\n" "vmov s29, r4\n"
      "vmov s30, r4\n" "vmov s31, r4\n"
      "vmsr fpscr, r4\n"
      ".Lshizuku_no_fp_clear_%=:\n"
      "movs r4, #0\n"
      "movs r5, #0\n"
      "movs r6, #0\n"
      "movs r7, #0\n"
      "mov r8, r4\n"
      "mov r9, r4\n"
      "mov r10, r4\n"
      "mov r11, r4\n"
      "mov lr, r4\n"
      "svc 0\n"
      ".if %c[service]\n"
      ".global shizuku_v2_service_resume\n"
      "shizuku_v2_service_resume:\n"
      ".endif\n"
      ".if %c[observe]\n"
      "ldr r4, [sp, #8]\n"
      "str r2, [r4, #0]\n"
      "str r3, [r4, #4]\n"
      "str r12, [r4, #8]\n"
      "str lr, [r4, #12]\n"
      ".endif\n"
      ".if %c[observe_fp]\n"
      "mrs r5, control\n"
      "tst r5, #4\n"
      "beq .Lshizuku_observe_fp_inactive_%=\n"
      "ldr r4, [sp, #8]\n"
      "vstmia r4, {s0-s15}\n"
      "vmrs r5, fpscr\n"
      "str r5, [r4, #64]\n"
      ".Lshizuku_observe_fp_inactive_%=:\n"
      ".endif\n"
      "ldr r2, [sp, #0]\n"
      "ldr r3, [sp, #4]\n"
      "ldr r7, [sp, #12]\n"
      "tst r3, #4\n"
      "beq .Lshizuku_no_fp_restore_%=\n"
      "sub r12, r2, #64\n"
      "mov sp, r12\n"
      "vpop {s16-s31}\n"
      "b .Lshizuku_converter_done_%=\n"
      ".Lshizuku_no_fp_restore_%=:\n"
      "mov sp, r2\n"
      ".Lshizuku_converter_done_%=:\n"
      : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r12)
      : [service] "i" (Service), [observe] "i" (ObserveScratch), [observe_fp] "i" (ObserveFP)
      : "r4", "r5", "r6", "r8", "r9", "r10", "r11", "lr",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
        "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
        "vfpcc", "cc", "memory");
  return {r0, r1};
}

} // namespace shizuku::armv8m_abi

#endif
