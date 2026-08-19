#ifndef SHIZUKU_KERNEL_OBJECT_HPP
#define SHIZUKU_KERNEL_OBJECT_HPP
#include "shizuku/config.hpp"
namespace shizuku {
// 構成で確定したカーネルオブジェクトの実体。カーネルと同じく静的領域に置く
// (将来オブジェクトへ渡す MPU region から構造的に外すため)。
extern shizuku::KERNEL_OBJECT kernel_object_instance;
} // namespace shizuku
#endif // SHIZUKU_KERNEL_OBJECT_HPP
