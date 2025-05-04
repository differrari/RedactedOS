#ifndef PCI_H
#define PCI_H

#include "types.h"

uint64_t find_pci_device(uint32_t vendor_id, uint32_t device_id);

uint64_t pci_get_bar_address(uint64_t base, uint8_t offset, uint8_t index);

void pci_enable_device(uint64_t pci_addr);

uint64_t pci_setup_bar(uint64_t pci_addr, uint32_t bar_index, uint64_t *mmio_start, uint64_t *mmio_size);

void pci_enable_verbose();

#endif