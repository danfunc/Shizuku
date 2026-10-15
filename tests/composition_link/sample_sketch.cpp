// Sample sketch test file for shizuku_sketch API
#include <cstdint>

volatile uint32_t sketch_counter = 0;

void setup() {
    sketch_counter = 1;
}

void loop() {
    sketch_counter = sketch_counter + 1;
}
