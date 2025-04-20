#include "console/kio.h"
#include "console/serial/uart.h"
#include "graph/gpu.h"
#include "pci.h"
#include "string.h"

void kernel_main() {

    printf("Kernel initializing...");

    enable_uart();

    printf("UART output enabled");

    size screen_size = {1024,768};

    printf("Preparing for draw");

    gpu_init(screen_size);

    printf("GPU initialized");

    gpu_clear(0x00FF00); 

    printf("Screen initialized");

    gpu_draw_line((point){0, screen_size.height/2}, (point){screen_size.width, screen_size.height/2}, 0xFF0000);

    printf("Kernel initialized");
}