#include "tests/firmware_composition/sample_object.hpp"
#include "internal_headers/shizuku/object_id_decl.hpp"

SHIZUKU_DECLARE_OBJECT_ID(test_sample_object)

namespace sample_module {

uintptr_t get_my_id() {
    return shizuku::object_id::test_sample_object;
}

} // namespace sample_module
