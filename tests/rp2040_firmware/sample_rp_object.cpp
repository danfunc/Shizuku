#include "sample_rp_object.hpp"

static volatile uint32_t g_rp_obj_counter = 0;

extern "C" {
void sample_rp_obj_main(void) {
    g_rp_obj_counter += 10;
}

uint32_t sample_rp_obj_get_count(void) {
    return g_rp_obj_counter;
}
}
