#include "uart.h"
#include "gpu/gpu.h"
#include "pci.h"

void kernel_main() {

    enable_uart();

    size screen_size = {1024,768};

    uart_puts("Hello world!\n");
    uart_puts("Preparing for draw\n");

    gpu_init(screen_size);
    gpu_clear(0x00FF00); 

    gpu_draw_line((point){0, screen_size.height/2}, (point){screen_size.width, screen_size.height/2}, 0xFF0000);

    uart_puts("Square drawn\n");
}