#ifndef SHIZUKU_CPU_DRIVER_CONCEPT_HPP
#define SHIZUKU_CPU_DRIVER_CONCEPT_HPP
#include <concepts>
#include <shizuku/concepts/context.hpp>
namespace shizuku{
    namespace concepts{
        template <typename CPU_DRIVER_CLASS>
        concept cpu_driver_requires = requires(CPU_DRIVER_CLASS cpu_driver){
            {cpu_driver.get_core_num()} -> std::same_as<unsigned int>;
            requires shizuku::concepts::context_requires<typename CPU_DRIVER_CLASS::context>;
            CPU_DRIVER_CLASS::syscall();
        };
    }
} //
#endif // SHIZUKU_CPU_DRIVER_HPP