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
    
    page_allocator_init();

    set_exception_vectors();

    init_main_process();

    load_module(&console_module);

    mmu_alloc();

    print_hardware();

    load_module(&rng_module);
    
    irq_init();
    kprintf("Interrupts initialized");

    enable_interrupt();

    load_module(&graphics_module);
    
    if (BOARD_TYPE == 2 && RPI_BOARD >= 5)
        pci_setup_rp1();

    load_module(&disk_module);

    bool input_available = load_module(&input_module);

    //TODO: look into why networking blocks other things
    bool network_available = load_module(&net_module);
    
    load_module(&audio_module);
    
    mmu_init();

    kprint("Kernel initialization finished");
    
    kprint("Starting processes");
    
    init_boot_filesystem();
    if (input_available) init_input_process();

    if (network_available) launch_net_process();

    init_bootprocess();

    load_module(&scheduler_module);

    panic("Kernel did not activate any process");
    
}
