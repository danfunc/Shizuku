#include "stdio.h"
#include "pico/stdlib.h"
#include "shizuku_entry.h"

int main(void)
{
    stdio_init_all();
    shizuku_entry();
    
    while (1)
    {
        sleep_ms(100);
        printf("hello");
        sleep_ms(100);
    }
    
    return 0;
}
