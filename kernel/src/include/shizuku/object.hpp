#ifndef SHIZUKU_OBJECT_HPP
#define SHIZUKU_OBJECT_HPP
#include "memory"
#include "queue"
#include "shizuku/config.hpp"
#include "shizuku/thread.hpp"
#include "string"
#include <map>
#include <set>
namespace shizuku {
namespace types {

using permission = struct permission {
  bool writable;
  bool readable;
  bool executable;
  bool kernel_only;
};

using memory = struct memory {
  shizuku::types::permission permission;
  void *real_start;
  void *virtual_start;
  shizuku::platform::std::size_t size;
};

inline bool operator<(memory x, memory y) {
  return x.virtual_start < y.virtual_start;
}

using function = struct function {
  int (*entry)(int argc, char *argv[]);
  shizuku::platform::std::string name;
};

using object = struct object {
  shizuku::platform::std::string name;
  shizuku::platform::std::set<memory> memory_map;
  shizuku::platform::std::map<shizuku::platform::std::string,
                              int (*)(int, char *[])>
      functions;
  shizuku::platform::std::set<shizuku::types::thread> threads;
  size_t thread_count;
  auto operator<=>(object const &right) const {
    return this->name <=> right.name;
  }
};

static inline bool operator<(object x, object y) { return x.name < y.name; }

} // namespace types

} // namespace shizuku

#endif