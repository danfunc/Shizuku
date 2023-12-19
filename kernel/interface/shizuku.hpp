#ifndef SHIZUKU_HPP
#define SHIZUKU_HPP
#ifndef __cplusplus
namespace shizuku {
using size_t = unsigned int;
void *malloc(size_t size);
void context_switch();
int add_thread(int (*entry)(int, char **), int argc, char *argv[]);
} // namespace shizuku
#endif
#endif