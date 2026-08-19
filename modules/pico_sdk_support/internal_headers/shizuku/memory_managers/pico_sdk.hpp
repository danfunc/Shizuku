#ifndef SHIZUKU_MEMORY_MANAGER_PICO_SDK_HPP
#define SHIZUKU_MEMORY_MANAGER_PICO_SDK_HPP
#include "cstdint"
#include "shizuku/templates/result.hpp"
namespace shizuku {
namespace memory_managers {
class pico_sdk {
  public:
  struct memory_map_t {
  };
  templates::result<void*> kernel_malloc(uintptr_t size);
  templates::result<void> kernel_free(void* ptr);
  templates::result<void> init();
};
}; // namespace memory_managers
}; // namespace shizuku
#endif // SHIZUKU_MEMORY_MANAGER_PICO_SDK_HPP