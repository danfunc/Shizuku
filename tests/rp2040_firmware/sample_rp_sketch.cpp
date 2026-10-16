#include <cstdint>

volatile uint32_t rp_sketch_state = 0;

void setup() {
    rp_sketch_state = 0x5049434F; // 'PICO'
}

void loop() {
    rp_sketch_state += 1;
}
