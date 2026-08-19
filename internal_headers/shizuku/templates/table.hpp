#ifndef SHIZUKU_TEMPLATE_TABLE_HPP
#define SHIZUKU_TEMPLATE_TABLE_HPP
#include "concepts"
#include "cstdint"
#include "result.hpp"
namespace shizuku {
namespace templates {
template <typename ELEMENT_TYPE, uintptr_t SIZE> class static_table {
  ELEMENT_TYPE instance[SIZE];

public:
  result<ELEMENT_TYPE &> operator[](uintptr_t position) {
    if (position < SIZE) {
      return {instance[position]};
    } else {
      return {range_overflow, "invalid access to static table. range over"};
    }
  };
  using element_type = ELEMENT_TYPE;
};
} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATE_TABLE_HPP