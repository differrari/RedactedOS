#include "hw.h"
#include "console/kio.h"
#include "gpio.h"

uint8_t BOARD_TYPE;
uint8_t RPI_BOARD;
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
uintptr_t MAILBOX_BASE = 0;
uintptr_t GPIO_BASE;
uintptr_t GPIO_PIN_BASE;
uintptr_t DWC2_BASE;

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
        uint32_t raspi = (reg >> 4) & 0xFFF;
        switch (raspi) {
            case 0xD08:  //4B. Cortex A72
                MMIO_BASE = 0xFE000000; 
                RPI_BOARD = 4;
                GPIO_PIN_BASE = 0x50;
            break;
            case 0xD0B:  //5. Cortex A76
                MMIO_BASE = 0x107C000000UL;
                RPI_BOARD = 5;
                GPIO_PIN_BASE = 0x50;
            break;
            default:  
                RPI_BOARD = 3;
                MMIO_BASE = 0x3F000000; 
            break;
        }
        if (RPI_BOARD == 3){
            GICD_BASE = MMIO_BASE + 0xB200;
        } else {
            GICD_BASE = MMIO_BASE + 0x1841000;
            GICC_BASE = MMIO_BASE + 0x1842000;
        }
        if (RPI_BOARD == 5){
            MAILBOX_BASE = MMIO_BASE + 0x13880;
            //EMMC base becomes 0x1000FFF000UL
            UART0_BASE = MMIO_BASE + 0x1001000;
        } else {
            GPIO_BASE  = MMIO_BASE + 0x200000;
            MAILBOX_BASE = MMIO_BASE + 0xB880;
            UART0_BASE = MMIO_BASE + 0x201000;
        }
        SDHCI_BASE = MMIO_BASE + 0x300000;
        DWC2_BASE  = MMIO_BASE + 0x980000;
        XHCI_BASE  = MMIO_BASE + 0x9C0000;
        RAM_START       = 0x10000000;
        CRAM_END        = (MMIO_BASE - 0x10000000) & 0xF0000000;
        RAM_START       = 0x10000000;
        CRAM_START      = 0x13600000;
        if (RPI_BOARD != 5) reset_gpio();
    }
}

void print_hardware(){
    kprintf("Board type %i",BOARD_TYPE);
}