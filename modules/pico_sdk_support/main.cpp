#include "shizuku/kernel.hpp"
#include "shizuku/cpu_drivers/rp2040.hpp"
int main(){
    shizuku::cpu_drivers::rp2040::get_core_num();
}