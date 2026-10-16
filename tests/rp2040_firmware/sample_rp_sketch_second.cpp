#include <cstdint>

volatile uint32_t rp_second_sketch_state = 0;

void setup_second() {
    rp_second_sketch_state = 0x20402040;
}

void loop_second() {
    rp_second_sketch_state += 2;
}
