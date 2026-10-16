#include "tests/firmware_composition/sample_object.hpp"
#include "shizuku/object_id_decl.hpp"

SHIZUKU_DECLARE_OBJECT_ID(sample_obj)

namespace sample_module {

uintptr_t get_my_id() {
    return shizuku::object_id::sample_obj;
}

} // namespace sample_module

extern "C" {

void sample_obj_main(void) {
}

} // extern "C"
