#include <stdio.h>
#include <unistd.h>

#ifndef GPU_MODE
#define GPU_MODE 0
#endif

int main(void)
{
    if (GPU_MODE == 1) {
        sleep(30);
        return 0;
    }
    if (GPU_MODE == 2) {
        puts("N/A");
        return 0;
    }
    if (GPU_MODE == 3) {
        puts("60");
        return 1;
    }
    if (GPU_MODE == 4) {
        for (int i = 0; i < 500; i++)
            puts("60");
        return 0;
    }
    puts("45\n63");
    return 0;
}
