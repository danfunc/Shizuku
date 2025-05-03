#ifndef SHIZUKU_CPUDRIVER_RP2040_HPP
#define SHIZUKU_CPUDRIVER_RP2040_HPP
#include "pico/stdlib.h"

namespace shizuku{
    namespace cpu_drivers{
        class rp2040{
            public:
            void init();
            unsigned int get_core_num() const {
                return ::get_core_num();
            }
            struct context
            {
                void pop();
            };
            static uint32_t syscall();
            
        };
    }
} //

#endif // SHIZUKU_CPUDRIVER_RP2040_HPP