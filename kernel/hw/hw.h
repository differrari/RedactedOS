#pragma once

// #include "elfvirt.h"
#include "types.h"

extern uint8_t BOARD_TYPE;

extern uint8_t USE_DTB;
extern uintptr_t PCI_BASE;

extern uintptr_t RAM_START;
extern uintptr_t RAM_SIZE;
extern uintptr_t CRAM_START;
extern uintptr_t CRAM_END;

extern uintptr_t UART0_BASE;
extern uintptr_t XHCI_BASE;

extern uintptr_t MMIO_BASE;

extern uintptr_t GICD_BASE;
extern uintptr_t GICC_BASE;

extern uintptr_t SDHCI_BASE;

extern uintptr_t GPIO_BASE;

extern uintptr_t GPIO_PIN_BASE;

extern uint8_t RPI_BOARD;

extern uintptr_t MAILBOX_BASE;

extern uintptr_t DWC2_BASE;

void detect_hardware();
void print_hardware();