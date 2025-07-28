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
#include "networking/processes/net_proc.h"
#include "memory/page_allocator.h"
#include "networking/network.h"
#include "math/random.h"
#include "filesystem/filesystem.h" 

void kernel_main() {

    detect_hardware();
    
    //  page_alloc_enable_verbose();
    page_allocator_init();

    print_hardware();

    mmu_alloc();
    // mmu_enable_verbose();
    enable_uart();
    kprintf_l("UART output enabled");
    // enable_talloc_verbose();

    uint64_t seed;
    asm volatile("mrs %0, cntvct_el0" : "=r"(seed));
    rng_init_global(seed);
    kprintf("Random init. seed: %i\n", seed);

    set_exception_vectors();
    kprintf_l("Exception vectors set");

   
    kprintf_l("Initializing kernel...");
    
    init_main_process();

    kprintf_l("Preparing for draw");
    gpu_size screen_size = {1080,720};
    
    irq_init();
    kprintf("Interrupts initialized");

    enable_interrupt();

    kprintf_l("Initializing GPU");

    gpu_init(screen_size);
    
    kprintf("GPU initialized");
    
    kprintf("Initializing disk...");

    if (BOARD_TYPE == 2 && RPI_BOARD >= 5)
        pci_setup_rp1();
    
    // disk_verbose();
    if (!init_disk_device())
        panic("Disk initialization failure");

    // xhci_enable_verbose();
    if (!input_init())
        panic("Input initialization error");

    bool network_available = network_init();
    init_input_process();

    mmu_init();
    kprintf_l("MMU Mapped");

    if (!init_boot_filesystem())
        panic("Filesystem initialization failure");

    init_dev_filesystem();

    kprintf_l("Kernel initialization finished");

    kprintf_l("Starting processes");

    if (network_available) launch_net_process();

    init_bootprocess();
    
    kprintf_l("Starting scheduler");
    
    start_scheduler();

    panic("Kernel did not activate any process");
    
}