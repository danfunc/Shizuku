#ifndef SHIZUKU_ENTRY
#define SHIZUKU_ENTRY
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*method)(size_t, size_t, size_t, size_t);
void shizuku_entry(size_t argc, char **argv, method init_obj_entry);
#ifdef __cplusplus
}
#endif
#endif