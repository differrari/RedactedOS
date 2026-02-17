#include "console/kio.h"
#include "console/serial/uart.h"
#include "graph/graphics.h"
#include "hw/hw.h"
#include "memory/talloc.h"
#include "pci.h"
#include "memory/mmu.h"
#include "exceptions/exception_handler.h"
#include "exceptions/irq.h"
#include "process/scheduler.h"
#include "filesystem/disk.h"
#include "kernel_processes/boot/bootprocess.h"
#include "usb/usb.h"
#include "networking/processes/net_proc.h"
#include "memory/page_allocator.h"
#include "networking/network.h"
#include "random/random.h"
#include "filesystem/filesystem.h"
#include "dev/module_loader.h" 
#include "audio/audio.h"
#include "mailbox/mailbox.h"
#include "math/vector.h"
#include "process/debug.h"
#include "theme/theme.h"
#include "tests/test_runner.h"
#include "pci/pcie.h"

void kernel_main() {

    detect_hardware();
    
    pre_talloc();
    mmu_init();
    if (BOARD_TYPE == 2) mailbox_init();

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
    
    bool can_init_usb = true;
    
#if !QEMU
    if (BOARD_TYPE == 2){
       if (!init_hostbridge()) can_init_usb = false;
       if (RPI_BOARD >= 5)
           pci_setup_rp1();
    }
#endif

    load_module(&disk_module);

    bool usb_available = can_init_usb ? load_module(&usb_module) : false;
    bool network_available = false;
    if (BOARD_TYPE == 1){
        if (system_config.use_net)
            network_available = load_module(&net_module);

        load_module(&audio_module);
    }

    kprint("Kernel initialization finished");
    
    kprint("Starting processes");

    if (BOARD_TYPE == 1) init_audio_mixer();
    
    init_filesystem();

    debug_load();
    
    load_module(&theme_mod);

#if TEST
    if (!run_tests()) panic("Test run failed",0);
#endif

    if (usb_available) init_usb_process();

    if (network_available && system_config.use_net) launch_net_process();

    init_bootprocess();

    load_module(&scheduler_module);
    
    start_scheduler();

    panic("Kernel did not activate any process", 0);
    
}
