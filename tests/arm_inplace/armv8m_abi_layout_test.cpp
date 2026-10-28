#include <cassert>
#include <cstdint>
#include <initializer_list>
#include "shizuku/armv8m_abi.hpp"

uintptr_t word_entry(uintptr_t a, uintptr_t, uintptr_t, uintptr_t) { return a; }
uintptr_t zero_entry() { return 17; }
void void_entry(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {}
uintptr_t fifth_entry(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t fifth) { return fifth; }
float float_result(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
double double_result(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t a) { return a; }
uintptr_t float_arg(float, uintptr_t, uintptr_t, uintptr_t) { return 0; }
uintptr_t six_args(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
static_assert(shizuku::armv8m_abi::supported_entry<decltype(&word_entry)>::value);
static_assert(shizuku::armv8m_abi::supported_entry<decltype(&zero_entry)>::value);
static_assert(shizuku::armv8m_abi::supported_entry<decltype(&void_entry)>::value);
static_assert(!shizuku::armv8m_abi::supported_entry<decltype(&float_result)>::value);
static_assert(!shizuku::armv8m_abi::supported_entry<decltype(&float_arg)>::value);
static_assert(shizuku::armv8m_abi::fp_entry_traits<decltype(&float_result)>::supported);
static_assert(shizuku::armv8m_abi::fp_entry_traits<decltype(&double_result)>::supported);
static_assert(!shizuku::armv8m_abi::fp_entry_traits<decltype(&float_arg)>::supported);
static_assert(!shizuku::armv8m_abi::supported_entry<decltype(&six_args)>::value);

int main() {
  namespace abi = shizuku::armv8m_abi;
  assert(abi::wrapper<&word_entry>(42, 0, 0, 0, 0) == 42);
  assert(abi::wrapper<&zero_entry>(42, 0, 0, 0, 0) == 17);
  assert(abi::wrapper<&void_entry>(0, 0, 0, 0, 0) == 0);
  assert(abi::wrapper<&fifth_entry>(1, 2, 3, 4, 0x55) == 0x55);
  assert(abi::fp_body<&float_result>(0, 0, 0, 0, 0) == 0.0f);
  assert(abi::fp_body<&double_result>(0, 0, 0, 0, 7) == 7.0);
  constexpr uintptr_t base = 0x10000;
  for (uintptr_t offset = 0; offset < abi::stack_alignment; offset += 8) {
    const uintptr_t original = base + offset;
    for (bool active : {false, true}) {
      const uintptr_t ret = abi::trap_sp(original, active);
      assert(ret == (active ? base - 96 : base - 32));
      assert(ret % 32 == 0);
      assert(ret + abi::scratch_bytes <= original);
      if (active) {
        const uintptr_t fp = abi::fp_saved_begin(original);
        assert(fp + abi::fp_saved_bytes == original);
        assert(ret + abi::scratch_bytes <= fp);
        assert(ret + 8 <= fp);
      }
    }
  }
}
