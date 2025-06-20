#ifndef SHIZUKU_RP2040_ABI_HPP
#define SHIZUKU_RP2040_ABI_HPP
#include <cstdint>
namespace shizuku{
    namespace abis{
        class rp2040{
            public:
            template<void (*func)(uint32_t, uint32_t,uint32_t,uint32_t,uint32_t)>
            static void abi_caller_converter(uint32_t arg0, uint32_t arg1,uint32_t arg2,uint32_t arg3,uint32_t syscall_num){
                asm (
                    "mov r0,%0;"
                    "mov r1,%1;"
                    "mov r2,%2;"
                    "mov r3,%3;"
                    "mov r12,%4;"
                    "bl %c[func];");
            };
            template<void (*func)(uint32_t, uint32_t,uint32_t,uint32_t,uint32_t)>
            static void abi_callee_converter(){
                uint32_t arg0, arg1, arg2, arg3, syscall_num;
                asm (
                    "mov %0, r0;"
                    "mov %1, r1;"
                    "mov %2, r2;"
                    "mov %3, r3;"
                    "mov %4,r12;"
                    : "=r"(arg0), "=r"(arg1), "=r"(arg2), "=r"(arg3),
                      "=r"(syscall_num));
                func(arg0, arg1, arg2, arg3, syscall_num);
            };
            using method_t = uint32_t (*)(uint32_t arg0, uint32_t arg1, uint32_t arg2,
                                    uint32_t arg3);
        };
    };
};
#endif // SHIZUKU_RP2040_ABI_HPP