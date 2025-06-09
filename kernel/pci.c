#include "pci.h"
#include "console/kio.h"
#include "memory/kalloc.h"
#include "memory/dma.h"
#include "fw/fw_cfg.h"
#include "kstring.h"
#include "exceptions/irq.h"
#include "memory/mmu.h"
#include "memory/memory_access.h"

#define PCI_BUS_MAX 256
#define PCI_SLOT_MAX 32
#define PCI_FUNC_MAX 8

#define PCI_COMMAND_REGISTER 0x04

#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_BUS 0x4

static uint64_t pci_base;

#define NINIT pci_base == 0x0

static bool pci_verbose = false;

void pci_enable_verbose(){
    pci_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (pci_verbose){\
            uint64_t _args[] = { __VA_ARGS__ }; \
            kprintf_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
        }\
    })

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
                    for (uint32_t i = 0; i < candidate->length; i++) sum += p[i];
                    if (sum != 0) continue;
                }
                kprintf("[PCI] Found a suitable candidate");
                return candidate;
            }
        }
    }
    kprintf("[PCI] No candidate found");
    return 0x0;
}

void find_pci(){
    pci_base = 0x4010000000;
    return;
    struct fw_cfg_file file;
    if (!fw_find_file(kstring_l("etc/acpi/rsdp"), &file))
        return;

    kprintf("[PCI] rsdp file found");

    struct acpi_rsdp_t data;
    
    fw_cfg_dma_read(&data, sizeof(struct acpi_rsdp_t), file.selector);

    kprintf("[PCI] rsdp data read with address %x (%x = %x)", data.xsdt_address, data.length,sizeof(struct acpi_rsdp_t));

    kstring a = kstring_ca_max(data.signature, 8);
    if (!kstring_equals(kstring_l("RSD PTR "), a)){
        kprintf("[PCI] Signature doesn't match %s", (uint64_t)a.data);
    } else 
        kprintf("[PCI] Signature match %s", (uint64_t)a.data);

    uint8_t sum = 0;
    uint8_t* bytes = (uint8_t*)&data;
    for (uint32_t i = 0; i < 20; i++) sum += bytes[i];
    if (sum != data.checksum) {
        kprintf("[PCI] Bad RSDP checksum %i vs %i", sum, data.checksum);
    }

    if (data.revision >= 2) {
        sum = 0;
        for (uint32_t i = 0; i < data.length; i++) sum += bytes[i];
        if (sum != data.extended_checksum) {
            kprintf("[PCI] Bad extended RSDP checksum %i vs %i", sum, data.extended_checksum);
        }
    }

    struct acpi_rsdp_t *bkdata = find_rsdp();

    uint64_t xsdt_addr = (bkdata->revision >= 2) ? bkdata->xsdt_address : (uint64_t)bkdata->rsdt_address;

    kprintf("[PCI] rsdp pointer %x", xsdt_addr);
    
    struct acpi_xsdt_t xsdt_header;
    dma_read(&xsdt_header, sizeof(xsdt_header), xsdt_addr);

    kprintf("[PCI] xsdt header found");
    
    uint32_t entry_count = (xsdt_header.length - sizeof(xsdt_header)) / sizeof(uint64_t);

    kprintf("[PCI] xsdt %c entries", xsdt_header.signature[0]);

    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t entry_addr;
        dma_read(&entry_addr, sizeof(entry_addr), xsdt_addr + sizeof(xsdt_header) + i * sizeof(uint64_t));

        kprintf("[PCI] entry found");

        char sig[4];
        dma_read(&sig, 4, entry_addr);

        if (sig[0] == 'M' && sig[1] == 'C' && sig[2] == 'F' && sig[3] == 'G') {
            struct acpi_mcfg_t mcfg;
            dma_read(&mcfg, sizeof(mcfg), entry_addr);

            kprintf("[PCI] Found PCI controller base address: %x", mcfg.allocations[0].base_address);
            return;
        }
    }

    kprintf("[PCI] MCFG not found");
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

uint64_t pci_read_address_bar(uintptr_t pci_addr, uint32_t bar_index){
    uint64_t bar_addr = pci_get_bar_address(pci_addr, 0x10, bar_index);
    uint32_t original = read32(bar_addr);
    uint64_t full = original;
    if ((original & 0x6) == 0x4) {
        uint64_t bar_addr_hi = pci_get_bar_address(pci_addr, 0x10, bar_index+1);
        uint64_t original_hi = read32(bar_addr_hi);
        full |= original_hi << 32;
    }
    return full & ~0xF;
}

void debug_read_bar(uint64_t base, uint8_t offset, uint8_t index){
    uint64_t addr = pci_get_bar_address(base, offset, index);
    uint64_t val = read32(addr);
    kprintf("[PCI] Reading@%x (%i) content: ", addr, index, val);
}

