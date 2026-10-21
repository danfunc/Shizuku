#ifndef SHIZUKU_SELFTEST_DISPATCHER_V1_CONFIG_HPP
#define SHIZUKU_SELFTEST_DISPATCHER_V1_CONFIG_HPP

#include "shizuku/config.hpp"

namespace shizuku::chips::esp32_c6 {
template <>
bool plic_vector_mode::faulted_in_thread_mode<
    shizuku::archs::rv32_c6::context_t>(
    const shizuku::archs::rv32_c6::context_t &);
} // namespace shizuku::chips::esp32_c6

#endif
