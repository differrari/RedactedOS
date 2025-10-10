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
#include "mailbox/mailbox.h"

void kernel_main() {

    detect_hardware();

    mmu_alloc();
    
    if (BOARD_TYPE == 2) mailbox_init();

    page_allocator_init();

    set_exception_vectors();

    init_main_process();

//    if (BOARD_TYPE == 1) disable_visual();

    load_module(&console_module);

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
    bool network_available = false;
    if (BOARD_TYPE == 1){
        //TODO: disabling networking until it is refactored to prevent memory issues
        network_available = load_module(&net_module);

        load_module(&audio_module);

        init_audio_mixer();
    }
    
    mmu_init();

    kprint("Kernel initialization finished");
    
    kprint("Starting processes");
    
    init_filesystem();
    if (input_available) init_input_process();

    if (network_available) launch_net_process();

    init_bootprocess();

    load_module(&scheduler_module);

    panic("Kernel did not activate any process", 0);
    
}
