// RP2350 SVC implementation in one translation unit for non-LTO inlining.
#include "shizuku/kernel.hpp"

namespace shizuku {
template <> uintptr_t KERNEL::current_object(uint32_t) const;
template <> uintptr_t KERNEL::caller_object(uint32_t) const;
} // namespace shizuku

#include "../kernel_object/handler.cpp"
#include "dispatch.cpp"
#include "../kernel_object/memory.cpp"
#include "shizuku/armv8m_svc.hpp"
