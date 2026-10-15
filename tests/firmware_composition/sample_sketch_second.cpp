// Second sketch with custom setup/loop symbol names to avoid collisions
#include <cstdint>

volatile uint32_t g_second_sketch_counter = 0;

void setup_second(void) {
    g_second_sketch_counter = 100;
}

void loop_second(void) {
    g_second_sketch_counter++;
}
