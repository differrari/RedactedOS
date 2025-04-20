#ifndef PCI_H
#define PCI_H

#include "types.h"

uint64_t find_pci_device(uint32_t vendor_id, uint32_t device_id);

void dump_pci_config(uint64_t base);

uint64_t pci_get_bar(uint64_t base, uint8_t offset, uint8_t index);

void debug_read_bar(uint64_t base, uint8_t offset, uint8_t index);

void inspect_bars(uint64_t base, uint8_t offset);

#endif