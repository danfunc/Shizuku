#ifndef SHIZUKU_TESTS_ARM_INPLACE_CORE_NUM_TEST_ACCESS_HPP
#define SHIZUKU_TESTS_ARM_INPLACE_CORE_NUM_TEST_ACCESS_HPP

#include "shizuku/core_num.hpp"

namespace shizuku {

// basic_core_num の任意の Reader に対する部分特殊化
template <class Reader>
struct core_num_test_access<basic_core_num<Reader>> {
  static basic_core_num<Reader> make(uint32_t v) {
    return basic_core_num<Reader>(v, typename basic_core_num<Reader>::ctor_from_value_tag{});
  }
};

} // namespace shizuku

#if __has_include("arch/armv8m/armv8m.hpp") && !defined(SHIZUKU_TEST_ARMV6M)
#include "arch/armv8m/armv8m.hpp"

namespace shizuku {

// armv8m::core_num に対する明示特殊化
template <>
struct core_num_test_access<archs::armv8m<8, 4>::core_num> {
  static archs::armv8m<8, 4>::core_num make(uint32_t v) {
    using core_t = archs::armv8m<8, 4>::core_num;
    return core_t(v, typename core_t::ctor_from_value_tag{});
  }
};

} // namespace shizuku
#endif

#endif // SHIZUKU_TESTS_ARM_INPLACE_CORE_NUM_TEST_ACCESS_HPP
