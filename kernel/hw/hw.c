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
uintptr_t MMIO_BASE = 0;
uintptr_t GICD_BASE = 0;
uintptr_t GICC_BASE = 0;
uintptr_t SDHCI_BASE = 0;

uint8_t RPI_BOARD;

void detect_hardware(){
    if (BOARD_TYPE == 1){
        UART0_BASE = 0x9000000;
        MMIO_BASE = 0x10010000;
        CRAM_END        = 0x60000000;
        RAM_START       = 0x40000000;
        CRAM_START      = 0x43600000;
        USE_PCI = 1;
        GICD_BASE = 0x08000000;
        GICC_BASE = 0x08010000;

    } else {
        uint32_t reg;
        asm volatile ("mrs %x0, midr_el1" : "=r" (reg));
        //We can detect if we're running on 0x400.... to know if we're in virt. And 41 vs 40 to know if we're in img or elf
        uint32_t raspi = (reg >> 4) & 0xFFF;
        switch (raspi) {
            case 0xD08:  
            MMIO_BASE = 0xFE000000; 
            GICD_BASE = 0xff841000;
            GICC_BASE = 0xff842000;
            RPI_BOARD = 4;
            break;;
            default:  MMIO_BASE = 0x3F000000; break;
        }
         UART0_BASE = MMIO_BASE + 0x201000;
         SDHCI_BASE = MMIO_BASE + 0x300000;
         XHCI_BASE  = MMIO_BASE + 0x9C0000;
         RAM_START       = 0x10000000;
         CRAM_END        = (MMIO_BASE - 0x10000000) & 0xF0000000;
         RAM_START       = 0x10000000;
         CRAM_START      = 0x13600000;
    }
}

void print_hardware(){
    kprintf("Board type %i",BOARD_TYPE);
}

void delay(uint32_t ms) {
    uint64_t freq;
    asm volatile ("mrs %0, cntfrq_el0" : "=r"(freq));

    uint64_t ticks;
    asm volatile ("mrs %0, cntpct_el0" : "=r"(ticks));

    uint64_t target = ticks + (freq / 1000) * ms;

    while (1) {
        uint64_t now;
        asm volatile ("mrs %0, cntpct_el0" : "=r"(now));
        if (now >= target) break;
    }
}