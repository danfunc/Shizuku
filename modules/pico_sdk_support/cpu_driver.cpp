#include <shizuku/cpu_drivers/rp2040.hpp>
#include <shizuku/concepts/cpu_driver.hpp>
#include <hardware/interp.h>

static_assert(shizuku::concepts::cpu_driver_requires<shizuku::cpu_drivers::rp2040>, "rp2040 does not satisfy cpu_driver concept");

using namespace shizuku;
using namespace cpu_drivers;

void rp2040::init(){
    
}