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

void func1(){
    while (1)
    {
        printf("First process");
    }
}

void func2(){
    while (1)
    {
        printf("Second process");
    }
}

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

    enable_interrupt();

    printf("Interrupts enabled");

    mmu_init();
    printf("MMU Mapped");

    printf("Kernel initialization finished");
    
    printf("Preparing user memory...");

    printf("There's %h memory for user processes",get_total_user_ram());

    printf("Starting 2 processes");

    create_process(func1);
    create_process(func2);

    printf("Starting scheduler");

    start_scheduler();

    printf("scheduler started");

    while (1)
    {
        printf("Current process %i", get_current_proc());
    }
    
}