#include "console/kio.h"
#include "console/serial/uart.h"
#include "graph/graphics.h"
#include "pci.h"
#include "string.h"
#include "console/kconsole/kconsole.h"

void kernel_main() {

    printf("Kernel initializing...");

    enable_uart();

    printf("UART output enabled");

    size screen_size = {1024,768};

    printf("Preparing for draw");

    gpu_init(screen_size);

    uart_puts("Testlog\n");

    printf("GPU initialized");

    // kconsole_clear();

    printf("Kernel initialization finished");
}