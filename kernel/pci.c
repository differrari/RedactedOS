#include "pci.h"
#include "console/kio.h"
#include "ram_e.h"
#include "dma.h"
#include "fw/fw_cfg.h"
#include "kstring.h"

#define PCI_BUS_MAX 256
#define PCI_SLOT_MAX 32
#define PCI_FUNC_MAX 8

#define PCI_COMMAND_REGISTER 0x04

static uint64_t pci_base;

#define NINIT pci_base == 0x0

struct acpi_rsdp_t {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    // ACPI 2.0+:
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
}__attribute__((packed));

struct acpi_xsdt_t {
    char signature[4]; // "XSDT"
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint64_t entries[]; // Array of table pointers
}__attribute__((packed));

struct acpi_mcfg_t {
    char signature[4]; // "MCFG"
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint64_t reserved;
    struct {
        uint64_t base_address; // <<<< This is what you want
        uint16_t pci_segment_group_number;
        uint8_t start_bus_number;
        uint8_t end_bus_number;
        uint32_t reserved2;
    } __attribute__((packed)) allocations[];
} __attribute__((packed));

#define RSDP_SEARCH_START 0x000E0000
#define RSDP_SEARCH_END   0x00100000

void* find_rsdp() {
    for (uint64_t addr = RSDP_SEARCH_START; addr < RSDP_SEARCH_END; addr += 16) {
        const char* sig = (const char*)(uintptr_t)addr;
        if (sig[0] == 'R' && sig[1] == 'S' && sig[2] == 'D' && sig[3] == ' ' &&
            sig[4] == 'P' && sig[5] == 'T' && sig[6] == 'R' && sig[7] == ' ') {
            
            struct acpi_rsdp_t* candidate = (struct acpi_rsdp_t*)(uintptr_t)addr;
            
            uint8_t sum = 0;
            uint8_t* p = (uint8_t*)candidate;
            for (int i = 0; i < 20; i++) sum += p[i];
            if (sum == 0) {
                if (candidate->revision >= 2) {
                    sum = 0;
                    for (int i = 0; i < candidate->length; i++) sum += p[i];
                    if (sum != 0) continue;
                }
                kprintf("Found a suitable candidate");
                return candidate;
            }
        }
    }
    kprintf("No candidate found");
    return 0x0;
}

void find_pci(){
    pci_base = 0x4010000000;
    return;
    struct fw_cfg_file file;
    if (!fw_find_file(string_l("etc/acpi/rsdp"), &file))
        return;

    kprintf("rsdp file found");

    struct acpi_rsdp_t data;
    
    fw_cfg_dma_read(&data, sizeof(struct acpi_rsdp_t), file.selector);

    kprintf("rsdp data read with address %h (%h = %h)", data.xsdt_address, data.length,sizeof(struct acpi_rsdp_t));

    kstring a = string_ca_max(data.signature, 8);
    if (!string_equals(string_l("RSD PTR "), a)){
        kprintf("Signature doesn't match %s", (uint64_t)a.data);
    } else 
        kprintf("Signature match %s", (uint64_t)a.data);

    uint8_t sum = 0;
    uint8_t* bytes = (uint8_t*)&data;
    for (uint32_t i = 0; i < 20; i++) sum += bytes[i];
    if (sum != data.checksum) {
        kprintf("Bad RSDP checksum %i vs %i", sum, data.checksum);
    }

    if (data.revision >= 2) {
        sum = 0;
        for (uint32_t i = 0; i < data.length; i++) sum += bytes[i];
        if (sum != data.extended_checksum) {
            kprintf("Bad extended RSDP checksum %i vs %i", sum, data.extended_checksum);
        }
    }

    struct acpi_rsdp_t *bkdata = find_rsdp();

    uint64_t xsdt_addr = (bkdata->revision >= 2) ? bkdata->xsdt_address : (uint64_t)bkdata->rsdt_address;

    kprintf("rsdp pointer %h", xsdt_addr);
    
    struct acpi_xsdt_t xsdt_header;
    dma_read(&xsdt_header, sizeof(xsdt_header), xsdt_addr);

    kprintf("xsdt header found");
    
    uint32_t entry_count = (xsdt_header.length - sizeof(xsdt_header)) / sizeof(uint64_t);

    kprintf("xsdt %c entries", xsdt_header.signature[0]);

    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t entry_addr;
        dma_read(&entry_addr, sizeof(entry_addr), xsdt_addr + sizeof(xsdt_header) + i * sizeof(uint64_t));

        kprintf("entry found");

        char sig[4];
        dma_read(&sig, 4, entry_addr);

        if (sig[0] == 'M' && sig[1] == 'C' && sig[2] == 'F' && sig[3] == 'G') {
            struct acpi_mcfg_t mcfg;
            dma_read(&mcfg, sizeof(mcfg), entry_addr);

            kprintf("Found PCI controller base address: %h", mcfg.allocations[0].base_address);
            return;
        }
    }

    kprintf("MCFG not found");
}

uint64_t pci_make_addr(uint32_t bus, uint32_t slot, uint32_t func, uint32_t offset){
    return pci_base
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
    kprintf("Reading@%h (%i) content: ", addr, index, val);
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

                    kprintf("Found device at bus %i, slot %i, func %i", bus, slot, func);

                    return device_address;
                }
            }
        }
    }
    kprintf("Device not found.");
    return 0;
}

void dump_pci_config(uint64_t base) {
    kprintf("Dumping PCI Configuration Space:");
    for (uint32_t offset = 0; offset < 0x40; offset += 4) {
        uint64_t val = read(base + offset);
        kprintf("Offset %h: %h",offset, val);
    }
}