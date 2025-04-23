#include "console/kio.h"
#include "console/serial/uart.h"
#include "graph/graphics.h"
#include "pci.h"
#include "string.h"
#include "console/kconsole/kconsole.h"
#include "mmu.h"

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
    mmu_init();

    printf("Now we're writing a really long string, because we both want to check string buffers and we want to check how visual console handles growth and wrapping and all that stuff. So it needs to be long, like really long, over 256 string sizes. We're almost there");
}