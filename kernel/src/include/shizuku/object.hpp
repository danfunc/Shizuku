#ifndef SHIZUKU_OBJECT_HPP
#define SHIZUKU_OBJECT_HPP
#include "cstdint"
#include "shizuku/config.hpp"
#include "shizuku/method.hpp"
#include "shizuku/thread.hpp"
namespace shizuku {
namespace types {

class kernel;
}
extern types::kernel kernel;
namespace types {
enum object_attribute { super_object, normal_object };
struct permission {
  bool writable;
  bool readable;
  bool executable;
  bool kernel_only;
};

struct memory {
  void *real_start;
  void *virtual_start;
  size_t size;
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
  shizuku::map<shizuku::string, method> method_map;
  size_t total_thread_count;
  shizuku::map<size_t,
               shizuku::platform::std::shared_ptr<shizuku::types::thread>>
      thread_map;
  shizuku::map<size_t, size_t> under_object_map;

public:
  const size_t object_id;
  const shizuku::types::object_attribute attribute;
  object(size_t this_object_id, size_t creator_object_id, method entry,
         uint32_t arg1, uint32_t arg2,
         shizuku::types::object_attribute attribute = normal_object)
      : object_id{this_object_id}, attribute{attribute} {
    total_thread_count = 1;
    thread_map[total_thread_count] = shizuku::thread_shared_ptr(
        new shizuku::types::thread(entry, this_object_id, creator_object_id,
                                   arg1, arg2, total_thread_count));
  }
  void export_method(method entry, shizuku::string const &name);
  shizuku::thread_shared_ptr create_thread(method entry,
                                           size_t caller_object_id,
                                           size_t callee_object_id, size_t arg1,
                                           size_t arg2);
  void remove_thread(size_t thread_id);
};

} // namespace types

} // namespace shizuku

#endif