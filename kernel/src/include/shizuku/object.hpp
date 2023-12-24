#ifndef SHIZUKU_OBJECT_HPP
#define SHIZUKU_OBJECT_HPP
#include "function.hpp"
#include "memory"
#include "queue"
#include "shizuku/config.hpp"
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
  friend shizuku::types::kernel;
};

class object_shared_ptr
    : shizuku::platform::std::shared_ptr<shizuku::types::object> {
  using shizuku::platform::std::shared_ptr<shizuku::types::object>::operator*;
  using shizuku::platform::std::shared_ptr<shizuku::types::object>::operator->;
  auto operator<=>(shizuku::platform::std::string const &right) const {
    return this->operator->()->operator<=>(right);
  }
};
class object_weak_ptr
    : shizuku::platform::std::weak_ptr<shizuku::types::object> {
  using shizuku::platform::std::weak_ptr<shizuku::types::object>::lock;
  auto operator<=>(shizuku::platform::std::string const &right) const {};
};

class object_tree
    : shizuku::platform::std::set<
          shizuku::platform::std::shared_ptr<shizuku::types::object>> {
  friend shizuku::types::kernel;
  using shizuku::platform::std::set<
      shizuku::platform::std::shared_ptr<shizuku::types::object>>::find;

public:
  const shizuku::platform::std::shared_ptr<shizuku::types::object>
  operator[](shizuku::platform::std::string const &name) const {
    return *this->find(
        shizuku::platform::std::make_shared<shizuku::types::object>(name));
  }
};

} // namespace types

} // namespace shizuku

#endif