#include "shizuku/memory_managers/pico_sdk.hpp"
#include "stdio.h"
#include "stdlib.h"

using namespace shizuku::memory_managers;
using namespace shizuku::templates;
shizuku::templates::result<void *> pico_sdk::kernel_malloc(uintptr_t size) {
  void *addr = ::malloc(size);
  if (addr != nullptr) {
    printf("malloc success\n");
    return addr;
  } else {
    return {result_code::allocation_failed, "malloc failed\n"};
  };
}
shizuku::templates::result<void> pico_sdk::kernel_free(void *ptr) {
  ::free(ptr);
  return {}; // return success;
}
shizuku::templates::result<void> pico_sdk::init() {
  return {}; // return success;
}