uint64_t pci_setup_bar(uint64_t pci_addr, uint32_t bar_index, uint64_t *mmio_start, uint64_t *mmio_size) {
    uint64_t bar_addr = pci_get_bar_address(pci_addr, 0x10, bar_index);
    uint32_t original = read32(bar_addr);

    write32(bar_addr, 0xFFFFFFFF);
    uint32_t bar_low = read32(bar_addr);
    kprintfv("[PCI] First bar size %x",bar_low);

    uint64_t size;
    if ((original & 0x6) == 0x4) {
        uint64_t bar_addr_hi = pci_get_bar_address(pci_addr, 0x10, bar_index+1);
        uint32_t original_hi = read32(bar_addr_hi);

        kprintfv("[PCI] Original second bar %x",original_hi);

        write32(bar_addr_hi, 0xFFFFFFFF);
        uint32_t bar_high = read32(bar_addr_hi);

        kprintfv("[PCI] Second bar size %x",bar_high);

        uint64_t combined = ((uint64_t)bar_high << 32) | (bar_low & ~0xF);
        size = ~combined + 1;

        kprintfv("[PCI] Total bar size %x",size);

        uint64_t config_base = alloc_mmio_region(size);
        *mmio_start = config_base;
        *mmio_size = size;

        write32(bar_addr, config_base & 0xFFFFFFFF);
        write32(bar_addr_hi, config_base >> 32);

        uint32_t new_hi = read32(bar_addr_hi);
        uint32_t new_lo = read32(bar_addr);

        kprintfv("[PCI] Two registers %x > %x",new_hi,new_lo);
    } else {
        bar_low &= ~0xF;
        size = ~((uint64_t)bar_low) + 1;

        uint64_t config_base = alloc_mmio_region(size);
        *mmio_start = config_base;
        *mmio_size = size;

        write32(bar_addr, config_base & 0xFFFFFFFF);
    }

    write32(pci_addr + 0x04, read32(pci_addr + 0x04) | 0x2);

    return *mmio_start;
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

                    kprintf("[PCI] Found device at bus %i, slot %i, func %i", bus, slot, func);

                    return device_address;
                }// else if (((vendor_device >> 16) & 0xFFFF) != 0xFFFF)
                   //  kprintf("[PCI] VENDOR: %x DID: %x",(vendor_device & 0xFFFF),((vendor_device >> 16) & 0xFFFF));
            }
        }
    }
    kprintf("[PCI] Device not found. Vendor = %x Device = %x",vendor_id,device_id);
    return 0;
}

void dump_pci_config(uint64_t base) {
    kprintf("[PCI] Dumping PCI Configuration Space:");
    for (uint32_t offset = 0; offset < 0x40; offset += 4) {
        uint64_t val = read(base + offset);
        kprintf("Offset %x: %x",offset, val);
    }
}

void pci_enable_device(uint64_t pci_addr){
    uint32_t cmd = read16(pci_addr + PCI_COMMAND_REGISTER);
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_REGISTER;
    write16(pci_addr + PCI_COMMAND_REGISTER,cmd);
}

void pci_register(uint64_t mmio_addr, uint64_t mmio_size){
    for (uint64_t addr = mmio_addr; addr <= mmio_addr + mmio_size; addr += GRANULE_4KB)
        register_device_memory(addr,addr);
}

bool pci_setup_msi(uint64_t pci_addr, uint8_t irq_line) {
    uint8_t cap_ptr = read8(pci_addr + 0x34);
    while (cap_ptr) {
        uint8_t cap_id = read8(pci_addr + cap_ptr);
        if (cap_id == 0x05) { // MSI
            uint16_t msg_ctrl = read16(pci_addr + cap_ptr + 0x2);
            bool is_64bit = msg_ctrl & (1 << 7);

            write32(pci_addr + cap_ptr + 0x4, 0x8020040);
            int offset = 0x8;
            if (is_64bit) {
                write32(pci_addr + cap_ptr + 0x8, 0);
                offset += 4;
            }

            write16(pci_addr + cap_ptr + offset, 50 + irq_line);
            msg_ctrl |= 1; // enable MSI
            write16(pci_addr + cap_ptr + 0x2, msg_ctrl);
            return true;
        }
        cap_ptr = read8(pci_addr + cap_ptr + 1);
    }
    return false;
}

typedef struct {
    uint32_t msg_addr_low;
    uint32_t msg_addr_high;
    uint32_t msg_data;
    uint32_t vector_control;
} msix_table_entry;

bool pci_setup_msix(uint64_t pci_addr, uint8_t irq_line) {

    uint8_t cap_ptr = read8(pci_addr + 0x34);
    while (cap_ptr) {
        uint8_t cap_id = read8(pci_addr + cap_ptr);
        if (cap_id == 0x11) { // MSI-X
            uint16_t msg_ctrl = read16(pci_addr + cap_ptr + 0x2);
            msg_ctrl &= ~(1 << 15); // Clear MSI-X Enable bit
            write16(pci_addr + cap_ptr + 0x2, msg_ctrl);
            uint32_t table_offset = read32(pci_addr + cap_ptr + 0x4);
            uint8_t bir = table_offset & 0x7;
            uint32_t table_addr_offset = table_offset & ~0x7;
            
            uint64_t table_addr = pci_read_address_bar(pci_addr, bir);
            
            msix_table_entry *msix_entry = (msix_table_entry *)(uintptr_t)(table_addr + table_addr_offset);
            
            msix_entry->msg_addr_low = 0x8020040;
            msix_entry->msg_addr_high = 0;
            msix_entry->msg_data = 50 + irq_line;
            msix_entry->vector_control = 0;
            
            msg_ctrl |= (1 << 15); // MSI-X Enable
            msg_ctrl &= ~(1 << 14); // Clear Function Mask
            write16(pci_addr + cap_ptr + 0x2, msg_ctrl);

            break;
        }
        cap_ptr = read8(pci_addr + cap_ptr + 1);
    }

    return true;
}
