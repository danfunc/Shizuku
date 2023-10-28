#include "stdio.h"
#include "pico/stdlib.h"

void entry();

int main(int argc, char const *argv[])
{
    stdio_init_all();
    entry();
    while (1)
    {
        sleep_ms(100);
        printf("hello");
        sleep_ms(100);
    }
    
    return 0;
}
