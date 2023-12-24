#include "shizuku/object.hpp"

void shizuku::types::object::add_func(
    shizuku::platform::std::string const &name,
    int (*entry)(void *user_arg1, void *user_arg2,
                 const shizuku::platform::std::string &parent_object_name,
                 size_t thread_id)) {
  this->functions.emplace(entry, name);
}