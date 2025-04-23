#include "console/kio.h"
#include "console/serial/uart.h"
#include "graph/graphics.h"
#include "pci.h"
#include "string.h"
#include "console/kconsole/kconsole.h"
#include "mmu.h"
#include "exception_handler.h"
#include "ram_e.h"
#include "gic.h"

void kernel_main() {

    printf("Kernel initializing...");

    string s = string_format("Hello. This is a test panic for %h",0x0);
    
    enable_uart();
    
    printf("UART output enabled");
    
    size screen_size = {1024,768};
    
    printf("Preparing for draw");
    
    gpu_init(screen_size);
    
    
    printf("GPU initialized");
    
    printf("Device initialization finished");
    
    set_exception_vectors();

    printf("Exception vectors set");

    gic_init();

    printf("Interrupts init");

    timer_init(1000);

    printf("Test timer done");

    enable_interrupt();

    printf("Interrupts enabled");

    mmu_init();
    printf("MMU Mapped");

    printf("Kernel initialization finished");
    printf("Now we're writing a really long string, because we both want to check string buffers and we want to check how visual console handles growth and wrapping and all that stuff. So it needs to be long, like really long, over 256 string sizes. We're almost there");
}