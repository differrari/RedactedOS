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
#include "process/default_process.h"

void kernel_main() {
    
    enable_uart();

    kprintf("Initializing kernel...");

    kprintf("Reading device tree %h",get_total_ram());
    
    kprintf("UART output enabled");
    
    size screen_size = {1024,768};
    
    kprintf("Preparing for draw");
    
    gpu_init(screen_size);
    
    kprintf("GPU initialized");
    
    kprintf("Device initialization finished");
    
    set_exception_vectors();

    kprintf("Exception vectors set");

    gic_init();

    kprintf("Interrupts init");


    kprintf("Interrupts enabled");

    mmu_enable_verbose();
    mmu_init();
    kprintf("MMU Mapped");

    kprintf("Kernel initialization finished");
    
    kprintf("Preparing user memory...");

    kprintf("There's %h memory for user processes",get_total_user_ram());

    kprintf("Starting default processes");

    default_processes();

    kprintf("Starting scheduler");


    start_scheduler();

    kprintf_raw("Error: Kernel did not activate any process");
    
}