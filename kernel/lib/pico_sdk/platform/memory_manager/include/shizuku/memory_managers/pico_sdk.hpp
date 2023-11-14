#ifndef PICO_SDK_MEMORY_MANAGER
#define PICO_SDK_MEMORY_MANAGER
#include "stdint.h"
namespace shizuku {
namespace types {
namespace memory_managers {
namespace pico_sdk {
class memory_manager {
private:
  using size_t = unsigned int;

public:
  void *malloc(size_t size);
  void free(void *pointer);
};
} // namespace pico_sdk
} // namespace memory_managers
} // namespace types
} // namespace shizuku

#endif