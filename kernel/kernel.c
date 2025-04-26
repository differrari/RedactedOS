#include "console/kio.h"
#include "console/serial/uart.h"
#include "graph/graphics.h"
#include "pci.h"
#include "string.h"
#include "console/kconsole/kconsole.h"
#include "mmu.h"
#include "exception_handler.h"
#include "ram_e.h"
#include "dtb.h"
#include "gic.h"
#include "process/scheduler.h"

void kernel_main() {
    
    enable_uart();

    printf("Initializing kernel...");

    printf("Reading device tree %h",get_total_ram());
    
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


    printf("Interrupts enabled");

    // mmu_enable_verbose();
    mmu_init();
    printf("MMU Mapped");

    printf("Kernel initialization finished");
    
    printf("Preparing user memory...");

    printf("There's %h memory for user processes",get_total_user_ram());

    printf("Starting default processes");

    default_processes();

    printf("Starting scheduler");

    enable_interrupt();

    start_scheduler();

    printf("Error: Kernel did not activate any process");
    
}