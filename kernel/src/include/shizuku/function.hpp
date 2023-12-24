
#ifndef SHIZUKU_FUNCTIONS_HPP
#define SHIZUKU_FUNCTIONS_HPP
#include "set"
#include "shizuku/platform.hpp"
namespace shizuku {
namespace types {
struct function {
  int (*entry_point)(void *user_arg1, void *user_arg2,
                     const shizuku::platform::std::string &parent_object_name,
                     size_t thread_id);
  shizuku::platform::std::string name;
  const auto operator<=>(function const &right) const {
    return this->name <=> right.name;
  };
};

class functions : public shizuku::platform::std::set<function> {
public:
  const function &operator[](shizuku::platform::std::string const &name) {
    function finder{.name = name};
    return *this->find(finder);
  }
};
} // namespace types
} // namespace shizuku
#endif
