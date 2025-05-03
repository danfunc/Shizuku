#ifndef SHIZUKU_CPU_MANAGER_HPP
#define SHIZUKU_CPU_MANAGER_HPP
#include "shizuku/concepts/cpu_driver.hpp"
#include "cstdint"
namespace shizuku {
namespace templates {
template <shizuku::concepts::cpu_driver_requires CPU_DRIVER, uint32_t core_count> class cpu_manager {};
} // namespace Templates
} // namespace shizuku
#endif // SHIZUKU_CPU_MANAGER_HPP