#include "console/kio.h"
#include "console/serial/uart.h"
#include "graph/graphics.h"
#include "hw/hw.h"
#include "pci.h"
#include "memory/mmu.h"
#include "exceptions/exception_handler.h"
#include "exceptions/irq.h"
#include "process/scheduler.h"
#include "filesystem/disk.h"
#include "kernel_processes/boot/bootprocess.h"
#include "input/input_dispatch.h"
#include "networking/processes/net_proc.h"
#include "memory/page_allocator.h"
#include "networking/network.h"
#include "dev/random/random.h"
#include "filesystem/filesystem.h"
#include "dev/module_loader.h" 
#include "audio/audio.h"

void kernel_main() {

    detect_hardware();
    
    //  page_alloc_enable_verbose();
    page_allocator_init();

    set_exception_vectors();

    init_main_process();

    load_module(&console_module);
    kprint("UART output enabled");

    // mmu_enable_verbose();
    mmu_alloc();

    print_hardware();

    load_module(&rng_module);

    kprint("Exception vectors set");
   
    kprint("Initializing kernel...");
    
    irq_init();
    kprintf("Interrupts initialized");

    enable_interrupt();

    load_module(&graphics_module);
    
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
    
    load_module(&audio_module);

    init_input_process();

    mmu_init();
    kprint("MMU Mapped");

    if (!init_boot_filesystem())
        panic("Filesystem initialization failure");

    kprint("Kernel initialization finished");

    kprint("Starting processes");

    if (network_available) launch_net_process();

    init_bootprocess();
    
    kprint("Starting scheduler");
    
    start_scheduler();

    panic("Kernel did not activate any process");
    
}