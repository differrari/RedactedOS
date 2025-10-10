#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t base_addr;
    uint64_t size;
} pci_device_mmio;

typedef struct {
    uint64_t addr;
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
} pci_device_info;

size_t pci_enumerate(pci_device_info* out, size_t max);
uint64_t find_pci_device(uint32_t vendor_id, uint32_t device_id);
uint64_t pci_get_bar_address(uint64_t base, uint8_t offset, uint8_t index);
uint64_t pci_setup_bar(uint64_t pci_addr, uint32_t bar_index, uint64_t *mmio_start, uint64_t *mmio_size);

void pci_enable_device(uint64_t pci_addr);
void pci_register(uint64_t mmio_addr, uint64_t mmio_size);

void pci_enable_verbose();

bool pci_setup_msi(uint64_t pci_addr, uint8_t irq_vector);

uint8_t pci_setup_interrupts(uint64_t pci_addr, uint8_t irq_line, uint8_t amount);

bool pci_setup_rp1();
bool pci_setup_msi_rp1(uint8_t irq_line, bool edge_triggered);

typedef struct {
    uint32_t addr_offset;
    uint8_t irq_num;
} msix_irq_line;

bool pci_setup_msix(uint64_t pci_addr, msix_irq_line* irq_lines, uint8_t line_size);

#ifdef __cplusplus
}
#endif