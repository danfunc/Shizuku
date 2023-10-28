#include "stdio.h"
#include "pico/stdlib.h"

int main(int argc, char const *argv[])
{
    stdio_init_all();
 
    while (1)
    {
        sleep_ms(100);
        printf("hello");
        sleep_ms(100);
    }
    
    return 0;
}
