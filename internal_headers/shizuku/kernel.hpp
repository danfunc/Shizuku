#ifndef SHIZUKU_KERNEL_HPP
#define SHIZUKU_KERNEL_HPP
#include "shizuku/config.hpp"
namespace shizuku {
// 構成 (configs/config_template.hpp.in) で確定した型のカーネル実体。
// 1 バイナリ 1 構成なので実体化も 1 つで、実装は KERNEL への明示的特殊化として
// source/kernel/ に置く。
extern shizuku::KERNEL kernel_instance;
} // namespace shizuku
#endif // SHIZUKU_KERNEL_HPP
