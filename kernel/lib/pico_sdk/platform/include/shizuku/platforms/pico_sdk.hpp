#ifndef PLATFORM_PICO_SDK_HPP
#define PLATFORM_PICO_SDK_HPP
#include "atomic"
#include "cstdint"
#include "deque"
#include "map"
#include "memory"
#include "queue"
#include "set"
#include "shizuku/memory_managers/pico_sdk.hpp"
#include "shizuku/method.hpp"
#include "string"
#include "vector"
namespace shizuku {
namespace types {
struct object;
struct thread;
struct task;
namespace processors {
namespace rp2040 {
struct context;
}
} // namespace processors

namespace platforms {
namespace pico_sdk {
using memory_manager =
    shizuku::types::memory_managers::pico_sdk::memory_manager;
namespace std = ::std;
using string = ::std::string;

} // namespace pico_sdk
} // namespace platforms
} // namespace types
using atomic_size_t = ::std::atomic_size_t;
using string = ::std::string;
using object_shared_ptr = ::std::shared_ptr<shizuku::types::object>;
using object_weak_ptr = ::std::weak_ptr<shizuku::types::object>;
using thread_shared_ptr = ::std::shared_ptr<shizuku::types::thread>;
using thread_weak_ptr = ::std::weak_ptr<shizuku::types::thread>;
using context_shared_ptr =
    ::std::shared_ptr<::shizuku::types::processors::rp2040::context>;
using context_weak_ptr =
    ::std::weak_ptr<::shizuku::types::processors::rp2040::context>;
using task_queue = ::std::priority_queue<::shizuku::types::task>;
using thread_map = ::std::map<int, shizuku::types::thread>;
using method_shared_ptr = ::std::shared_ptr<shizuku::types::method>;
using method_weak_ptr = ::std::weak_ptr<shizuku::types::method>;
template <typename T, typename U> using map = ::std::map<T, U>;
template <typename T> using shared_ptr = ::std::shared_ptr<T>;
} // namespace shizuku

#endif
