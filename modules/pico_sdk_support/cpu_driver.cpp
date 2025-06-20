#include <shizuku/cpu_drivers/rp2040.hpp>
#include <shizuku/concepts/cpu_driver.hpp>
#include <hardware/exception.h>


static_assert(shizuku::concepts::cpu_driver_requires<shizuku::cpu_drivers::rp2040>, "rp2040 does not satisfy cpu_driver concept");

using namespace shizuku;
using namespace cpu_drivers;

void rp2040::init(){
    exception_set_exclusive_handler(SVCALL_EXCEPTION,(exception_handler_t)svc_handler);
}
void rp2040::trap(){
    while (1)
    {
        #ifdef DEBUG
        printf("Trap occurred! Core: %d\n", get_core_num());
        #endif
    }
}