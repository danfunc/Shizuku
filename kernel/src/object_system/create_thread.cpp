#include <shizuku/object.hpp>
using namespace shizuku::types;

size_t object::create_thread(method entry, size_t caller_object_id,
                             size_t callee_object_id, size_t arg1,
                             size_t arg2) {
  this->total_thread_count++;
  this->thread_map[this->total_thread_count] = shizuku::thread_shared_ptr{
      new shizuku::types::thread{entry, callee_object_id, caller_object_id,
                                 arg1, arg2, this->total_thread_count}};
  return this->total_thread_count;
}
