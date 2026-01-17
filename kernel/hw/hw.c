#include "hw.h"
#include "console/kio.h"
#include "gpio.h"
#include "sysregs.h"

uint8_t BOARD_TYPE;
uint8_t RPI_BOARD;
uint8_t USE_DTB = 0;
uintptr_t PCI_BASE = 0;
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
uint32_t MSI_OFFSET;
uintptr_t LOWEST_ADDR;
uintptr_t PM_BASE;

void detect_hardware(){
    if (BOARD_TYPE == 1){
        UART0_BASE = 0x9000000;
        MMIO_BASE = 0x10010000;
        CRAM_END        = 0x60000000;
        RAM_START       = 0x40000000;
        CRAM_START      = 0x43600000;
        PCI_BASE = 0x4010000000;
        GICD_BASE = 0x08000000;
        GICC_BASE = 0x08010000;
        MSI_OFFSET = 50;
        LOWEST_ADDR = GICD_BASE;
    } else {
        uint32_t reg;
        asm volatile ("mrs %x0, midr_el1" : "=r" (reg));
        uint32_t raspi = (reg >> 4) & 0xFFF;
        switch (raspi) {
            case 0xD08:  //4B. Cortex A72
                MMIO_BASE = 0xFE000000; 
                RPI_BOARD = 4;
                GPIO_PIN_BASE = 0x50;
                #if QEMU
                SDHCI_BASE = MMIO_BASE + 0x300000;
                #else
                SDHCI_BASE = MMIO_BASE + 0x340000;//EMMC2 direct, no routing needed
                #endif
                GICD_BASE = MMIO_BASE + 0x1841000;
                GICC_BASE = MMIO_BASE + 0x1842000;
                PCI_BASE = 0xFD500000;
            break;
            case 0xD0B:  //5. Cortex A76
                MMIO_BASE = 0x107C000000UL;
                RPI_BOARD = 5;
                GICD_BASE =     MMIO_BASE + 0x3FF9000;
                GICC_BASE =     MMIO_BASE + 0x3FFA000;
                MAILBOX_BASE =  MMIO_BASE + 0x13880;
                SDHCI_BASE =    MMIO_BASE + 0xFFF000UL;
                UART0_BASE =    MMIO_BASE + 0x1001000;
                XHCI_BASE =     0x1F00300000UL;
                PCI_BASE = 0x1000120000UL;
            break;
            default:  
                RPI_BOARD = 3;
                MMIO_BASE = 0x3F000000; 
                SDHCI_BASE = MMIO_BASE + 0x300000;
                GICD_BASE = MMIO_BASE + 0xB200;
            break;
        }
        if (RPI_BOARD != 5){
            GPIO_BASE  = MMIO_BASE + 0x200000;
            MAILBOX_BASE = MMIO_BASE + 0xB880;
            UART0_BASE = MMIO_BASE + 0x201000;
            XHCI_BASE  = MMIO_BASE + 0x9C0000;
        }
        DWC2_BASE  = MMIO_BASE + 0x980000;
        RAM_START       = 0x10000000;
        CRAM_END        = (MMIO_BASE - 0x10000000) & 0xF0000000;
        RAM_START       = 0x10000000;
        CRAM_START      = 0x13600000;
        MSI_OFFSET = 0;
        LOWEST_ADDR = MMIO_BASE;
        PM_BASE = MMIO_BASE + 0x100000u;
    }
}

void hw_high_va(){
    if (UART0_BASE) UART0_BASE |= HIGH_VA;
    if (MMIO_BASE) MMIO_BASE |= HIGH_VA;
    if (BOARD_TYPE != 1 && PCI_BASE)
        PCI_BASE |= HIGH_VA;
    if (GICD_BASE) GICD_BASE |= HIGH_VA;
    if (GICC_BASE) GICC_BASE |= HIGH_VA;
    if (MAILBOX_BASE) MAILBOX_BASE |= HIGH_VA;
    if (SDHCI_BASE) SDHCI_BASE |= HIGH_VA;
    if (XHCI_BASE) XHCI_BASE |= HIGH_VA;
}

void print_hardware(){
    kprintf("Board type %i",BOARD_TYPE);
}