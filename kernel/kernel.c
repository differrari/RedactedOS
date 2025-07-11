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
#include "shared/math/random.h"

void kernel_main() {

    detect_hardware();
    
    mmu_alloc();
    // mmu_enable_verbose();
    enable_uart();
    kprintf_l("UART output enabled");
    // enable_talloc_verbose();
    uint64_t seed;
    asm volatile("mrs %0, cntvct_el0" : "=r"(seed)); //virtual timer counter as entropy source for rng seed
    rng_init_global(seed);
    kprintf("Random init. seed: %i\n", seed);
    //kprintf("Next32: %i\n", rng_next32(&global_rng));
    set_exception_vectors();
    kprintf_l("Exception vectors set");

    print_hardware();

    page_allocator_init();
    // page_alloc_enable_verbose();
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
    
    // disk_verbose();
    if (!find_disk())
        panic("Disk initialization failure");

    // xhci_enable_verbose();
    if (!input_init())
        panic("Input initialization error");

    bool network_available = network_init();

    mmu_init();
    kprintf_l("MMU Mapped");

    if (!disk_init())
        panic("Disk read failure");

    kprintf_l("Kernel initialization finished");

    kprintf_l("Starting processes");

    if (network_available) launch_net_process();

    init_bootprocess();
    
    kprintf_l("Starting scheduler");
    
    disable_interrupt();
    start_scheduler();

    panic("Kernel did not activate any process");
    
}
