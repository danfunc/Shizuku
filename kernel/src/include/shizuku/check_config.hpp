#ifndef SHIZUKU_CHECK_CONFIG_HPP
#define SHIZUKU_CHECK_CONFIG_HPP

#include "shizuku/config.hpp"
#include "shizuku/same_type.hpp" //include original same_as but it is replacable to std::same_as
#include <concepts>

namespace shizuku {
namespace types {
struct task;
struct thread;
struct object;
} // namespace types
namespace concepts {
template <typename T, typename U>
concept shared_ptr_requires = requires(T pointer) {
  { pointer.get() } -> ::shizuku::same_as<U *>;
  { pointer.operator*() } -> ::shizuku::same_as<U &>;
  { pointer.operator->() } -> ::shizuku::same_as<U *>;
};
template <typename T, typename U>
concept weak_ptr_requires = requires(T pointer) {
  { pointer.lock() } -> ::shizuku::concepts::shared_ptr_requires<U>;
};
template <typename T>
concept object_shared_ptr_requires =
    shared_ptr_requires<T, shizuku::types::object>;
template <typename T>
concept object_weak_ptr_requires = weak_ptr_requires<T, shizuku::types::object>;
template <typename T>
concept thread_weak_ptr_requires = weak_ptr_requires<T, shizuku::types::thread>;
template <typename T>
concept thread_shared_ptr_requires =
    shared_ptr_requires<T, shizuku::types::thread>;
template <typename T>
concept context_shared_ptr = shared_ptr_requires<T, shizuku::context>;
template <typename T>
concept context_weak_ptr = weak_ptr_requires<T, shizuku::context>;
template <typename T>
concept context_requires =
    requires(T context, int return_value,
             int (*entry)(void *, void *, void *, void *), void *arg1,
             void *arg2, void *arg3,
             void *arg4) { T(entry, arg1, arg2, arg3, arg4); };
template <typename T>
concept task_queue_requires = requires(T queue, shizuku::types::task task) {
  { queue.top() } -> shizuku::same_as<shizuku::types::task const &>;
  queue.push(task);
  queue.pop();
};
template <typename T>
concept cpu_driver_requires =
    requires(shizuku::context *context, int return_value,
             int (*entry)(void *, void *, ::shizuku::thread_weak_ptr &,
                          ::shizuku::object_weak_ptr &),
             void *arg1, void *arg2) {
      { T::get_core_num() } -> ::shizuku::same_as<uint>;
      { T::save_context(context) } -> ::shizuku::same_as<int>;
      { T::load_context(context, return_value) } -> ::shizuku::same_as<int>;
    };
template <typename T, typename U, typename S>
concept map_requires = requires(T map, U key, S value) {
  { map.operator[](key) } -> shizuku::same_as<S &>;
};
} // namespace concepts
} // namespace shizuku

static_assert(shizuku::concepts::cpu_driver_requires<
              shizuku::cpu_driver>); // cpu_driver check
static_assert(shizuku::concepts::object_shared_ptr_requires<
              shizuku::object_shared_ptr>); // check object_shared_ptr
static_assert(shizuku::concepts::object_weak_ptr_requires<
              shizuku::object_weak_ptr>); // check object_weak_ptr
static_assert(shizuku::concepts::thread_weak_ptr_requires<
              shizuku::thread_weak_ptr>); // check thread_weak_ptr
static_assert(shizuku::concepts::thread_shared_ptr_requires<
              shizuku::thread_shared_ptr>); // check thread_shared_ptr
static_assert(shizuku::concepts::context_shared_ptr<
              shizuku::context_shared_ptr>); // check context_shared_ptr
static_assert(shizuku::concepts::context_weak_ptr<
              shizuku::context_weak_ptr>); // check context_weak_ptr
static_assert(shizuku::concepts::task_queue_requires<
              shizuku::task_queue>); // check task queue
static_assert(shizuku::concepts::map_requires<shizuku::map<int, int>, int,
                                              int>); // check map
static_assert(shizuku::same_as<shizuku::atomic_size_t, shizuku::atomic_size_t>,
              "shizuku::atomic_size_t not defined!!!");
#endif