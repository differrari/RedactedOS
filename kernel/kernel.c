#include "console/kio.h"
#include "console/serial/uart.h"
#include "graph/graphics.h"
#include "pci.h"
#include "kstring.h"
#include "console/kconsole/kconsole.h"
#include "mmu.h"
#include "interrupts/exception_handler.h"
#include "memory/kalloc.h"
#include "dtb.h"
#include "interrupts/irq.h"
#include "process/scheduler.h"
#include "default_process.h"
#include "filesystem/disk.h"
#include "kernel_processes/boot/bootprocess.h"
#include "input/xhci_bridge.h"
#include "input/xhci.h"
#include "kernel_processes/monitor/monitor_processes.h"
#include "process_loader.h"
#include "memory/page_allocator.h"

void kernel_main() {
    
    mmu_alloc();
    enable_uart();
    kprintf("UART output enabled");
    // enable_talloc_verbose();
    
    set_exception_vectors();
    kprintf("Exception vectors set");

    page_allocator_init();
    kprintf("Initializing kernel...");
    
    init_main_process();

    kprintf("Preparing for draw");
    gpu_size screen_size = {1080,720};
    
    irq_init();
    kprintf("Interrupts initialized");

    enable_interrupt();
    gpu_init(screen_size);
    
    kprintf("GPU initialized");
    
    // page_alloc_enable_verbose();
    // xhci_enable_verbose();
    if (!xhci_input_init()){
        panic("Input initialization failure");
    }
    
    kprintf("Initializing disk...");
    
    // disk_verbose();
    if (!find_disk())
        panic("Disk initialization failure");

    // mmu_enable_verbose();
    mmu_init();
    kprintf("MMU Mapped");

    kprintf("Kernel initialization finished");

    kprintf("Starting processes");

    // translate_enable_verbose();

    init_bootprocess();
    
    kprintf("Starting scheduler");
    
    disable_interrupt();
    start_scheduler();

    panic("Kernel did not activate any process");
    
}