#include "console/kio.h"
#include "console/serial/uart.h"
#include "graph/graphics.h"
#include "hw/hw.h"
#include "pci.h"
#include "kstring.h"
#include "console/kconsole/kconsole.h"
#include "memory/mmu.h"
#include "exceptions/exception_handler.h"
#include "memory/kalloc.h"
#include "exceptions/irq.h"
#include "process/scheduler.h"
#include "filesystem/disk.h"
#include "kernel_processes/boot/bootprocess.h"
#include "input/input_dispatch.h"
#include "kernel_processes/monitor/monitor_processes.h"
#include "networking/processes/net_proc.h"
#include "memory/page_allocator.h"
#include "networking/network.h"

void kernel_main() {

    detect_hardware();
    
    mmu_alloc();
    // mmu_enable_verbose();
    enable_uart();
    kprintf("UART output enabled");
    // enable_talloc_verbose();
    
    set_exception_vectors();
    kprintf("Exception vectors set");

    print_hardware();

    page_allocator_init();
    // page_alloc_enable_verbose();
    kprintf("Initializing kernel...");
    
    init_main_process();

    kprintf("Preparing for draw");
    gpu_size screen_size = {1080,720};
    
    irq_init();
    kprintf("Interrupts initialized");

    enable_interrupt();

    kprintf("Initializing GPU");

    gpu_init(screen_size);
    
    kprintf("GPU initialized");
    
    kprintf("Initializing disk...");
    
    // disk_verbose();
    if (!find_disk())
        panic("Disk initialization failure");

    // xhci_enable_verbose();
    if (!input_init())
        panic("Input initialization error");

    bool network_available = network_init();

    mmu_init();
    kprintf("MMU Mapped");

    if (!disk_init())
        panic("Disk read failure");

    kprintf("Kernel initialization finished");

    kprintf("Starting processes");

    if (network_available) launch_net_process();

    init_bootprocess();
    
    kprintf("Starting scheduler");
    
    disable_interrupt();
    start_scheduler();

    panic("Kernel did not activate any process");
    
}