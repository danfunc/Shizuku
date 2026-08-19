#ifndef SHIZUKU_TEMPLATE_OBJECT_HPP
#define SHIZUKU_TEMPLATE_OBJECT_HPP
#include "cstdint"
#include "result.hpp"
namespace shizuku {
namespace templates {

template <typename THREAD_T, typename THREAD_TABLE, typename METHOD,
          typename METHOD_TABLE, typename MEMORY_MAP, typename MEMORY_TABLE>
struct object {
  enum obj_type : uintptr_t {
    UNDEFINED_OBJECT = 0,
    KERNEL_OBJECT = 1,
    PRIVILEGED_LAND_OBJECT = 2,
    UNPRIVILEGED_LAND_OBJECT = 3,
  } type;
  using THREAD = THREAD_T;
  THREAD_TABLE thread_table;
  METHOD_TABLE method_table;
  MEMORY_TABLE memory_table;
  object(){};
  result<THREAD> create_thread(THREAD thread);
};

} // namespace templates

} // namespace shizuku

#endif // SHIZUKU_TEMPLATE_OBJECT_HPP