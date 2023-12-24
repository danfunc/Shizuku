#ifndef PLATFORM_PICO_SDK_HPP
#define PLATFORM_PICO_SDK_HPP
#include "cstdint"
#include "deque"
#include "shizuku/memory_managers/pico_sdk.hpp"
#include "string"
#include "vector"
namespace shizuku {
namespace types

{
namespace platforms {
namespace pico_sdk {
using memory_manager =
    shizuku::types::memory_managers::pico_sdk::memory_manager;
namespace std = ::std;
using string = ::std::string;
} // namespace pico_sdk
} // namespace platforms

} // namespace types

} // namespace shizuku

#endif
