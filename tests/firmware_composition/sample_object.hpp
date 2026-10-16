#pragma once
#include <cstdint>

namespace sample_module {
uintptr_t get_my_id();
}

extern "C" {
void sample_obj_main(void);
}
