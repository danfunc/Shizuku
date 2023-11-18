#ifndef SHIZUKU_OBJECT_HPP
#define SHIZUKU_OBJECT_HPP
#include "memory"
#include "shizuku/config.hpp"
#include "string"
#include <map>
#include <set>
namespace shizuku {
namespace types {

using thread = struct thread {
  shizuku::platform::std::shared_ptr<shizuku::context> context;
  int priority;
};

inline bool operator<(const thread &x, const thread &y) {
  return x.context < y.context;
}

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
  shizuku::platform::std::set<thread> thread_map;
  shizuku::platform::std::set<memory> memory_map;
  shizuku::platform::std::map<shizuku::platform::std::string,
                              int (*)(int, char *[])>
      functions;
  object() {
    thread_map = shizuku::platform::std::set<thread>();
    memory_map = shizuku::platform::std::set<memory>();
  }
};

static inline bool operator<(object x, object y) { return x.name < y.name; }

} // namespace types

} // namespace shizuku

#endif