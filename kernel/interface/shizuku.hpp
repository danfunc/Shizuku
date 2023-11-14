#ifndef SHIZUKU_HPP
#define SHIZUKU_HPP
#include "cstdint"
namespace shizuku {
void *malloc(std::size_t size);
void context_switch();
} // namespace shizuku
#endif