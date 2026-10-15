#include <cstdint>

volatile uint32_t comp_sketch_state = 0;

void setup() {
    comp_sketch_state = 0xAA55AA55;
}

void loop() {
    comp_sketch_state += 1;
}
