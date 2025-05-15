#ifndef SHIZUKU_CPUDRIVER_RP2040_HPP
#define SHIZUKU_CPUDRIVER_RP2040_HPP
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "initializer_list"

namespace shizuku{
    namespace cpu_drivers{
        class rp2040{
            public:
            void init();
            static unsigned int get_core_num()  {
                return get_core_num();
            }
            struct context
            {
                void pop();
            };
            static uint32_t syscall();
            private:
            static uint32_t svc_handler(uint32_t arg0,uint32_t arg1,uint32_t arg2,uint32_t arg3);
            
        };
    }
} //

#endif // SHIZUKU_CPUDRIVER_RP2040_HPP