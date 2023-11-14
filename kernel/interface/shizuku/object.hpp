
#ifndef SHIZUKU_OBJECT_HPP
#define SHIZUKU_OBJECT_HPP
#include "shizuku/config.hpp"

namespace shizuku {
namespace types {
/*
using thread = struct thread {
shizuku::context &context;
size_t remain_time;
};
using permission = struct permission {
bool writable;
bool readable;
bool executable;
bool kernel_only;
};
using memory_map = struct memory_map {
shizuku::types::permission permission;
void *physical_address_start;
void *virtual_address_start;
shizuku::platform::std::size_t size;
};

using object = struct object {};*/

template <typename CONTEXT> struct object;
template <typename CONTEXT, typename PARENT> struct thread {
  CONTEXT context;
  PARENT parent;
};

} // namespace types

} // namespace shizuku

#endif