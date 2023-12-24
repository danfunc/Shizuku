#ifndef SHIZUKU_OBJECT_HPP
#define SHIZUKU_OBJECT_HPP
#include "memory"
#include "queue"
#include "shizuku/config.hpp"
#include "shizuku/function.hpp"
#include "shizuku/thread.hpp"
#include "string"
#include <map>
#include <set>
namespace shizuku {
namespace types {
class kernel;

struct permission {
  bool writable;
  bool readable;
  bool executable;
  bool kernel_only;
};

struct memory {
  void *real_start;
  void *virtual_start;
  shizuku::platform::std::size_t size;
  shizuku::types::permission permission;
  int operator<=>(const memory &right) const {
    return (int)this->virtual_start - (int)right.virtual_start;
  }
};

inline bool operator<(memory x, memory y) {
  return x.virtual_start < y.virtual_start;
}
class object {
  friend shizuku::types::kernel;
  shizuku::platform::std::string name;
  shizuku::platform::std::set<memory> memory_map;
  shizuku::types::functions functions;
  shizuku::platform::std::set<
      shizuku::platform::std::shared_ptr<shizuku::types::thread>>
      threads;
  friend shizuku::types::kernel;

public:
  object(shizuku::platform::std::string const &name) { this->name = name; };
  auto operator<=>(object const &right) const {
    return this->name <=> right.name;
  };
  void add_thread(
      shizuku::platform::std::shared_ptr<shizuku::types::thread> &thread_ptr) {
    this->threads.insert(thread_ptr);
  }
  void add_func(
      shizuku::platform::std::string const &name,
      int (*entry)(void *user_arg1, void *user_arg2,
                   const shizuku::platform::std::string &parent_object_name,
                   size_t thread_id));
  void exit(shizuku::platform::std::shared_ptr<shizuku::types::thread> const
                &thread) {
    threads.erase(thread);
  }
  void call_func(shizuku::platform::std::string const &name, void *user_arg1,
                 void *user_arg2);
};

class object_tree : shizuku::platform::std::set<shizuku::types::object> {
  friend shizuku::types::kernel;
  using shizuku::platform::std::set<shizuku::types::object>::find;

public:
  shizuku::types::object *const
  operator[](shizuku::platform::std::string const &name) const {
    return (shizuku::types::object *)&(*this->find(name));
  }
};

} // namespace types

} // namespace shizuku

#endif