
#ifndef SHIZUKU_OBJECT_HPP
#define SHIZUKU_OBJECT_HPP
#include "shizuku/config.hpp"

namespace shizuku {
namespace types {
using thread = struct thread {
  shizuku::context context;
  size_t remain_time;
};

using object = struct object {};
} // namespace types

} // namespace shizuku

#endif