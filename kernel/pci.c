#include "pci.h"
#include "console/kio.h"
#include "ram_e.h"
#include "fw/fw_cfg.h"

#define PCI_ECAM_BASE 0x4010000000
#define PCI_BUS_MAX 256
#define PCI_SLOT_MAX 32
#define PCI_FUNC_MAX 8

#define PCI_COMMAND_REGISTER 0x04

static uint64_t pci_base;

#define NINIT pci_base == 0x0

typedef struct {
    char signature[8];        // Expected "RSD PTR "
    uint8_t checksum;         // checksum of first 20 bytes
    char oem_id[6];           // OEM string
    uint8_t revision;         // 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t rsdt_address;    // ACPI 1.0 addr
    // ACPI 2.0+:
    uint32_t length;          // total size of RSDP (should be 36)
    uint64_t xsdt_address;    // 64-bit physical address of XSDT (new table)
    uint8_t extended_checksum;// checksum of full 36 bytes
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

void find_pci(){
    struct fw_cfg_file file;
    if (!fw_find_file(string_l("etc/acpi/rsdp"), &file))
        return;
    
    printf("Found RSDP file");
}

uint64_t pci_make_addr(uint32_t bus, uint32_t slot, uint32_t func, uint32_t offset){
    return PCI_ECAM_BASE
            | (((uint64_t)bus) << 20)
            | (((uint64_t)slot) << 15)
            | (((uint64_t)func) << 12) 
            | ((uint64_t)(offset & 0xFFF));
}

uint64_t pci_get_bar_address(uint64_t base, uint8_t offset, uint8_t index){
    if (NINIT)
        find_pci();

    return base + offset + (index * 4);
}

void debug_read_bar(uint64_t base, uint8_t offset, uint8_t index){
    uint64_t addr = pci_get_bar_address(base, offset, index);
    uint64_t val = read32(addr);
    printf("Reading@%h (%i) content: ", addr, index, val);
}


uint64_t find_pci_device(uint32_t vendor_id, uint32_t device_id) {

    if (NINIT)
        find_pci();

    for (uint32_t bus = 0; bus < PCI_BUS_MAX; bus++) {
        for (uint32_t slot = 0; slot < PCI_SLOT_MAX; slot++) {
            for (uint32_t func = 0; func < PCI_FUNC_MAX; func++) {
                uint64_t device_address = pci_make_addr(bus, slot, func, 0x00);
                uint64_t vendor_device = read(device_address);
                if ((vendor_device & 0xFFFF) == vendor_id && ((vendor_device >> 16) & 0xFFFF) == device_id) {

                    printf("Found device at bus %i, slot %i, func %i", bus, slot, func);

                    return device_address;
                }
            }
        }
    }
    printf("Device not found.");
    return 0;
}

void dump_pci_config(uint64_t base) {
    printf("Dumping PCI Configuration Space:");
    for (uint32_t offset = 0; offset < 0x40; offset += 4) {
        uint64_t val = read(base + offset);
        printf("Offset %h: %h",offset, val);
    }
}