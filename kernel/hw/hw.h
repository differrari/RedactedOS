#pragma once

// #include "elfvirt.h"
#include "types.h"

extern uint8_t BOARD_TYPE;

extern uint8_t USE_DTB;
extern uint8_t USE_PCI;

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

extern uint8_t RPI_BOARD;

void detect_hardware();
void print_hardware();

#ifdef __cplusplus
extern "C" {
#endif
void delay(uint32_t count);
#ifdef __cplusplus
}
#endif