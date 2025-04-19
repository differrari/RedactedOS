#include "uart.h"
#include "framebuffer.h"
#include "pci.h"

void kernel_main() {

    enable_uart();

    uart_puts("Hello world!\n");
    uart_puts("Preparing for draw\n");
    // gpu_debug();
    gpu_init();
    // gpu_clear(0x00FF00FF); 
    uart_puts("Square drawn\n");
}