#ifndef SHIZUKU_HPP
#define SHIZUKU_HPP
namespace shizuku {
using size_t = unsigned int;
void *malloc(size_t size);
void context_switch();
} // namespace shizuku
#endif