#include "hw.h"
#include "console/kio.h"

uint8_t BOARD_TYPE;
uint8_t USE_DTB = 0;
uint8_t USE_PCI = 0;
uintptr_t RAM_START = 0;
uintptr_t RAM_SIZE = 0;
uintptr_t CRAM_START = 0;
uintptr_t CRAM_END = 0;
uintptr_t UART0_BASE = 0;
uintptr_t XHCI_BASE = 0;

void detect_hardware(){
    if (BOARD_TYPE == 1){
        UART0_BASE = 0x9000000;
    } else {
        uint32_t reg;
        asm volatile ("mrs %x0, midr_el1" : "=r" (reg));
        //We can detect if we're running on 0x400.... to know if we're in virt. And 41 vs 40 to know if we're in img or elf
        uint32_t raspi = (reg >> 4) & 0xFFF;
        switch (raspi) {
            case 0xC07:
            case 0xD03:  UART0_BASE = 0x3F201000; break;
            case 0xD08:  UART0_BASE = 0xFE201000; break;;
            default: UART0_BASE = 0x20201000; break;
        }
    }

    RAM_START       = 0x40000000;
    RAM_SIZE        = 0x20000000;
    CRAM_END        = 0x60000000;
    CRAM_START      = 0x43600000;
    XHCI_BASE       = 0xFE9C0000;
    USE_PCI         = BOARD_TYPE == 1;
}

void print_hardware(){
    kprintf("Board type %i",BOARD_TYPE);
}