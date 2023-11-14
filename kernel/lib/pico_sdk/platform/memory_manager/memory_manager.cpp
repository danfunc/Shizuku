#include "shizuku/memory_managers/pico_sdk.hpp"
#include "stdlib.h"
using namespace shizuku::types::memory_managers::pico_sdk;
void *memory_manager::malloc(size_t size) { return ::malloc(size); }
void memory_manager::free(void *pointer) { ::free(pointer); };