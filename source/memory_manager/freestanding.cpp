#include "shizuku/memory_managers/freestanding.hpp"
#include <cstddef>

extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);

namespace shizuku {
namespace memory_managers {

templates::result<void *> freestanding::kernel_malloc(uintptr_t size) {
  void *addr = malloc(size);
  if (addr != nullptr) {
    return addr;
  }
  return {result_code::allocation_failed, "malloc failed\n"};
}

templates::result<void> freestanding::kernel_free(void *ptr) {
  free(ptr);
  return {};
}

templates::result<void> freestanding::init() {
  return {};
}

} // namespace memory_managers
} // namespace shizuku
